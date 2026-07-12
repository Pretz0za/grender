/**
 * GRIP embedding demo for graphs stored under the gviz data directory.
 *
 * Loads <gviz-data>/<dataset>/data.edges, keeps only the largest connected
 * component, embeds with GRIP in the requested dimension, and renders online
 * while refinement runs.
 *
 * Usage: datasetDemo <dataset> <dim(2|3|4)>
 *
 * Controls:
 *   R      - run one GRIP refinement round
 *   N      - advance to the next (finer) GRIP layer
 *   space  - toggle continuous refinement
 *   F      - fit view
 *   S      - toggle the stats overlay
 *   drag   - pan (2D) / orbit (3D)
 *   scroll - zoom
 */

#include "grender/grender.h"

#include "algorithms/search/gvizBreadthFirst.h"
#include "algorithms/search/gvizConnectedComponents.h"
#include "ds/gvizGraph.h"
#include "ds/gvizSubgraph.h"
#include "embedders/gvizGRIPEmbedder.h"
#include "utils/graphLoader.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GRENDER_GVIZ_DATA_DIR
#error "GRENDER_GVIZ_DATA_DIR must be defined by CMake"
#endif

static void actionToggleAuto(gvizEmbeddedGraph *eg, void *userData,
                             const gvizActionPayload *payload) {
  (void)eg, (void)payload;
  bool *autoRefine = userData;
  *autoRefine = !*autoRefine;
  printf("auto refine: %s\n", *autoRefine ? "on" : "off");
}

static size_t subgraphFirstVertex(const gvizSubgraph *sg) {
  gvizSubgraphVertexIterator it = gvizSubgraphVertexIteratorCreate(sg);
  size_t v = 0;
  gvizSubgraphVertexIterate(&it, &v);
  return v;
}

static size_t estimateGraphDiameter(gvizSubgraph sg) {
  const gvizGraph *g = sg.g;
  size_t n = gvizGraphSize(g);
  if (n <= 1)
    return 1;

  size_t *dist = malloc(n * sizeof(size_t));
  if (!dist)
    return n;

  size_t start = subgraphFirstVertex(&sg);
  gvizSubgraph bfs = gvizSubgraphCreateEmpty(g);
  size_t far = start;
  size_t maxDist = 0;

  if (gvizSearchBreadthFirst(&sg, &bfs, start, 0, dist) == 0) {
    gvizSubgraphVertexIterator it = gvizSubgraphVertexIteratorCreate(&sg);
    size_t v;
    while (gvizSubgraphVertexIterate(&it, &v)) {
      if (dist[v] != SIZE_MAX && dist[v] > maxDist) {
        maxDist = dist[v];
        far = v;
      }
    }
  }

  gvizSubgraphRelease(&bfs);
  bfs = gvizSubgraphCreateEmpty(g);
  maxDist = 0;
  if (gvizSearchBreadthFirst(&sg, &bfs, far, 0, dist) == 0) {
    gvizSubgraphVertexIterator it = gvizSubgraphVertexIteratorCreate(&sg);
    size_t v;
    while (gvizSubgraphVertexIterate(&it, &v)) {
      if (dist[v] != SIZE_MAX && dist[v] > maxDist)
        maxDist = dist[v];
    }
  }

  gvizSubgraphRelease(&bfs);
  free(dist);
  return maxDist > 0 ? maxDist : 1;
}

