/**
 * Live Tutte barycentric embedding demo.
 *
 * A planar graph is combinatorially embedded via Boyer-Myrvold, then relaxed by
 * gviz's Tutte embedder while grender draws every frame. The embedder registers
 * "tutte.step" and "tutte.fixOuterFace" on its embedded graph; grender
 * registers "grender.pickFace" when the graph is attached.
 *
 * Controls:
 *   R        - run one Tutte relaxation step
 *   space    - toggle continuous stepping
 *   B        - pin the highlighted face as the new outer boundary and re-embed
 *   right click - highlight the face under the cursor
 *   F        - fit view
 *   S        - toggle stats overlay
 *   drag     - pan
 *   scroll   - zoom
 *
 * Usage: tutteDemo [rows] [cols] [screenshot.ppm]
 */

#include "algorithms/search/gvizConnectedComponents.h"
#include "grender/grender.h"

#include "ds/gvizGraph.h"
#include "ds/gvizSubgraph.h"
#include "embedders/gvizTutteEmbedder.h"
#include "utils/graphs.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static void actionToggleAuto(gvizEmbeddedGraph *eg, void *userData,
                             const gvizActionPayload *payload) {
  (void)eg;
  (void)payload;
  bool *autoStep = userData;
  *autoStep = !*autoStep;
  printf("auto step: %s\n", *autoStep ? "on" : "off");
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

  printf("components=%zu largest=%zu (%.1f%% of vertices)\n", count,
         sizes[largest], 100.0 * (double)sizes[largest] / (double)n);

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
  size_t rows = argc > 1 ? (size_t)atoi(argv[1]) : 10;
  size_t cols = argc > 2 ? (size_t)atoi(argv[2]) : 10;
  const char *obj = argc > 3 ? argv[3] : NULL;
  const char *screenshotPath = argc > 4 ? argv[4] : NULL;

  if (rows < 2 || cols < 2) {
    fprintf(stderr, "rows and cols must be >= 2\n");
    return 1;
  }

  gvizGraph graph;
  if (obj) {
    gvizGraphLoadFromObjFile(obj, &graph);
  } else {
    graph = build_rect_mesh(rows, cols);
  }
  gvizGraphBuildLayout(&graph);

  gvizSubgraph sg = gvizSubgraphCreateFull(&graph);
  gvizTutteState tutte = {0};
  if (gvizTutteEmbedderInit(&tutte, sg, 2, 0) < 0) {
    fprintf(stderr, "Tutte init failed\n");
    gvizGraphRelease(&graph);
    return 1;
  }

  if (gvizTutteEmbedderBegin(&tutte) < 0) {
    fprintf(stderr, "Tutte begin failed (graph may be non-planar)\n");
    gvizTutteEmbedderRelease(&tutte);
    gvizGraphRelease(&graph);
    return 1;
  }

  gvizEmbeddedGraph *eg = (gvizEmbeddedGraph *)&tutte;
  bool autoStep = true;
  gvizEmbeddedGraphAddAction(eg, "demo.toggleAuto", actionToggleAuto,
                             &autoStep);

  grRendererDesc desc;
  grRendererDescInit(&desc);
  desc.title =
      "grender - Tutte (R: step, space: auto, right-click: pick face, B: fix)";
  desc.nodeStyle.radius = 3.0f;
  desc.nodeStyle.fillColor = GR_COLOR(0.55f, 0.78f, 1.0f, 1.0f);
  desc.edgeStyle.color = GR_COLOR(0.45f, 0.55f, 0.75f, 0.45f);
  desc.edgeStyle.width = 1.5f;

  grRenderer *r = grRendererCreate(&desc);
  if (!r) {
    fprintf(stderr, "renderer creation failed\n");
    gvizTutteEmbedderRelease(&tutte);
    gvizGraphRelease(&graph);
    return 1;
  }

  if (grRendererSetGraph(r, eg) < 0) {
    fprintf(stderr, "graph attach failed\n");
    grRendererDestroy(r);
    gvizTutteEmbedderRelease(&tutte);
    gvizGraphRelease(&graph);
    return 1;
  }

  grRendererBindKey(r, 'R', "tutte.step");
  grRendererBindKey(r, 'B', "tutte.fixOuterFace");
  grRendererBindKey(r, GR_KEY_SPACE, "demo.toggleAuto");
  grRendererBindMouse(r, GR_MOUSE_BUTTON_RIGHT, GR_ACTION_PICK_FACE);

  const size_t stepsBeforeShot = screenshotPath ? 300 : SIZE_MAX;
  size_t totalSteps = 0;

  while (grRendererFrame(r)) {
    if (autoStep && !tutte.converged) {
      double dt = grRendererDeltaTime(r);
      for (size_t i = 0; i < 20; i++) {
        gvizTutteEmbedderStep(&tutte, dt);
      }
    }

    if (screenshotPath) {
      totalSteps++;
      if (totalSteps >= stepsBeforeShot) {
        grRendererFitView(r);
        grRendererFrame(r);
        if (grRendererSaveScreenshot(r, screenshotPath) == 0)
          printf("screenshot saved to %s\n", screenshotPath);
        else
          fprintf(stderr, "screenshot failed\n");
        grRendererRequestClose(r);
      }
    }
  }

  grRendererDestroy(r);
  gvizTutteEmbedderRelease(&tutte);
  gvizGraphRelease(&graph);
  return 0;
}
