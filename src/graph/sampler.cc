/*!
 *  Copyright (c) 2018 by Contributors
 * \file graph/sampler.cc
 * \brief DGL sampler implementation
 */
#include <dgl/sampler.h>
#include <dgl/immutable_graph.h>
#include <dgl/runtime/container.h>
#include <dgl/packed_func_ext.h>
#include <dgl/random.h>
#include <dmlc/omp.h>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <numeric>
#include "../c_api_common.h"
#include "../array/common.h"  // for ATEN_FLOAT_TYPE_SWITCH

using namespace dgl::runtime;

namespace dgl {

namespace {
/*
 * ArrayHeap is used to sample elements from vector
 */
template<typename ValueType>
class ArrayHeap {
 public:
  explicit ArrayHeap(const std::vector<ValueType>& prob) {
    vec_size_ = prob.size();
    bit_len_ = ceil(log2(vec_size_));
    limit_ = 1 << bit_len_;
    // allocate twice the size
    heap_.resize(limit_ << 1, 0);
    // allocate the leaves
    for (int i = limit_; i < vec_size_+limit_; ++i) {
      heap_[i] = prob[i-limit_];
    }
    // iterate up the tree (this is O(m))
    for (int i = bit_len_-1; i >= 0; --i) {
      for (int j = (1 << i); j < (1 << (i + 1)); ++j) {
        heap_[j] = heap_[j << 1] + heap_[(j << 1) + 1];
      }
    }
  }
  ~ArrayHeap() {}

  /*
   * Remove term from index (this costs O(log m) steps)
   */
  void Delete(size_t index) {
    size_t i = index + limit_;
    ValueType w = heap_[i];
    for (int j = bit_len_; j >= 0; --j) {
      heap_[i] -= w;
      i = i >> 1;
    }
  }

  /*
   * Add value w to index (this costs O(log m) steps)
   */
  void Add(size_t index, ValueType w) {
    size_t i = index + limit_;
    for (int j = bit_len_; j >= 0; --j) {
      heap_[i] += w;
      i = i >> 1;
    }
  }

  /*
   * Sample from arrayHeap
   */
  size_t Sample() {
    ValueType xi = heap_[1] * RandomEngine::ThreadLocal()->Uniform<float>();
    int i = 1;
    while (i < limit_) {
      i = i << 1;
      if (xi >= heap_[i]) {
        xi -= heap_[i];
        i += 1;
      }
    }
    return i - limit_;
  }

  /*
   * Sample a vector by given the size n
   */
  void SampleWithoutReplacement(size_t n, std::vector<size_t>* samples) {
    // sample n elements
    for (size_t i = 0; i < n; ++i) {
      samples->at(i) = this->Sample();
      this->Delete(samples->at(i));
    }
  }