static int largestComponentSubgraph(const gvizGraph *graph, gvizSubgraph *out) {
  size_t n = gvizGraphSize(graph);
  gvizSubgraph full = gvizSubgraphCreateFull(graph);

  size_t *labels = malloc(n * sizeof(size_t));
  if (!labels) {
    gvizSubgraphRelease(&full);
    return -1;
  }

  size_t count = 0;
  if (gvizConnectedComponents(&full, labels, &count) < 0) {
    free(labels);
    gvizSubgraphRelease(&full);
    return -1;
  }
  gvizSubgraphRelease(&full);

  if (count == 0) {
    free(labels);
    return -1;
  }

  size_t *sizes = calloc(count, sizeof(size_t));
  if (!sizes) {
    free(labels);
    return -1;
  }
  gvizConnectedComponentSizes(labels, n, count, sizes);

  size_t largest = 0;
  for (size_t c = 1; c < count; c++) {
    if (sizes[c] > sizes[largest])
      largest = c;
  }

  printf("connected components: %zu (using largest: %zu vertices, "
         "ignoring %zu)\n",
         count, sizes[largest], n - sizes[largest]);

  gvizVertexSubset vs = gvizVertexSubsetCreateEmpty(graph);
  if (!vs) {
    free(labels);
    free(sizes);
    return -1;
  }

  for (size_t v = 0; v < n; v++) {
    if (labels[v] == largest)
      gvizVertexSubsetShowVertex(vs, v);
  }

  free(labels);
  free(sizes);

  *out = gvizSubgraphCreateVertexInduced(graph, vs);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <dataset> <dim(2|3|4)>\n", argv[0]);
    return 1;
  }

  const char *dataset = argv[1];
  size_t dim = (size_t)atoi(argv[2]);
  if (dim != 2 && dim != 3 && dim != 4) {
    fprintf(stderr, "dim must be 2, 3, or 4\n");
    return 1;
  }

  char path[512];
  snprintf(path, sizeof(path), "%s/%s/data.edges", GRENDER_GVIZ_DATA_DIR,
           dataset);

  gvizEdgesFileOptions opts;
  gvizEdgesFileOptionsInit(&opts);

  printf("loading %s...\n", path);
  gvizGraph graph;
  if (gvizGraphLoadFromEdgesFile(path, &opts, &graph) < 0) {
    fprintf(stderr, "failed to load graph from %s\n", path);
    return 1;
  }
  gvizGraphBuildLayout(&graph);
  printf("loaded %zu vertices, %zu edges\n", gvizGraphSize(&graph),
         gvizGraphEdgeCount(&graph));

  gvizSubgraph sg;
  if (largestComponentSubgraph(&graph, &sg) < 0) {
    fprintf(stderr, "connected component analysis failed\n");
    gvizGraphRelease(&graph);
    return 1;
  }
  printf("subgraph: %zu vertices, %zu edges\n", gvizSubgraphVertexCount(&sg),
         gvizSubgraphEdgeCount(&sg));

  printf("estimating diameter...\n");
  size_t diameter = estimateGraphDiameter(sg);
  printf("diameter estimate: %zu\n", diameter);

  gvizGRIPState grip = {0};
  if (gvizGRIPEmbedderInit(&grip, sg, diameter, dim) < 0) {
    fprintf(stderr, "GRIP init failed\n");
    gvizGRIPEmbedderRelease(&grip);
    gvizGraphRelease(&graph);
    return 1;
  }
  gvizGRIPEmbedderConfigureK(&grip, 256, 256, 256, GVIZ_GRIP_K_CONSTANT);

  printf("building MIS filtration...\n");
  gvizGRIPEmbedderBegin(&grip);
  printf("embedding (%zud)...\n", dim);

  gvizEmbeddedGraph *eg = (gvizEmbeddedGraph *)&grip;

  bool autoRefine = true;
  gvizEmbeddedGraphAddAction(eg, "demo.toggleAuto", actionToggleAuto,
                             &autoRefine);

  grRendererDesc desc;
  grRendererDescInit(&desc);
  desc.title = "grender - GRIP dataset";
  desc.nodeStyle.radius =
      gvizSubgraphVertexCount(&sg) > 100000 ? 1.0f : 2.5f;
  desc.nodeStyle.fillColor = GR_COLOR(0.55f, 0.78f, 1.0f, 1.0f);
  desc.edgeStyle.color = GR_COLOR(0.45f, 0.55f, 0.75f, 0.35f);

  grRenderer *r = grRendererCreate(&desc);
  if (!r) {
    fprintf(stderr, "renderer creation failed\n");
    gvizGRIPEmbedderRelease(&grip);
    gvizGraphRelease(&graph);
    return 1;
  }
  if (grRendererSetGraph(r, eg) < 0) {
    fprintf(stderr, "graph attach failed\n");
    grRendererDestroy(r);
    gvizGRIPEmbedderRelease(&grip);
    gvizGraphRelease(&graph);
    return 1;
  }

  grRendererBindKey(r, 'R', "grip.refineRound");
  grRendererBindKey(r, 'N', "grip.nextStage");
  grRendererBindKey(r, GR_KEY_SPACE, "demo.toggleAuto");

  const size_t roundsPerStage = SIZE_MAX;
  while (grRendererFrame(r)) {
    if (!autoRefine)
      continue;
    if (grip.currRound >= roundsPerStage) {
      if (grip.currLayer == 0)
        autoRefine = false;
      else
        beginNewStage(&grip);
    } else {
      runRefinementRound(&grip);
    }
  }

  grRendererDestroy(r);
  gvizGRIPEmbedderRelease(&grip);
  gvizGraphRelease(&graph);
  return 0;
}
