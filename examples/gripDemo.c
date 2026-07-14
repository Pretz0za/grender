/**
 * Live GRIP embedding demo.
 *
 * A Möbius strip mesh is embedded by gviz's GRIP embedder while grender draws
 * every frame, demonstrating online rendering (spec 3) and creator-defined
 * actions (spec 4): the GRIP embedder registers "grip.refineRound" and
 * "grip.nextStage" on its embedded graph; this app merely binds keys to those
 * names without knowing what they do.
 *
 * Controls:
 *   R      - run one GRIP refinement round
 *   N      - advance to the next (finer) GRIP layer
 *   space  - toggle continuous refinement
 *   F      - fit view
 *   S      - toggle the stats overlay (GRIP heat/displacement charts)
 *   drag   - pan (2D) / orbit (3D)
 *   scroll - zoom
 *
 * Usage: gripDemo [rows] [cols] [dim(2|3|4)] [screenshot.ppm] [noStats]
 *
 * Pass noStats (or --no-stats) as the last argument to disable GRIP stat
 * collection at init time; grender needs no changes (zero series registered).
 * number of frames, saves the image, and exits (used for automated checks).
 */

#include "grender/grender.h"

#include "ds/gvizGraph.h"
#include "ds/gvizSubgraph.h"
#include "utils/graphs.h"
#include "embedders/gvizGRIPEmbedder.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Demo-owned action: toggles the auto-refine flag. Registered on the embedded
 *  graph like any embedder action would be. */
static void actionToggleAuto(gvizEmbeddedGraph *eg, void *userData,
                             const gvizActionPayload *payload) {
  (void)eg, (void)payload;
  bool *autoRefine = userData;
  *autoRefine = !*autoRefine;
  printf("auto refine: %s\n", *autoRefine ? "on" : "off");
}

int main(int argc, char **argv) {
  size_t rows = argc > 1 ? (size_t)atoi(argv[1]) : 24;
  size_t cols = argc > 2 ? (size_t)atoi(argv[2]) : 48;
  size_t dim = argc > 3 ? (size_t)atoi(argv[3]) : 3;
  const char *screenshotPath = NULL;
  bool gripStats = true;
  for (int i = 4; i < argc; i++) {
    if (!strcmp(argv[i], "noStats") || !strcmp(argv[i], "--no-stats"))
      gripStats = false;
    else if (!screenshotPath)
      screenshotPath = argv[i];
  }
  if (dim != 2 && dim != 3 && dim != 4) {
    fprintf(stderr, "dim must be 2, 3, or 4\n");
    return 1;
  }

  size_t depth = rows;
  // gvizGraph graph = createSierpinski(depth, NULL);
  gvizGraph graph = build_rect_mesh(rows, cols);
  gvizGraphBuildLayout(&graph);
  gvizSubgraph sg = gvizSubgraphCreateFull(&graph);

  gvizGRIPState grip = {0};
  size_t diameter = rows + cols + 64;

  if (!gripStats)
    gvizGRIPEmbedderConfigureStats(&grip, false);
  if (gvizGRIPEmbedderInit(&grip, sg, diameter, dim) < 0) {
    fprintf(stderr, "GRIP init failed\n");
    return 1;
  }
  gvizEmbeddedGraph *eg = (gvizEmbeddedGraph *)&grip;
  gvizGRIPEmbedderConfigureK(&grip, 128, 128, 128, GVIZ_GRIP_K_CONSTANT);

  gvizGRIPEmbedderBegin(&grip);

  bool autoRefine = true;
  gvizEmbeddedGraphAddAction(eg, "demo.toggleAuto", actionToggleAuto,
                             &autoRefine);

  grRendererDesc desc;
  grRendererDescInit(&desc);
  desc.title = "grender - GRIP Möbius (R: refine, N: next stage, space: auto)";
  desc.nodeStyle.radius = 7.0f;
  desc.nodeStyle.fillColor = GR_COLOR(0.55f, 0.78f, 1.0f, 1.0f);
  desc.edgeStyle.color = GR_COLOR(0.45f, 0.55f, 0.75f, 0.35f);

  grRenderer *r = grRendererCreate(&desc);
  if (!r) {
    fprintf(stderr, "renderer creation failed\n");
    return 1;
  }
  if (grRendererSetGraph(r, eg) < 0) {
    fprintf(stderr, "graph attach failed\n");
    return 1;
  }

  grRendererBindKey(r, 'R', "grip.refineRound");
  grRendererBindKey(r, 'N', "grip.nextStage");
  grRendererBindKey(r, GR_KEY_SPACE, "demo.toggleAuto");

  const size_t roundsPerStage = screenshotPath ? 150 : SIZE_MAX;
  while (grRendererFrame(r)) {
    if (autoRefine) {
      if (grip.currRound >= roundsPerStage) {
        if (screenshotPath) {
          char path[512];
          snprintf(path, sizeof(path), "%s.layer%zu.ppm", screenshotPath,
                   grip.currLayer);
          grRendererFitView(r);
          grRendererFrame(r);
          if (grRendererSaveScreenshot(r, path) == 0)
            printf("screenshot saved to %s\n", path);
          else
            fprintf(stderr, "screenshot failed\n");
        }
        if (grip.currLayer == 0)
          grRendererRequestClose(r);
        else
          beginNewStage(&grip);
      } else {
        runRefinementRound(&grip);
      }
    }
  }

  grRendererDestroy(r);
  gvizGRIPEmbedderRelease(&grip);
  gvizGraphRelease(&graph);
  return 0;
}