 private:
  int vec_size_;  // sample size
  int bit_len_;   // bit size
  int limit_;
  std::vector<ValueType> heap_;
};

/*
 * Uniformly sample integers from [0, set_size) without replacement.
 */
void RandomSample(size_t set_size, size_t num, std::vector<size_t>* out) {
  std::unordered_set<size_t> sampled_idxs;
  while (sampled_idxs.size() < num) {
    sampled_idxs.insert(RandomEngine::ThreadLocal()->RandInt(set_size));
  }
  out->clear();
  out->insert(out->end(), sampled_idxs.begin(), sampled_idxs.end());
}

/*
 * For a sparse array whose non-zeros are represented by nz_idxs,
 * negate the sparse array and outputs the non-zeros in the negated array.
 */
void NegateArray(const std::vector<size_t> &nz_idxs,
                 size_t arr_size,
                 std::vector<size_t>* out) {
  // nz_idxs must have been sorted.
  auto it = nz_idxs.begin();
  size_t i = 0;
  CHECK_GT(arr_size, nz_idxs.back());
  for (; i < arr_size && it != nz_idxs.end(); i++) {
    if (*it == i) {
      it++;
      continue;
    }
    out->push_back(i);
  }
  for (; i < arr_size; i++) {
    out->push_back(i);
  }
}

/*
 * Uniform sample vertices from a list of vertices.
 */
void GetUniformSample(const dgl_id_t* edge_id_list,
                      const dgl_id_t* vid_list,
                      const size_t ver_len,
                      const size_t max_num_neighbor,
                      std::vector<dgl_id_t>* out_ver,
                      std::vector<dgl_id_t>* out_edge) {
  // Copy vid_list to output
  if (ver_len <= max_num_neighbor) {
    out_ver->insert(out_ver->end(), vid_list, vid_list + ver_len);
    out_edge->insert(out_edge->end(), edge_id_list, edge_id_list + ver_len);
    return;
  }
  // If we just sample a small number of elements from a large neighbor list.
  std::vector<size_t> sorted_idxs;
  if (ver_len > max_num_neighbor * 2) {
    sorted_idxs.reserve(max_num_neighbor);
    RandomSample(ver_len, max_num_neighbor, &sorted_idxs);
    std::sort(sorted_idxs.begin(), sorted_idxs.end());
  } else {
    std::vector<size_t> negate;
    negate.reserve(ver_len - max_num_neighbor);
    RandomSample(ver_len, ver_len - max_num_neighbor, &negate);
    std::sort(negate.begin(), negate.end());
    NegateArray(negate, ver_len, &sorted_idxs);
  }
  // verify the result.
  CHECK_EQ(sorted_idxs.size(), max_num_neighbor);
  for (size_t i = 1; i < sorted_idxs.size(); i++) {
    CHECK_GT(sorted_idxs[i], sorted_idxs[i - 1]);
  }
  for (auto idx : sorted_idxs) {
    out_ver->push_back(vid_list[idx]);
    out_edge->push_back(edge_id_list[idx]);
  }
}

/*
 * Non-uniform sample via ArrayHeap
 *
 * \param probability Transition probability on the entire graph, indexed by edge ID
 */
template<typename ValueType>
void GetNonUniformSample(const ValueType* probability,
                         const dgl_id_t* edge_id_list,
                         const dgl_id_t* vid_list,
                         const size_t ver_len,
                         const size_t max_num_neighbor,
                         std::vector<dgl_id_t>* out_ver,
                         std::vector<dgl_id_t>* out_edge) {
  // Copy vid_list to output
  if (ver_len <= max_num_neighbor) {
    out_ver->insert(out_ver->end(), vid_list, vid_list + ver_len);
    out_edge->insert(out_edge->end(), edge_id_list, edge_id_list + ver_len);
    return;
  }
  // Make sample
  std::vector<size_t> sp_index(max_num_neighbor);
  std::vector<ValueType> sp_prob(ver_len);
  for (size_t i = 0; i < ver_len; ++i) {
    sp_prob[i] = probability[edge_id_list[i]];
  }
  ArrayHeap<ValueType> arrayHeap(sp_prob);
  arrayHeap.SampleWithoutReplacement(max_num_neighbor, &sp_index);
  out_ver->resize(max_num_neighbor);
  out_edge->resize(max_num_neighbor);
  for (size_t i = 0; i < max_num_neighbor; ++i) {
    size_t idx = sp_index[i];
    out_ver->at(i) = vid_list[idx];
    out_edge->at(i) = edge_id_list[idx];
  }
  sort(out_ver->begin(), out_ver->end());
  sort(out_edge->begin(), out_edge->end());
}

/*
 * Used for subgraph sampling
 */
struct neigh_list {
  std::vector<dgl_id_t> neighs;
  std::vector<dgl_id_t> edges;
  neigh_list(const std::vector<dgl_id_t> &_neighs,
             const std::vector<dgl_id_t> &_edges)
    : neighs(_neighs), edges(_edges) {}
};

struct neighbor_info {
  dgl_id_t id;
  size_t pos;
  size_t num_edges;

