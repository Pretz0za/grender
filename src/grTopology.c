#include "grInternal.h"

#include "ds/gvizGraph.h"
#include "ds/gvizSubgraph.h"

#include <stdlib.h>
#include <string.h>

/* Applies the shared per-edge filters and, when (u, v) survives them,
 * appends it to the edge buffer. Returns false only when the buffer is
 * full and iteration should stop. */
static bool topoAppendEdge(grTopology *topo, gvizEmbeddedGraph *graph,
                           bool directed, size_t u, size_t v, size_t *ei,
                           size_t edgeCapacity) {
  if (!directed && v <= u)
    return true;
  if (directed && v == u)
    return true;
  if (!gvizEmbeddedGraphIsEdgeVisible(graph, u, v))
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
 * (gvizEmbeddedGraphOutNeighbors) whenever the embedding has one -- for a
 * dynamic (growing) graph, subgraph neighbor iteration proxies the parent
 * graph's LIVE adjacency and would expose edges the embedding hasn't
 * committed yet (see gvizEmbeddedGraph.h's GROWTH & SYNC section), while
 * the snapshot is exactly what the layout is simulating. Embeddings that
 * have never synced (static embedders that never call
 * gvizEmbeddedGraphSync) have no snapshot, and there the live subgraph IS
 * the committed structure, so neighbor iteration remains the fallback.
 */
int grTopologyExtract(grTopology *topo, gvizEmbeddedGraph *graph) {
  grTopologyRelease(topo);

  const gvizSubgraph *sg = gvizEmbeddedGraphStructure(graph);
  bool directed = gvizGraphIsDirected(sg->g) != 0;

  size_t nodeCount = gvizSubgraphVertexCount(sg);
  size_t edgeCapacity = gvizSubgraphEdgeCount(sg);

  topo->nodeIds = malloc(sizeof(uint32_t) * (nodeCount ? nodeCount : 1));
  topo->edges = malloc(sizeof(uint32_t) * 2 * (edgeCapacity ? edgeCapacity : 1));
  if (!topo->nodeIds || !topo->edges) {
    grTopologyRelease(topo);
    return -1;
  }

  size_t ni = 0, ei = 0;
  size_t u;
  gvizSubgraphVertexIterator vit = gvizSubgraphVertexIteratorCreate(sg);
  while (gvizSubgraphVertexIterate(&vit, &u)) {
    if (!gvizEmbeddedGraphIsVertexVisible(graph, u))
      continue;
    topo->nodeIds[ni++] = (uint32_t)u;

    size_t nbrCount;
    const size_t *nbrs = gvizEmbeddedGraphOutNeighbors(graph, u, &nbrCount);
    if (nbrs) {
      for (size_t k = 0; k < nbrCount; k++)
        if (!topoAppendEdge(topo, graph, directed, u, nbrs[k], &ei,
                            edgeCapacity))
          break;
    } else {
      size_t v;
      gvizSubgraphNeighborIterator nit =
          gvizSubgraphNeighborIteratorCreate(sg, u);
      while (gvizSubgraphNeighborIterate(&nit, &v))
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
