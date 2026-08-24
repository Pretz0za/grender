#include "grInternal.h"

#include <stdlib.h>
#include <string.h>

/* Applies the shared per-edge filters and, when (u, v) survives them,
 * appends it to the edge buffer. Both @p u and @p v are LOCAL indices.
 * Returns false only when the buffer is full and iteration should stop. */
static bool topoAppendEdge(grTopology *topo, gviz::layout::EmbeddedGraph &graph,
                           bool directed, size_t u, size_t v, size_t *ei,
                           size_t edgeCapacity) {
  if (!directed && v <= u)
    return true;
  if (directed && v == u)
    return true;
  if (!graph.IsEdgeVisible(u, v, true))
    return true;
  if (*ei == edgeCapacity)
    return false;
  topo->edges[*ei * 2] = (uint32_t)u;
  topo->edges[*ei * 2 + 1] = (uint32_t)v;
  (*ei)++;
  return true;
}

/**
 * The single place where grender reads graph structure -- exclusively
 * through @p structure's gviz::GraphLike-derived type erasure (see
 * grGraphStructure in grInternal.h) and @p embedding's draw mask. Runs when
 * structure or draw mask changes, not on pure position updates.
 *
 * Builds the full adjacency CSR (outOffsets/outNbrs, and inOffsets/inNbrs
 * for a directed structure) over every LOCAL vertex first -- grender's own
 * substitute for the synced adjacency snapshot gviz::layout::EmbeddedGraph
 * used to maintain before this refactor (OutNeighbors/InNeighbors, removed
 * along with Sync() -- see EmbeddedGraph.hpp) -- then derives the
 * draw-mask-filtered node/edge instance buffers from it.
 */
int grTopologyExtract(grTopology *topo, grGraphStructure &structure,
                      gviz::layout::EmbeddedGraph &embedding) {
  grTopologyRelease(topo);

  size_t n = structure.VertexCount();
  bool directed = structure.IsDirected();
  topo->directed = directed;
  topo->vertexCount = n;

  // ---- full out-adjacency CSR, every local vertex ----
  try {
    topo->outOffsets.resize(n + 1, 0);
    std::vector<uint32_t> tmp;
    for (size_t u = 0; u < n; u++) {
      tmp.clear();
      structure.NeighborsLocal(u, tmp);
      topo->outOffsets[u + 1] = topo->outOffsets[u] + (uint32_t)tmp.size();
      topo->outNbrs.insert(topo->outNbrs.end(), tmp.begin(), tmp.end());
    }

    if (directed) {
      std::vector<uint32_t> inCounts(n, 0);
      for (uint32_t v : topo->outNbrs)
        inCounts[v]++;
      topo->inOffsets.resize(n + 1, 0);
      for (size_t u = 0; u < n; u++)
        topo->inOffsets[u + 1] = topo->inOffsets[u] + inCounts[u];
      topo->inNbrs.resize(topo->outNbrs.size());
      std::vector<uint32_t> cursor(topo->inOffsets.begin(),
                                   topo->inOffsets.end() - 1);
      for (size_t u = 0; u < n; u++) {
        for (uint32_t k = topo->outOffsets[u]; k < topo->outOffsets[u + 1]; k++) {
          uint32_t v = topo->outNbrs[k];
          topo->inNbrs[cursor[v]++] = (uint32_t)u;
        }
      }
    }
  } catch (const std::bad_alloc &) {
    grTopologyRelease(topo);
    return -1;
  }

  // ---- visible node/edge instance buffers ----
  size_t edgeCapacity = topo->outNbrs.size();
  topo->nodeIds = (uint32_t *)malloc(sizeof(uint32_t) * (n ? n : 1));
  topo->edges = (uint32_t *)malloc(sizeof(uint32_t) * 2 *
                                   (edgeCapacity ? edgeCapacity : 1));
  if (!topo->nodeIds || !topo->edges) {
    grTopologyRelease(topo);
    return -1;
  }

  size_t ni = 0, ei = 0;
  for (size_t u = 0; u < n; u++) {
    if (!embedding.IsVertexVisible(u))
      continue;
    topo->nodeIds[ni++] = (uint32_t)u;

    for (uint32_t k = topo->outOffsets[u]; k < topo->outOffsets[u + 1]; k++) {
      if (!topoAppendEdge(topo, embedding, directed, u, topo->outNbrs[k], &ei,
                          edgeCapacity))
        break;
    }
  }

  topo->nodeCount = ni;
  topo->edgeCount = ei;
  return 0;
}

void grTopologyRelease(grTopology *topo) {
  free(topo->nodeIds);
  free(topo->edges);
  topo->outOffsets.clear();
  topo->outNbrs.clear();
  topo->inOffsets.clear();
  topo->inNbrs.clear();
  topo->nodeIds = nullptr;
  topo->edges = nullptr;
  topo->nodeCount = 0;
  topo->edgeCount = 0;
  topo->directed = false;
  topo->vertexCount = 0;
}

uint32_t grTopologyVertexDegree(const grTopology *topo, size_t v) {
  if (v + 1 >= topo->outOffsets.size())
    return 0;
  uint32_t deg = topo->outOffsets[v + 1] - topo->outOffsets[v];
  if (topo->directed && v + 1 < topo->inOffsets.size())
    deg += topo->inOffsets[v + 1] - topo->inOffsets[v];
  return deg;
}
