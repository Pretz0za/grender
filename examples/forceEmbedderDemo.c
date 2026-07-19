/**
 * Live Barnes-Hut force embedding demo, using gviz's configurable
 * gvizForceEmbedder framework (LinLog or Fruchterman-Reingold, gravity,
 * Barnes-Hut approximation).
 *
 * A random spanning-tree-plus-extra-edges graph (edge connectivity
 * configurable) is laid out while grender draws every frame. The embedder
 * registers "forceEmbedder.step" on its embedded graph; this app merely
 * binds a key to that name without knowing what it does.
 *
 * Controls:
 *   R      - run one relaxation step
 *   space  - toggle continuous stepping
 *   h/l    - decrease/increase ideal edge length
 *   j/k    - decrease/increase gravity k
 *   N/M    - decrease/increase Barnes-Hut theta
 *   F      - fit view
 *   S      - toggle stats overlay
 *   drag   - pan
 *   scroll - zoom
 *
 * Usage: forceEmbedderDemo [N] [seed] [edgeConnectivity] [model] [screenshot.ppm]
 *   model: "linlog" (default) or "fr"
 */

#include "embedders/gvizEmbeddedGraph.h"
#include "grender/grender.h"

#include "ds/gvizGraph.h"
#include "ds/gvizSubgraph.h"
#include "embedders/gvizForceEmbedder.h"
#include "utils/graphs.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>

#define DEMO_GRAVITY_K_DEFAULT 0.05
#define DEMO_GRAVITY_K_STEP 0.01
#define DEMO_THETA_STEP 0.1
#define DEMO_THETA_MIN 0.1
#define DEMO_EDGE_LENGTH_STEP 0.2
#define DEMO_EDGE_LENGTH_MIN 0.2

static void actionToggleAuto(gvizEmbeddedGraph *eg, void *userData,
                             const gvizActionPayload *payload) {
  (void)eg;
  (void)payload;
  bool *autoStep = userData;
  *autoStep = !*autoStep;
  printf("auto step: %s\n", *autoStep ? "on" : "off");
}

static void actionGravityUp(gvizEmbeddedGraph *eg, void *userData,
                            const gvizActionPayload *payload) {
  (void)userData;
  (void)payload;
  gvizForceEmbedderState *state = (gvizForceEmbedderState *)eg;
  gvizForceEmbedderConfigureGravity(state, state->gravityK + DEMO_GRAVITY_K_STEP);
  printf("gravity k: %f\n", state->gravityK);
}

static void actionGravityDown(gvizEmbeddedGraph *eg, void *userData,
                              const gvizActionPayload *payload) {
  (void)userData;
  (void)payload;
  gvizForceEmbedderState *state = (gvizForceEmbedderState *)eg;
  double k = fmax(state->gravityK - DEMO_GRAVITY_K_STEP, 0.0);
  gvizForceEmbedderConfigureGravity(state, k);
  printf("gravity k: %f\n", state->gravityK);
}

static void actionEdgeLengthUp(gvizEmbeddedGraph *eg, void *userData,
                               const gvizActionPayload *payload) {
  (void)userData;
  (void)payload;
  gvizForceEmbedderState *state = (gvizForceEmbedderState *)eg;
  gvizForceEmbedderConfigure(state, state->edgeLength + DEMO_EDGE_LENGTH_STEP, 0);
  printf("edge length: %f\n", state->edgeLength);
}

static void actionEdgeLengthDown(gvizEmbeddedGraph *eg, void *userData,
                                 const gvizActionPayload *payload) {
  (void)userData;
  (void)payload;
  gvizForceEmbedderState *state = (gvizForceEmbedderState *)eg;
  double edgeLength =
      fmax(state->edgeLength - DEMO_EDGE_LENGTH_STEP, DEMO_EDGE_LENGTH_MIN);
  gvizForceEmbedderConfigure(state, edgeLength, 0);
  printf("edge length: %f\n", state->edgeLength);
}

static void actionThetaUp(gvizEmbeddedGraph *eg, void *userData,
                          const gvizActionPayload *payload) {
  (void)userData;
  (void)payload;
  gvizForceEmbedderState *state = (gvizForceEmbedderState *)eg;
  gvizForceEmbedderConfigureBarnesHut(state, state->theta + DEMO_THETA_STEP, 0);
  printf("theta: %f\n", state->theta);
}

static void actionThetaDown(gvizEmbeddedGraph *eg, void *userData,
                            const gvizActionPayload *payload) {
  (void)userData;
  (void)payload;
  gvizForceEmbedderState *state = (gvizForceEmbedderState *)eg;
  double theta = fmax(state->theta - DEMO_THETA_STEP, DEMO_THETA_MIN);
  gvizForceEmbedderConfigureBarnesHut(state, theta, 0);
  printf("theta: %f\n", state->theta);
}

