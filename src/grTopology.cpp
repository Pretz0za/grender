#include "grInternal.h"

#include <stdlib.h>
#include <string.h>

/* Applies the shared per-edge filters and, when (u, v) survives them,
 * appends it to the edge buffer. Returns false only when the buffer is
 * full and iteration should stop. */
static bool topoAppendEdge(grTopology *topo, gviz::layout::EmbeddedGraph &graph,
                           bool directed, size_t u, size_t v, size_t *ei,
                           size_t edgeCapacity) {
  if (!directed && v <= u)
    return true;
  if (directed && v == u)
    return true;
  if (!graph.IsEdgeVisible(u, v))
    return true;
  if (*ei == edgeCapacity)
    return false;
  topo->edges[*ei * 2] = (uint32_t)u;
  topo->edges[*ei * 2 + 1] = (uint32_t)v;
  (*ei)++;
  return true;
}

/**
 * The single place where grender reads graph structure, exclusively through
 * the gviz public embedding/subgraph/draw-mask API. Runs when structure or
 * draw mask changes, not on pure position updates.
 *
 * Edges come from the embedding's synced adjacency snapshot
 * (EmbeddedGraph::OutNeighbors) whenever the embedding has one -- for a
 * dynamic (growing) graph, subgraph neighbor iteration proxies the parent
 * graph's LIVE adjacency and would expose edges the embedding hasn't
 * committed yet (see EmbeddedGraph.hpp's GROWTH & SYNC section), while
 * the snapshot is exactly what the layout is simulating. Embeddings that
 * have never synced (static embedders that never call EmbeddedGraph::Sync)
 * have no snapshot, and there the live subgraph IS the committed structure,
 * so neighbor iteration remains the fallback.
 */
int grTopologyExtract(grTopology *topo, gviz::layout::EmbeddedGraph &graph) {
  grTopologyRelease(topo);

  gviz::Subgraph &sg = graph.Structure();
  bool directed = sg.ParentIsDirected();

  size_t nodeCount = sg.VertexCount();
  size_t edgeCapacity = sg.EdgeCount();

  topo->nodeIds =
      (uint32_t *)malloc(sizeof(uint32_t) * (nodeCount ? nodeCount : 1));
  topo->edges = (uint32_t *)malloc(sizeof(uint32_t) * 2 *
                                   (edgeCapacity ? edgeCapacity : 1));
  if (!topo->nodeIds || !topo->edges) {
    grTopologyRelease(topo);
    return -1;
  }

  size_t ni = 0, ei = 0;
  for (size_t u : sg) {
    if (!graph.IsVertexVisible(u))
      continue;
    topo->nodeIds[ni++] = (uint32_t)u;

    std::span<const size_t> nbrs = graph.OutNeighbors(u);
    if (!nbrs.empty()) {
      for (size_t k = 0; k < nbrs.size(); k++)
        if (!topoAppendEdge(topo, graph, directed, u, nbrs[k], &ei,
                            edgeCapacity))
          break;
    } else {
      for (size_t v : sg.Neighbors(u))
        if (!topoAppendEdge(topo, graph, directed, u, v, &ei, edgeCapacity))
          break;
    }
  }

  topo->nodeCount = ni;
  topo->edgeCount = ei;
  topo->directed = directed;
  return 0;
}

void grTopologyRelease(grTopology *topo) {
  free(topo->nodeIds);
  free(topo->edges);
  memset(topo, 0, sizeof(*topo));
}