  neighbor_info(dgl_id_t id, size_t pos, size_t num_edges) {
    this->id = id;
    this->pos = pos;
    this->num_edges = num_edges;
  }
};

NodeFlow ConstructNodeFlow(std::vector<dgl_id_t> neighbor_list,
                           std::vector<dgl_id_t> edge_list,
                           std::vector<size_t> layer_offsets,
                           std::vector<std::pair<dgl_id_t, int> > *sub_vers,
                           std::vector<neighbor_info> *neigh_pos,
                           const std::string &edge_type,
                           int64_t num_edges, int num_hops, bool is_multigraph) {
  NodeFlow nf = NodeFlow::Create();
  uint64_t num_vertices = sub_vers->size();
  nf->node_mapping = aten::NewIdArray(num_vertices);
  nf->edge_mapping = aten::NewIdArray(num_edges);
  nf->layer_offsets = aten::NewIdArray(num_hops + 1);
  nf->flow_offsets = aten::NewIdArray(num_hops);

  dgl_id_t *node_map_data = static_cast<dgl_id_t *>(nf->node_mapping->data);
  dgl_id_t *layer_off_data = static_cast<dgl_id_t *>(nf->layer_offsets->data);
  dgl_id_t *flow_off_data = static_cast<dgl_id_t *>(nf->flow_offsets->data);
  dgl_id_t *edge_map_data = static_cast<dgl_id_t *>(nf->edge_mapping->data);

  // Construct sub_csr_graph
  // TODO(minjie): is nodeflow a multigraph?
  auto subg_csr = CSRPtr(new CSR(num_vertices, num_edges, is_multigraph));
  dgl_id_t* indptr_out = static_cast<dgl_id_t*>(subg_csr->indptr()->data);
  dgl_id_t* col_list_out = static_cast<dgl_id_t*>(subg_csr->indices()->data);
  dgl_id_t* eid_out = static_cast<dgl_id_t*>(subg_csr->edge_ids()->data);
  size_t collected_nedges = 0;

  // The data from the previous steps:
  // * node data: sub_vers (vid, layer), neigh_pos,
  // * edge data: neighbor_list, edge_list, probability.
  // * layer_offsets: the offset in sub_vers.
  dgl_id_t ver_id = 0;
  std::vector<std::unordered_map<dgl_id_t, dgl_id_t>> layer_ver_maps;
  layer_ver_maps.resize(num_hops);
  size_t out_node_idx = 0;
  for (int layer_id = num_hops - 1; layer_id >= 0; layer_id--) {
    // We sort the vertices in a layer so that we don't need to sort the neighbor Ids
    // after remap to a subgraph. However, we don't need to sort the first layer
    // because we want the order of the nodes in the first layer is the same as
    // the input seed nodes.
    if (layer_id > 0) {
      std::sort(sub_vers->begin() + layer_offsets[layer_id],
                sub_vers->begin() + layer_offsets[layer_id + 1],
                [](const std::pair<dgl_id_t, dgl_id_t> &a1,
                   const std::pair<dgl_id_t, dgl_id_t> &a2) {
        return a1.first < a2.first;
      });
    }

    // Save the sampled vertices and its layer Id.
    for (size_t i = layer_offsets[layer_id]; i < layer_offsets[layer_id + 1]; i++) {
      node_map_data[out_node_idx++] = sub_vers->at(i).first;
      layer_ver_maps[layer_id].insert(std::pair<dgl_id_t, dgl_id_t>(sub_vers->at(i).first,
                                                                    ver_id++));
      CHECK_EQ(sub_vers->at(i).second, layer_id);
    }
  }
  CHECK(out_node_idx == num_vertices);

  // sampling algorithms have to start from the seed nodes, so the seed nodes are
  // in the first layer and the input nodes are in the last layer.
  // When we expose the sampled graph to a Python user, we say the input nodes
  // are in the first layer and the seed nodes are in the last layer.
  // Thus, when we copy sampled results to a CSR, we need to reverse the order of layers.
  std::fill(indptr_out, indptr_out + num_vertices + 1, 0);
  size_t row_idx = layer_offsets[num_hops] - layer_offsets[num_hops - 1];
  layer_off_data[0] = 0;
  layer_off_data[1] = layer_offsets[num_hops] - layer_offsets[num_hops - 1];
  int out_layer_idx = 1;
  for (int layer_id = num_hops - 2; layer_id >= 0; layer_id--) {
    // Because we don't sort the vertices in the first layer above, we can't sort
    // the neighbor positions of the vertices in the first layer either.
    if (layer_id > 0) {
      std::sort(neigh_pos->begin() + layer_offsets[layer_id],
                neigh_pos->begin() + layer_offsets[layer_id + 1],
                [](const neighbor_info &a1, const neighbor_info &a2) {
                  return a1.id < a2.id;
                });
    }

    for (size_t i = layer_offsets[layer_id]; i < layer_offsets[layer_id + 1]; i++) {
      dgl_id_t dst_id = sub_vers->at(i).first;
      CHECK_EQ(dst_id, neigh_pos->at(i).id);
      size_t pos = neigh_pos->at(i).pos;
      CHECK_LE(pos, neighbor_list.size());
      const size_t nedges = neigh_pos->at(i).num_edges;
      if (neighbor_list.empty()) CHECK_EQ(nedges, 0);

      // We need to map the Ids of the neighbors to the subgraph.
      auto neigh_it = neighbor_list.begin() + pos;
      for (size_t i = 0; i < nedges; i++) {
        dgl_id_t neigh = *(neigh_it + i);
        CHECK(layer_ver_maps[layer_id + 1].find(neigh) != layer_ver_maps[layer_id + 1].end());
        col_list_out[collected_nedges + i] = layer_ver_maps[layer_id + 1][neigh];
      }
      // We can simply copy the edge Ids.
      std::copy_n(edge_list.begin() + pos,
                  nedges, edge_map_data + collected_nedges);
      collected_nedges += nedges;
      indptr_out[row_idx+1] = indptr_out[row_idx] + nedges;
      row_idx++;
    }
    layer_off_data[out_layer_idx + 1] = layer_off_data[out_layer_idx]
        + layer_offsets[layer_id + 1] - layer_offsets[layer_id];
    out_layer_idx++;
  }
  CHECK_EQ(row_idx, num_vertices);
  CHECK_EQ(indptr_out[row_idx], num_edges);
  CHECK_EQ(out_layer_idx, num_hops);
  CHECK_EQ(layer_off_data[out_layer_idx], num_vertices);

  // Copy flow offsets.
  flow_off_data[0] = 0;
  int out_flow_idx = 0;
  for (size_t i = 0; i < layer_offsets.size() - 2; i++) {
    size_t num_edges = indptr_out[layer_off_data[i + 2]] - indptr_out[layer_off_data[i + 1]];
    flow_off_data[out_flow_idx + 1] = flow_off_data[out_flow_idx] + num_edges;
    out_flow_idx++;
  }
  CHECK(out_flow_idx == num_hops - 1);
  CHECK(flow_off_data[num_hops - 1] == static_cast<uint64_t>(num_edges));

  std::iota(eid_out, eid_out + num_edges, 0);

  if (edge_type == std::string("in")) {
    nf->graph = GraphPtr(new ImmutableGraph(subg_csr, nullptr));
  } else {
    nf->graph = GraphPtr(new ImmutableGraph(nullptr, subg_csr));
  }

  return nf;
}

template<typename ValueType>
NodeFlow SampleSubgraph(const ImmutableGraph *graph,
                        const std::vector<dgl_id_t>& seeds,
                        const ValueType* probability,
                        const std::string &edge_type,
                        int num_hops,
                        size_t num_neighbor,
                        const bool add_self_loop) {
  CHECK_EQ(graph->NumBits(), 64) << "32 bit graph is not supported yet";
  const size_t num_seeds = seeds.size();
  auto orig_csr = edge_type == "in" ? graph->GetInCSR() : graph->GetOutCSR();
  const dgl_id_t* val_list = static_cast<dgl_id_t*>(orig_csr->edge_ids()->data);
  const dgl_id_t* col_list = static_cast<dgl_id_t*>(orig_csr->indices()->data);
  const dgl_id_t* indptr = static_cast<dgl_id_t*>(orig_csr->indptr()->data);

  std::unordered_set<dgl_id_t> sub_ver_map;  // The vertex Ids in a layer.
  std::vector<std::pair<dgl_id_t, int> > sub_vers;
  sub_vers.reserve(num_seeds * 10);
  // add seed vertices
  for (size_t i = 0; i < num_seeds; ++i) {
    auto ret = sub_ver_map.insert(seeds[i]);
    // If the vertex is inserted successfully.
    if (ret.second) {
      sub_vers.emplace_back(seeds[i], 0);
    }
  }
  std::vector<dgl_id_t> tmp_sampled_src_list;
  std::vector<dgl_id_t> tmp_sampled_edge_list;
  // ver_id, position
  std::vector<neighbor_info> neigh_pos;
  neigh_pos.reserve(num_seeds);
  std::vector<dgl_id_t> neighbor_list;
  std::vector<dgl_id_t> edge_list;
  std::vector<size_t> layer_offsets(num_hops + 1);
  int64_t num_edges = 0;

  layer_offsets[0] = 0;
  layer_offsets[1] = sub_vers.size();
  for (int layer_id = 1; layer_id < num_hops; layer_id++) {
    // We need to avoid resampling the same node in a layer, but we allow a node
    // to be resampled in multiple layers. We use `sub_ver_map` to keep track of
    // sampled nodes in a layer, and clear it when entering a new layer.
    sub_ver_map.clear();
    // Previous iteration collects all nodes in sub_vers, which are collected
    // in the previous layer. sub_vers is used both as a node collection and a queue.
    for (size_t idx = layer_offsets[layer_id - 1]; idx < layer_offsets[layer_id]; idx++) {
      dgl_id_t dst_id = sub_vers[idx].first;
      const int cur_node_level = sub_vers[idx].second;

      tmp_sampled_src_list.clear();
      tmp_sampled_edge_list.clear();
      dgl_id_t ver_len = *(indptr+dst_id+1) - *(indptr+dst_id);
      if (probability == nullptr) {  // uniform-sample
        GetUniformSample(val_list + *(indptr + dst_id),
                         col_list + *(indptr + dst_id),
                         ver_len,
                         num_neighbor,
                         &tmp_sampled_src_list,
                         &tmp_sampled_edge_list);
      } else {  // non-uniform-sample
        GetNonUniformSample(probability,
                            val_list + *(indptr + dst_id),
                            col_list + *(indptr + dst_id),
                            ver_len,
                            num_neighbor,
                            &tmp_sampled_src_list,
                            &tmp_sampled_edge_list);
      }
      // If we need to add self loop and it doesn't exist in the sampled neighbor list.
      if (add_self_loop && std::find(tmp_sampled_src_list.begin(), tmp_sampled_src_list.end(),
                                     dst_id) == tmp_sampled_src_list.end()) {
        tmp_sampled_src_list.push_back(dst_id);
        const dgl_id_t *src_list = col_list + *(indptr + dst_id);
        const dgl_id_t *eid_list = val_list + *(indptr + dst_id);
        // TODO(zhengda) this operation has O(N) complexity. It can be pretty slow.
        const dgl_id_t *src = std::find(src_list, src_list + ver_len, dst_id);
        // If there doesn't exist a self loop in the graph.
        // we have to add -1 as the edge id for the self-loop edge.
        if (src == src_list + ver_len)
          tmp_sampled_edge_list.push_back(-1);
        else
          tmp_sampled_edge_list.push_back(eid_list[src - src_list]);
      }
      CHECK_EQ(tmp_sampled_src_list.size(), tmp_sampled_edge_list.size());
      neigh_pos.emplace_back(dst_id, neighbor_list.size(), tmp_sampled_src_list.size());
      // Then push the vertices
      for (size_t i = 0; i < tmp_sampled_src_list.size(); ++i) {
        neighbor_list.push_back(tmp_sampled_src_list[i]);
      }
      // Finally we push the edge list
      for (size_t i = 0; i < tmp_sampled_edge_list.size(); ++i) {
        edge_list.push_back(tmp_sampled_edge_list[i]);
      }
      num_edges += tmp_sampled_src_list.size();
      for (size_t i = 0; i < tmp_sampled_src_list.size(); ++i) {
        // We need to add the neighbor in the hashtable here. This ensures that
        // the vertex in the queue is unique. If we see a vertex before, we don't
        // need to add it to the queue again.
        auto ret = sub_ver_map.insert(tmp_sampled_src_list[i]);
        // If the sampled neighbor is inserted to the map successfully.
        if (ret.second) {
          sub_vers.emplace_back(tmp_sampled_src_list[i], cur_node_level + 1);
        }
      }
    }
    layer_offsets[layer_id + 1] = layer_offsets[layer_id] + sub_ver_map.size();
    CHECK_EQ(layer_offsets[layer_id + 1], sub_vers.size());
  }

  return ConstructNodeFlow(neighbor_list, edge_list, layer_offsets, &sub_vers, &neigh_pos,
                           edge_type, num_edges, num_hops, graph->IsMultigraph());
}

}  // namespace

DGL_REGISTER_GLOBAL("nodeflow._CAPI_NodeFlowGetGraph")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    NodeFlow nflow = args[0];
    *rv = nflow->graph;
  });

