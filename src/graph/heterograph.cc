/*!
 *  Copyright (c) 2019 by Contributors
 * \file graph/heterograph.cc
 * \brief Heterograph implementation
 */
#include "./heterograph.h"
#include <dgl/packed_func_ext.h>
#include <dgl/runtime/container.h>
#include "../c_api_common.h"
#include "./bipartite.h"

using namespace dgl::runtime;

namespace dgl {
namespace {

HeteroSubgraph EdgeSubgraphPreserveNodes(
    const HeteroGraph* hg, const std::vector<IdArray>& eids) {
  CHECK_EQ(eids.size(), hg->NumEdgeTypes())
    << "Invalid input: the input list size must be the same as the number of edge type.";
  HeteroSubgraph ret;
  ret.induced_vertices.resize(hg->NumVertexTypes());
  ret.induced_edges = eids;
  // When preserve_nodes is true, simply compute EdgeSubgraph for each bipartite
  std::vector<HeteroGraphPtr> subrels(hg->NumEdgeTypes());
  for (dgl_type_t etype = 0; etype < hg->NumEdgeTypes(); ++etype) {
    auto pair = hg->meta_graph()->FindEdge(etype);
    const dgl_type_t src_vtype = pair.first;
    const dgl_type_t dst_vtype = pair.second;
    const auto& rel_vsg = hg->GetRelationGraph(etype)->EdgeSubgraph(
        {eids[etype]}, true);
    subrels[etype] = rel_vsg.graph;
    ret.induced_vertices[src_vtype] = rel_vsg.induced_vertices[0];
    ret.induced_vertices[dst_vtype] = rel_vsg.induced_vertices[1];
  }
  ret.graph = HeteroGraphPtr(new HeteroGraph(hg->meta_graph(), subrels));
  return ret;
}

HeteroSubgraph EdgeSubgraphNoPreserveNodes(
    const HeteroGraph* hg, const std::vector<IdArray>& eids) {
  CHECK_EQ(eids.size(), hg->NumEdgeTypes())
    << "Invalid input: the input list size must be the same as the number of edge type.";
  HeteroSubgraph ret;
  ret.induced_vertices.resize(hg->NumVertexTypes());
  ret.induced_edges = eids;
  // NOTE(minjie): EdgeSubgraph when preserve_nodes is false is quite complicated in
  // heterograph. This is because we need to make sure bipartite graphs that incident
  // on the same vertex type must have the same ID space. For example, suppose we have
  // following heterograph:
  //
  // Meta graph: A -> B -> C
  // Bipartite graphs:
  // * A -> B: (0, 0), (0, 1)
  // * B -> C: (1, 0), (1, 1)
  //
  // Suppose for A->B, we only keep edge (0, 0), while for B->C we only keep (1, 0). We need
  // to make sure that in the result subgraph, node type B still has two nodes. This means
  // we cannot simply compute EdgeSubgraph for B->C which will relabel node#1 of type B to be
  // node #0.
  //
  // One implementation is as follows:
  // (1) For each bipartite graph, slice out the edges using the given eids.
  // (2) Make a dictionary map<vtype, vector<IdArray>>, where the key is the vertex type
  //     and the value is the incident nodes from the bipartite graphs that has the vertex
  //     type as either srctype or dsttype.
  // (3) Then for each vertex type, use aten::Relabel_ on its vector<IdArray>.
  //     aten::Relabel_ computes the union of the vertex sets and relabel
  //     the unique elements from zero. The returned mapping array is the final induced
  //     vertex set for that vertex type.
  // (4) Use the relabeled edges to construct the bipartite graph.
  // step (1) & (2)
  std::vector<EdgeArray> subedges(hg->NumEdgeTypes());
  std::vector<std::vector<IdArray>> vtype2incnodes(hg->NumVertexTypes());
  for (dgl_type_t etype = 0; etype < hg->NumEdgeTypes(); ++etype) {
    auto pair = hg->meta_graph()->FindEdge(etype);
    const dgl_type_t src_vtype = pair.first;
    const dgl_type_t dst_vtype = pair.second;
    auto earray = hg->GetRelationGraph(etype)->FindEdges(0, eids[etype]);
    vtype2incnodes[src_vtype].push_back(earray.src);
    vtype2incnodes[dst_vtype].push_back(earray.dst);
    subedges[etype] = earray;
  }
  // step (3)
  for (dgl_type_t vtype = 0; vtype < hg->NumVertexTypes(); ++vtype) {
    ret.induced_vertices[vtype] = aten::Relabel_(vtype2incnodes[vtype]);
  }
  // step (4)
  std::vector<HeteroGraphPtr> subrels(hg->NumEdgeTypes());
  for (dgl_type_t etype = 0; etype < hg->NumEdgeTypes(); ++etype) {
    auto pair = hg->meta_graph()->FindEdge(etype);
    const dgl_type_t src_vtype = pair.first;
    const dgl_type_t dst_vtype = pair.second;
    subrels[etype] = Bipartite::CreateFromCOO(
      ret.induced_vertices[src_vtype]->shape[0],
      ret.induced_vertices[dst_vtype]->shape[0],
      subedges[etype].src,
      subedges[etype].dst);
  }
  ret.graph = HeteroGraphPtr(new HeteroGraph(hg->meta_graph(), subrels));
  return ret;
}

}  // namespace

HeteroGraph::HeteroGraph(GraphPtr meta_graph, const std::vector<HeteroGraphPtr>& rel_graphs)
  : BaseHeteroGraph(meta_graph), relation_graphs_(rel_graphs) {
  // Sanity check
  CHECK_EQ(meta_graph->NumEdges(), rel_graphs.size());
  CHECK(!rel_graphs.empty()) << "Empty heterograph is not allowed.";
  // all relation graph must be bipartite graphs
  for (const auto rg : rel_graphs) {
    CHECK_EQ(rg->NumVertexTypes(), 2) << "Each relation graph must be a bipartite graph.";
    CHECK_EQ(rg->NumEdgeTypes(), 1) << "Each relation graph must be a bipartite graph.";
  }
  // create num verts per type
  num_verts_per_type_.resize(meta_graph_->NumVertices(), -1);
  for (dgl_type_t vtype = 0; vtype < meta_graph_->NumVertices(); ++vtype) {
    for (dgl_type_t etype : meta_graph->OutEdgeVec(vtype)) {
      const auto nv = rel_graphs[etype]->NumVertices(Bipartite::kSrcVType);
      if (num_verts_per_type_[vtype] < 0) {
        num_verts_per_type_[vtype] = nv;
      } else {
        CHECK_EQ(num_verts_per_type_[vtype], nv)
          << "Mismatch number of vertices for vertex type " << vtype;
      }
    }
  }
}

bool HeteroGraph::IsMultigraph() const {
  return const_cast<HeteroGraph*>(this)->is_multigraph_.Get([this] () {
      for (const auto hg : relation_graphs_) {
        if (hg->IsMultigraph()) {
          return true;
        }
      }
      return false;
    });
}

BoolArray HeteroGraph::HasVertices(dgl_type_t vtype, IdArray vids) const {
  CHECK(IsValidIdArray(vids)) << "Invalid id array input";
  return aten::LT(vids, NumVertices(vtype));
}

HeteroSubgraph HeteroGraph::VertexSubgraph(const std::vector<IdArray>& vids) const {
  CHECK_EQ(vids.size(), NumVertexTypes())
    << "Invalid input: the input list size must be the same as the number of vertex types.";
  HeteroSubgraph ret;
  ret.induced_vertices = vids;
  ret.induced_edges.resize(NumEdgeTypes());
  std::vector<HeteroGraphPtr> subrels(NumEdgeTypes());
  for (dgl_type_t etype = 0; etype < NumEdgeTypes(); ++etype) {
    auto pair = meta_graph_->FindEdge(etype);
    const dgl_type_t src_vtype = pair.first;
    const dgl_type_t dst_vtype = pair.second;
    const auto& rel_vsg = GetRelationGraph(etype)->VertexSubgraph(
        {vids[src_vtype], vids[dst_vtype]});
    subrels[etype] = rel_vsg.graph;
    ret.induced_edges[etype] = rel_vsg.induced_edges[0];
  }
  ret.graph = HeteroGraphPtr(new HeteroGraph(meta_graph_, subrels));
  return ret;
}

HeteroSubgraph HeteroGraph::EdgeSubgraph(
    const std::vector<IdArray>& eids, bool preserve_nodes) const {
  if (preserve_nodes) {
    return EdgeSubgraphPreserveNodes(this, eids);
  } else {
    return EdgeSubgraphNoPreserveNodes(this, eids);
  }
}

// creator implementation
HeteroGraphPtr CreateBipartiteFromCOO(
    int64_t num_src, int64_t num_dst, IdArray row, IdArray col) {
  return Bipartite::CreateFromCOO(num_src, num_dst, row, col);
}

HeteroGraphPtr CreateBipartiteFromCSR(
    int64_t num_src, int64_t num_dst,
    IdArray indptr, IdArray indices, IdArray edge_ids) {
  return Bipartite::CreateFromCSR(num_src, num_dst, indptr, indices, edge_ids);
}

HeteroGraphPtr CreateHeteroGraph(
    GraphPtr meta_graph, const std::vector<HeteroGraphPtr>& rel_graphs) {
  return HeteroGraphPtr(new HeteroGraph(meta_graph, rel_graphs));
}

///////////////////////// C APIs /////////////////////////

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroCreateBipartiteFromCOO")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    int64_t num_src = args[0];
    int64_t num_dst = args[1];
    IdArray row = args[2];
    IdArray col = args[3];
    auto hgptr = CreateBipartiteFromCOO(num_src, num_dst, row, col);
    *rv = HeteroGraphRef(hgptr);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroCreateBipartiteFromCSR")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    int64_t num_src = args[0];
    int64_t num_dst = args[1];
    IdArray indptr = args[2];
    IdArray indices = args[3];
    IdArray edge_ids = args[4];
    auto hgptr = CreateBipartiteFromCSR(num_src, num_dst, indptr, indices, edge_ids);
    *rv = HeteroGraphRef(hgptr);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroCreateHeteroGraph")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    GraphRef meta_graph = args[0];
    List<HeteroGraphRef> rel_graphs = args[1];
    std::vector<HeteroGraphPtr> rel_ptrs;
    rel_ptrs.reserve(rel_graphs.size());
    for (const auto& ref : rel_graphs) {
      rel_ptrs.push_back(ref.sptr());
    }
    auto hgptr = CreateHeteroGraph(meta_graph.sptr(), rel_ptrs);
    *rv = HeteroGraphRef(hgptr);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroGetMetaGraph")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    *rv = GraphRef(hg->meta_graph());
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroGetRelationGraph")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    *rv = HeteroGraphRef(hg->GetRelationGraph(etype));
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroAddVertices")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t vtype = args[1];
    int64_t num = args[2];
    hg->AddVertices(vtype, num);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroAddEdge")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    dgl_id_t src = args[2];
    dgl_id_t dst = args[3];
    hg->AddEdge(etype, src, dst);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroAddEdges")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    IdArray src = args[2];
    IdArray dst = args[3];
    hg->AddEdges(etype, src, dst);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroClear")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    hg->Clear();
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroContext")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    *rv = hg->Context();
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroNumBits")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    *rv = hg->NumBits();
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroIsMultigraph")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    *rv = hg->IsMultigraph();
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroIsReadonly")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    *rv = hg->IsReadonly();
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroNumVertices")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t vtype = args[1];
    *rv = static_cast<int64_t>(hg->NumVertices(vtype));
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroNumEdges")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    *rv = static_cast<int64_t>(hg->NumEdges(etype));
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroHasVertex")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t vtype = args[1];
    dgl_id_t vid = args[2];
    *rv = hg->HasVertex(vtype, vid);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroHasVertices")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t vtype = args[1];
    IdArray vids = args[2];
    *rv = hg->HasVertices(vtype, vids);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroHasEdgeBetween")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    dgl_id_t src = args[2];
    dgl_id_t dst = args[3];
    *rv = hg->HasEdgeBetween(etype, src, dst);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroHasEdgesBetween")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    IdArray src = args[2];
    IdArray dst = args[3];
    *rv = hg->HasEdgesBetween(etype, src, dst);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroPredecessors")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    dgl_id_t dst = args[2];
    *rv = hg->Predecessors(etype, dst);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroSuccessors")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    dgl_id_t src = args[2];
    *rv = hg->Successors(etype, src);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroEdgeId")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    dgl_id_t src = args[2];
    dgl_id_t dst = args[3];
    *rv = hg->EdgeId(etype, src, dst);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroEdgeIds")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    IdArray src = args[2];
    IdArray dst = args[3];
    const auto& ret = hg->EdgeIds(etype, src, dst);
    *rv = ConvertEdgeArrayToPackedFunc(ret);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroFindEdges")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    IdArray eids = args[2];
    const auto& ret = hg->FindEdges(etype, eids);
    *rv = ConvertEdgeArrayToPackedFunc(ret);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroInEdges_1")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    dgl_id_t vid = args[2];
    const auto& ret = hg->InEdges(etype, vid);
    *rv = ConvertEdgeArrayToPackedFunc(ret);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroInEdges_2")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    IdArray vids = args[2];
    const auto& ret = hg->InEdges(etype, vids);
    *rv = ConvertEdgeArrayToPackedFunc(ret);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroOutEdges_1")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    dgl_id_t vid = args[2];
    const auto& ret = hg->OutEdges(etype, vid);
    *rv = ConvertEdgeArrayToPackedFunc(ret);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroOutEdges_2")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    IdArray vids = args[2];
    const auto& ret = hg->OutEdges(etype, vids);
    *rv = ConvertEdgeArrayToPackedFunc(ret);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroEdges")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    std::string order = args[2];
    const auto& ret = hg->Edges(etype, order);
    *rv = ConvertEdgeArrayToPackedFunc(ret);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroInDegree")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    dgl_id_t vid = args[2];
    *rv = static_cast<int64_t>(hg->InDegree(etype, vid));
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroInDegrees")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    IdArray vids = args[2];
    *rv = hg->InDegrees(etype, vids);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroOutDegree")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    dgl_id_t vid = args[2];
    *rv = static_cast<int64_t>(hg->OutDegree(etype, vid));
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroOutDegrees")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    IdArray vids = args[2];
    *rv = hg->OutDegrees(etype, vids);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroGetAdj")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    dgl_type_t etype = args[1];
    bool transpose = args[2];
    std::string fmt = args[3];
    *rv = ConvertNDArrayVectorToPackedFunc(
        hg->GetAdj(etype, transpose, fmt));
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroVertexSubgraph")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    List<Value> vids = args[1];
    std::vector<IdArray> vid_vec;
    vid_vec.reserve(vids.size());
    for (Value val : vids) {
      vid_vec.push_back(val->data);
    }
    std::shared_ptr<HeteroSubgraph> subg(
        new HeteroSubgraph(hg->VertexSubgraph(vid_vec)));
    *rv = HeteroSubgraphRef(subg);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroEdgeSubgraph")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroGraphRef hg = args[0];
    List<Value> eids = args[1];
    bool preserve_nodes = args[2];
    std::vector<IdArray> eid_vec;
    eid_vec.reserve(eids.size());
    for (Value val : eids) {
      eid_vec.push_back(val->data);
    }
    std::shared_ptr<HeteroSubgraph> subg(
        new HeteroSubgraph(hg->EdgeSubgraph(eid_vec, preserve_nodes)));
    *rv = HeteroSubgraphRef(subg);
  });

// HeteroSubgraph C APIs

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroSubgraphGetGraph")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroSubgraphRef subg = args[0];
    *rv = HeteroGraphRef(subg->graph);
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroSubgraphGetInducedVertices")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroSubgraphRef subg = args[0];
    List<Value> induced_verts;
    for (IdArray arr : subg->induced_vertices) {
      induced_verts.push_back(Value(MakeValue(arr)));
    }
    *rv = induced_verts;
  });

DGL_REGISTER_GLOBAL("graph_index._CAPI_DGLHeteroSubgraphGetInducedEdges")
.set_body([] (DGLArgs args, DGLRetValue* rv) {
    HeteroSubgraphRef subg = args[0];
    List<Value> induced_edges;
    for (IdArray arr : subg->induced_edges) {
      induced_edges.push_back(Value(MakeValue(arr)));
    }
    *rv = induced_edges;
  });

}  // namespace dgl