static int parseModel(const char *arg, gvizForceModelKind *out) {
  if (!arg || strcasecmp(arg, "linlog") == 0) {
    *out = GVIZ_FORCE_MODEL_LINLOG;
    return 0;
  }
  if (strcasecmp(arg, "fr") == 0) {
    *out = GVIZ_FORCE_MODEL_FRUCHTERMAN_REINGOLD;
    return 0;
  }
  return -1;
}

int main(int argc, char **argv) {
  size_t N = argc > 1 ? (size_t)atoi(argv[1]) : 200;
  unsigned int seed =
      argc > 2 ? (unsigned int)(atoi(argv[2])) : (unsigned int)time(NULL);
  double edgeConnectivity = argc > 3 ? strtod(argv[3], NULL) : 0.0;
  gvizForceModelKind model;
  if (parseModel(argc > 4 ? argv[4] : NULL, &model) < 0) {
    fprintf(stderr, "unknown model \"%s\", expected \"linlog\" or \"fr\"\n",
            argv[4]);
    return 1;
  }
  const char *screenshotPath = argc > 5 ? argv[5] : NULL;

  if (N == 0) {
    fprintf(stderr, "N must be >= 1\n");
    return 1;
  }
  if (edgeConnectivity < 0.0 || edgeConnectivity > 1.0) {
    fprintf(stderr, "edge connectivity must be in [0, 1]\n");
    return 1;
  }

  gvizGraph graph = build_random_connected_graph(N, edgeConnectivity, seed);
  gvizGraphBuildLayout(&graph);
  gvizSubgraph sg = gvizSubgraphCreateFull(&graph);

  gvizForceEmbedderState fe = {0};
  if (gvizForceEmbedderInit(&fe, sg, 2, model) < 0) {
    fprintf(stderr, "force embedder init failed\n");
    gvizGraphRelease(&graph);
    return 1;
  }
  gvizForceEmbedderSetBarnesHutEnabled(&fe, 1);
  gvizForceEmbedderConfigureGravity(&fe, DEMO_GRAVITY_K_DEFAULT);
  gvizForceEmbedderBegin(&fe, seed);

  gvizEmbeddedGraph *eg = (gvizEmbeddedGraph *)&fe;
  bool autoStep = false;
  gvizEmbeddedGraphAddAction(eg, "demo.toggleAuto", actionToggleAuto,
                             &autoStep);
  gvizEmbeddedGraphAddAction(eg, "demo.gravityUp", actionGravityUp, NULL);
  gvizEmbeddedGraphAddAction(eg, "demo.gravityDown", actionGravityDown, NULL);
  gvizEmbeddedGraphAddAction(eg, "demo.edgeLengthUp", actionEdgeLengthUp, NULL);
  gvizEmbeddedGraphAddAction(eg, "demo.edgeLengthDown", actionEdgeLengthDown,
                             NULL);
  gvizEmbeddedGraphAddAction(eg, "demo.thetaUp", actionThetaUp, NULL);
  gvizEmbeddedGraphAddAction(eg, "demo.thetaDown", actionThetaDown, NULL);

  grRendererDesc desc;
  grRendererDescInit(&desc);
  desc.title =
      "grender - Force Embedder (R: step, space: auto, h/l: edge len, j/k: gravity, N/M: theta)";
  desc.nodeStyle.radius = 4.0f;
  desc.nodeStyle.fillColor = GR_COLOR(0.55f, 0.78f, 1.0f, 1.0f);
  desc.edgeStyle.color = GR_COLOR(0.45f, 0.55f, 0.75f, 0.45f);
  desc.edgeStyle.width = 1.5f;

  grRenderer *r = grRendererCreate(&desc);
  if (!r) {
    fprintf(stderr, "renderer creation failed\n");
    gvizForceEmbedderRelease(&fe);
    gvizGraphRelease(&graph);
    return 1;
  }

  if (grRendererSetGraph(r, eg) < 0) {
    fprintf(stderr, "graph attach failed\n");
    grRendererDestroy(r);
    gvizForceEmbedderRelease(&fe);
    gvizGraphRelease(&graph);
    return 1;
  }

  grRendererBindKey(r, 'R', "forceEmbedder.step");
  grRendererBindKey(r, GR_KEY_SPACE, "demo.toggleAuto");
  grRendererBindKey(r, 'L', "demo.edgeLengthUp");
  grRendererBindKey(r, 'H', "demo.edgeLengthDown");
  grRendererBindKey(r, 'K', "demo.gravityUp");
  grRendererBindKey(r, 'J', "demo.gravityDown");
  grRendererBindKey(r, 'M', "demo.thetaUp");
  grRendererBindKey(r, 'N', "demo.thetaDown");
  grRendererFitView(r);

  const size_t stepsBeforeShot = screenshotPath ? 300 : SIZE_MAX;
  size_t totalSteps = 0;

  while (grRendererFrame(r)) {
    if (autoStep) {
      for (size_t i = 0; i < 1; i++)
        gvizForceEmbedderStep(&fe);
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
  gvizForceEmbedderRelease(&fe);
  gvizGraphRelease(&graph);
  return 0;
}