DGL_REGISTER_GLOBAL("nodeflow._CAPI_NodeFlowGetNodeMapping")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    NodeFlow nflow = args[0];
    *rv = nflow->node_mapping;
  });

DGL_REGISTER_GLOBAL("nodeflow._CAPI_NodeFlowGetEdgeMapping")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    NodeFlow nflow = args[0];
    *rv = nflow->edge_mapping;
  });

DGL_REGISTER_GLOBAL("nodeflow._CAPI_NodeFlowGetLayerOffsets")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    NodeFlow nflow = args[0];
    *rv = nflow->layer_offsets;
  });

DGL_REGISTER_GLOBAL("nodeflow._CAPI_NodeFlowGetBlockOffsets")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    NodeFlow nflow = args[0];
    *rv = nflow->flow_offsets;
  });

template<typename ValueType>
NodeFlow SamplerOp::NeighborSample(const ImmutableGraph *graph,
                                   const std::vector<dgl_id_t>& seeds,
                                   const std::string &edge_type,
                                   int num_hops, int expand_factor,
                                   const bool add_self_loop,
                                   const ValueType *probability) {
  return SampleSubgraph(graph,
                        seeds,
                        probability,
                        edge_type,
                        num_hops + 1,
                        expand_factor,
                        add_self_loop);
}

