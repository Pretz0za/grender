/**
 * Live Fruchterman-Reingold (pairwise) embedding demo.
 *
 * A small depth-2 Sierpinski triangle is laid out with gviz's brute-force
 * pairwise FR embedder while grender draws every frame. The embedder
 * registers "frPairwise.step" on its embedded graph; this app merely binds
 * a key to that name without knowing what it does.
 *
 * Controls:
 *   R      - run one FR relaxation step
 *   space  - toggle continuous stepping
 *   F      - fit view
 *   S      - toggle stats overlay
 *   drag   - pan
 *   scroll - zoom
 *
 * Usage: frPairwiseDemo [depth] [screenshot.ppm]
 */

#include "grender/grender.h"

#include "ds/gvizGraph.h"
#include "ds/gvizSubgraph.h"
#include "embedders/gvizFRPairwiseEmbedder.h"
#include "utils/graphs.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void actionToggleAuto(gvizEmbeddedGraph *eg, void *userData,
                             const gvizActionPayload *payload) {
  (void)eg;
  (void)payload;
  bool *autoStep = userData;
  *autoStep = !*autoStep;
  printf("auto step: %s\n", *autoStep ? "on" : "off");
}

int main(int argc, char **argv) {
  int N = argc > 1 ? atoi(argv[1]) : 2;
  double density = argc > 2 ? strtod(argv[2], NULL) : 0.25;
  unsigned int seed = argc > 3 ? (unsigned int)(atoi(argv[3])) : (unsigned int)time(NULL);
  const char *screenshotPath = argc > 4 ? argv[4] : NULL;

  // if (depth < 0) {
  //   fprintf(stderr, "depth must be >= 0\n");
  //   return 1;
  // }
  //
  if (density > 1.0 || density < 0.0) {
     fprintf(stderr, "density must be in [0, 1]\n");
     return 1;
  }

  gvizGraph graph = build_random_connected_graph(N, density, seed);
  gvizGraphBuildLayout(&graph);
  gvizSubgraph sg = gvizSubgraphCreateFull(&graph);

  gvizFRPairwiseState fr = {0};
  if (gvizFRPairwiseEmbedderInit(&fr, sg, 2) < 0) {
    fprintf(stderr, "FR pairwise init failed\n");
    gvizGraphRelease(&graph);
    return 1;
  }
  gvizFRPairwiseEmbedderBegin(&fr, 0);

  gvizEmbeddedGraph *eg = (gvizEmbeddedGraph *)&fr;
  bool autoStep = false;
  gvizEmbeddedGraphAddAction(eg, "demo.toggleAuto", actionToggleAuto,
                             &autoStep);

  grRendererDesc desc;
  grRendererDescInit(&desc);
  desc.title = "grender - FR Pairwise Sierpinski (R: step, space: auto)";
  desc.nodeStyle.radius = 4.0f;
  desc.nodeStyle.fillColor = GR_COLOR(0.55f, 0.78f, 1.0f, 1.0f);
  desc.edgeStyle.color = GR_COLOR(0.45f, 0.55f, 0.75f, 0.45f);
  desc.edgeStyle.width = 1.5f;

  grRenderer *r = grRendererCreate(&desc);
  if (!r) {
    fprintf(stderr, "renderer creation failed\n");
    gvizFRPairwiseEmbedderRelease(&fr);
    gvizGraphRelease(&graph);
    return 1;
  }

  if (grRendererSetGraph(r, eg) < 0) {
    fprintf(stderr, "graph attach failed\n");
    grRendererDestroy(r);
    gvizFRPairwiseEmbedderRelease(&fr);
    gvizGraphRelease(&graph);
    return 1;
  }

  grRendererBindKey(r, 'R', "frPairwise.step");
  grRendererBindKey(r, GR_KEY_SPACE, "demo.toggleAuto");
  grRendererFitView(r);

  const size_t stepsBeforeShot = screenshotPath ? 300 : SIZE_MAX;
  size_t totalSteps = 0;

  while (grRendererFrame(r)) {
    if (autoStep) {
      for (size_t i = 0; i < 1; i++)
        gvizFRPairwiseEmbedderStep(&fr);
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
  gvizFRPairwiseEmbedderRelease(&fr);
  gvizGraphRelease(&graph);
  return 0;
}
