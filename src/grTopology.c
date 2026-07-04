#include "grInternal.h"

#include "ds/gvizGraph.h"
#include "ds/gvizSubgraph.h"

#include <stdlib.h>
#include <string.h>

/**
 * The single place where grender reads graph structure, exclusively through
 * the gviz public subgraph and draw-mask API. Runs when structure or draw
 * mask changes, not on pure position updates.
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

    size_t v;
    gvizSubgraphNeighborIterator nit = gvizSubgraphNeighborIteratorCreate(sg, u);
    while (gvizSubgraphNeighborIterate(&nit, &v)) {
      if (!directed && v <= u)
        continue;
      if (directed && v == u)
        continue;
      if (!gvizEmbeddedGraphIsEdgeVisible(graph, u, v))
        continue;
      if (ei == edgeCapacity)
        break;
      topo->edges[ei * 2] = (uint32_t)u;
      topo->edges[ei * 2 + 1] = (uint32_t)v;
      ei++;
    }
  }

  topo->nodeCount = ni;
  topo->edgeCount = ei;
  return 0;
}

void grTopologyRelease(grTopology *topo) {
  free(topo->nodeIds);
  free(topo->edges);
  memset(topo, 0, sizeof(*topo));
}