namespace {
  void ConstructLayers(const dgl_id_t *indptr,
                       const dgl_id_t *indices,
                       const std::vector<dgl_id_t>& seed_array,
                       IdArray layer_sizes,
                       std::vector<dgl_id_t> *layer_offsets,
                       std::vector<dgl_id_t> *node_mapping,
                       std::vector<int64_t> *actl_layer_sizes,
                       std::vector<float> *probabilities) {
    /*
     * Given a graph and a collection of seed nodes, this function constructs NodeFlow
     * layers via uniform layer-wise sampling, and return the resultant layers and their
     * corresponding probabilities.
     */
    std::copy(seed_array.begin(), seed_array.end(), std::back_inserter(*node_mapping));
    actl_layer_sizes->push_back(node_mapping->size());
    probabilities->insert(probabilities->end(), node_mapping->size(), 1);
    const int64_t* layer_sizes_data = static_cast<int64_t*>(layer_sizes->data);
    const int64_t num_layers = layer_sizes->shape[0];

    size_t curr = 0;
    size_t next = node_mapping->size();
    for (int64_t i = num_layers - 1; i >= 0; --i) {
      const int64_t layer_size = layer_sizes_data[i];
      std::unordered_set<dgl_id_t> candidate_set;
      for (auto j = curr; j != next; ++j) {
        auto src = (*node_mapping)[j];
        candidate_set.insert(indices + indptr[src], indices + indptr[src + 1]);
      }

      std::vector<dgl_id_t> candidate_vector;
      std::copy(candidate_set.begin(), candidate_set.end(),
                std::back_inserter(candidate_vector));

      std::unordered_map<dgl_id_t, size_t> n_occurrences;
      auto n_candidates = candidate_vector.size();
      for (int64_t j = 0; j != layer_size; ++j) {
        auto dst = candidate_vector[
          RandomEngine::ThreadLocal()->RandInt(n_candidates)];
        if (!n_occurrences.insert(std::make_pair(dst, 1)).second) {
          ++n_occurrences[dst];
        }
      }

      for (auto const &pair : n_occurrences) {
        node_mapping->push_back(pair.first);
        float p = pair.second * n_candidates / static_cast<float>(layer_size);
        probabilities->push_back(p);
      }

      actl_layer_sizes->push_back(node_mapping->size() - next);
      curr = next;
      next = node_mapping->size();
    }
    std::reverse(node_mapping->begin(), node_mapping->end());
    std::reverse(actl_layer_sizes->begin(), actl_layer_sizes->end());
    layer_offsets->push_back(0);
    for (const auto &size : *actl_layer_sizes) {
      layer_offsets->push_back(size + layer_offsets->back());
    }
  }

  void ConstructFlows(const dgl_id_t *indptr,
                      const dgl_id_t *indices,
                      const dgl_id_t *eids,
                      const std::vector<dgl_id_t> &node_mapping,
                      const std::vector<int64_t> &actl_layer_sizes,
                      std::vector<dgl_id_t> *sub_indptr,
                      std::vector<dgl_id_t> *sub_indices,
                      std::vector<dgl_id_t> *sub_eids,
                      std::vector<dgl_id_t> *flow_offsets,
                      std::vector<dgl_id_t> *edge_mapping) {
    /*
     * Given a graph and a sequence of NodeFlow layers, this function constructs dense
     * subgraphs (flows) between consecutive layers.
     */
    auto n_flows = actl_layer_sizes.size() - 1;
    for (int64_t i = 0; i < actl_layer_sizes.front() + 1; i++)
      sub_indptr->push_back(0);
    flow_offsets->push_back(0);
    int64_t first = 0;
    for (size_t i = 0; i < n_flows; ++i) {
      auto src_size = actl_layer_sizes[i];
      std::unordered_map<dgl_id_t, dgl_id_t> source_map;
      for (int64_t j = 0; j < src_size; ++j) {
        source_map.insert(std::make_pair(node_mapping[first + j], first + j));
      }
      auto dst_size = actl_layer_sizes[i + 1];
      for (int64_t j = 0; j < dst_size; ++j) {
        auto dst = node_mapping[first + src_size + j];
        typedef std::pair<dgl_id_t, dgl_id_t> id_pair;
        std::vector<id_pair> neighbor_indices;
        for (dgl_id_t k = indptr[dst]; k < indptr[dst + 1]; ++k) {
          // TODO(gaiyu): accelerate hash table lookup
          auto ret = source_map.find(indices[k]);
          if (ret != source_map.end()) {
            neighbor_indices.push_back(std::make_pair(ret->second, eids[k]));
          }
        }
        auto cmp = [](const id_pair p, const id_pair q)->bool { return p.first < q.first; };
        std::sort(neighbor_indices.begin(), neighbor_indices.end(), cmp);
        for (const auto &pair : neighbor_indices) {
          sub_indices->push_back(pair.first);
          edge_mapping->push_back(pair.second);
        }
        sub_indptr->push_back(sub_indices->size());
      }
      flow_offsets->push_back(sub_indices->size());
      first += src_size;
    }
    sub_eids->resize(sub_indices->size());
    std::iota(sub_eids->begin(), sub_eids->end(), 0);
  }
}  // namespace

NodeFlow SamplerOp::LayerUniformSample(const ImmutableGraph *graph,
                                       const std::vector<dgl_id_t>& seeds,
                                       const std::string &neighbor_type,
                                       IdArray layer_sizes) {
  const auto g_csr = neighbor_type == "in" ? graph->GetInCSR() : graph->GetOutCSR();
  const dgl_id_t *indptr = static_cast<dgl_id_t*>(g_csr->indptr()->data);
  const dgl_id_t *indices = static_cast<dgl_id_t*>(g_csr->indices()->data);
  const dgl_id_t *eids = static_cast<dgl_id_t*>(g_csr->edge_ids()->data);

  std::vector<dgl_id_t> layer_offsets;
  std::vector<dgl_id_t> node_mapping;
  std::vector<int64_t> actl_layer_sizes;
  std::vector<float> probabilities;
  ConstructLayers(indptr,
                  indices,
                  seeds,
                  layer_sizes,
                  &layer_offsets,
                  &node_mapping,
                  &actl_layer_sizes,
                  &probabilities);

  std::vector<dgl_id_t> sub_indptr, sub_indices, sub_edge_ids;
  std::vector<dgl_id_t> flow_offsets;
  std::vector<dgl_id_t> edge_mapping;
  ConstructFlows(indptr,
                 indices,
                 eids,
                 node_mapping,
                 actl_layer_sizes,
                 &sub_indptr,
                 &sub_indices,
                 &sub_edge_ids,
                 &flow_offsets,
                 &edge_mapping);
  // sanity check
  CHECK_GT(sub_indptr.size(), 0);
  CHECK_EQ(sub_indptr[0], 0);
  CHECK_EQ(sub_indptr.back(), sub_indices.size());
  CHECK_EQ(sub_indices.size(), sub_edge_ids.size());

  NodeFlow nf = NodeFlow::Create();
  auto sub_csr = CSRPtr(new CSR(aten::VecToIdArray(sub_indptr),
                                aten::VecToIdArray(sub_indices),
                                aten::VecToIdArray(sub_edge_ids)));

  if (neighbor_type == std::string("in")) {
    nf->graph = GraphPtr(new ImmutableGraph(sub_csr, nullptr));
  } else {
    nf->graph = GraphPtr(new ImmutableGraph(nullptr, sub_csr));
  }

  nf->node_mapping = aten::VecToIdArray(node_mapping);
  nf->edge_mapping = aten::VecToIdArray(edge_mapping);
  nf->layer_offsets = aten::VecToIdArray(layer_offsets);
  nf->flow_offsets = aten::VecToIdArray(flow_offsets);

  return nf;
}

void BuildCsr(const ImmutableGraph &g, const std::string neigh_type) {
  if (neigh_type == "in") {
    auto csr = g.GetInCSR();
    assert(csr);
  } else if (neigh_type == "out") {
    auto csr = g.GetOutCSR();
    assert(csr);
  } else {
    LOG(FATAL) << "We don't support sample from neighbor type " << neigh_type;
  }
}

template<typename ValueType>
std::vector<NodeFlow> NeighborSamplingImpl(const ImmutableGraphPtr gptr,
                                           const IdArray seed_nodes,
                                           const int64_t batch_start_id,
                                           const int64_t batch_size,
                                           const int64_t max_num_workers,
                                           const int64_t expand_factor,
                                           const int64_t num_hops,
                                           const std::string neigh_type,
                                           const bool add_self_loop,
                                           const ValueType *probability) {
    // process args
    CHECK(IsValidIdArray(seed_nodes));
    const dgl_id_t* seed_nodes_data = static_cast<dgl_id_t*>(seed_nodes->data);
    const int64_t num_seeds = seed_nodes->shape[0];
    const int64_t num_workers = std::min(max_num_workers,
        (num_seeds + batch_size - 1) / batch_size - batch_start_id);
    // We need to make sure we have the right CSR before we enter parallel sampling.
    BuildCsr(*gptr, neigh_type);
    // generate node flows
    std::vector<NodeFlow> nflows(num_workers);
#pragma omp parallel for
    for (int i = 0; i < num_workers; i++) {
      // create per-worker seed nodes.
      const int64_t start = (batch_start_id + i) * batch_size;
      const int64_t end = std::min(start + batch_size, num_seeds);
      // TODO(minjie): the vector allocation/copy is unnecessary
      std::vector<dgl_id_t> worker_seeds(end - start);
      std::copy(seed_nodes_data + start, seed_nodes_data + end,
                worker_seeds.begin());
      nflows[i] = SamplerOp::NeighborSample(
          gptr.get(), worker_seeds, neigh_type, num_hops, expand_factor,
          add_self_loop, probability);
    }
    return nflows;
}

DGL_REGISTER_GLOBAL("sampling._CAPI_UniformSampling")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    // arguments
    const GraphRef g = args[0];
    const IdArray seed_nodes = args[1];
    const int64_t batch_start_id = args[2];
    const int64_t batch_size = args[3];
    const int64_t max_num_workers = args[4];
    const int64_t expand_factor = args[5];
    const int64_t num_hops = args[6];
    const std::string neigh_type = args[7];
    const bool add_self_loop = args[8];

    auto gptr = std::dynamic_pointer_cast<ImmutableGraph>(g.sptr());
    CHECK(gptr) << "sampling isn't implemented in mutable graph";

    std::vector<NodeFlow> nflows = NeighborSamplingImpl<float>(
        gptr, seed_nodes, batch_start_id, batch_size, max_num_workers,
        expand_factor, num_hops, neigh_type, add_self_loop, nullptr);

    *rv = List<NodeFlow>(nflows);
  });

DGL_REGISTER_GLOBAL("sampling._CAPI_NeighborSampling")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    // arguments
    const GraphRef g = args[0];
    const IdArray seed_nodes = args[1];
    const int64_t batch_start_id = args[2];
    const int64_t batch_size = args[3];
    const int64_t max_num_workers = args[4];
    const int64_t expand_factor = args[5];
    const int64_t num_hops = args[6];
    const std::string neigh_type = args[7];
    const bool add_self_loop = args[8];
    const NDArray probability = args[9];

    auto gptr = std::dynamic_pointer_cast<ImmutableGraph>(g.sptr());
    CHECK(gptr) << "sampling isn't implemented in mutable graph";

    std::vector<NodeFlow> nflows;

    CHECK(probability->dtype.code == kDLFloat)
      << "transition probability must be float";
    CHECK(probability->ndim == 1)
      << "transition probability must be a 1-dimensional vector";

    ATEN_FLOAT_TYPE_SWITCH(
      probability->dtype,
      FloatType,
      "transition probability",
      {
        const FloatType *prob;

        if (probability->ndim == 1 && probability->shape[0] == 0) {
          prob = nullptr;
        } else {
          CHECK(probability->shape[0] == gptr->NumEdges())
            << "transition probability must have same number of elements as edges";
          CHECK(probability.IsContiguous())
            << "transition probability must be contiguous tensor";
          prob = static_cast<const FloatType *>(probability->data);
        }

        nflows = NeighborSamplingImpl(
            gptr, seed_nodes, batch_start_id, batch_size, max_num_workers,
            expand_factor, num_hops, neigh_type, add_self_loop, prob);
    });

    *rv = List<NodeFlow>(nflows);
  });

DGL_REGISTER_GLOBAL("sampling._CAPI_LayerSampling")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    // arguments
    GraphRef g = args[0];
    const IdArray seed_nodes = args[1];
    const int64_t batch_start_id = args[2];
    const int64_t batch_size = args[3];
    const int64_t max_num_workers = args[4];
    const IdArray layer_sizes = args[5];
    const std::string neigh_type = args[6];
    // process args
    auto gptr = std::dynamic_pointer_cast<ImmutableGraph>(g.sptr());
    CHECK(gptr) << "sampling isn't implemented in mutable graph";
    CHECK(IsValidIdArray(seed_nodes));
    const dgl_id_t* seed_nodes_data = static_cast<dgl_id_t*>(seed_nodes->data);
    const int64_t num_seeds = seed_nodes->shape[0];
    const int64_t num_workers = std::min(max_num_workers,
        (num_seeds + batch_size - 1) / batch_size - batch_start_id);
    // We need to make sure we have the right CSR before we enter parallel sampling.
    BuildCsr(*gptr, neigh_type);
    // generate node flows
    std::vector<NodeFlow> nflows(num_workers);
#pragma omp parallel for
    for (int i = 0; i < num_workers; i++) {
      // create per-worker seed nodes.
      const int64_t start = (batch_start_id + i) * batch_size;
      const int64_t end = std::min(start + batch_size, num_seeds);
      // TODO(minjie): the vector allocation/copy is unnecessary
      std::vector<dgl_id_t> worker_seeds(end - start);
      std::copy(seed_nodes_data + start, seed_nodes_data + end,
                worker_seeds.begin());
      nflows[i] = SamplerOp::LayerUniformSample(
          gptr.get(), worker_seeds, neigh_type, layer_sizes);
    }
    *rv = List<NodeFlow>(nflows);
  });

}  // namespace dgl
