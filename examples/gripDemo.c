/**
 * Live GRIP embedding demo.
 *
 * A rectangular mesh is embedded by gviz's GRIP embedder while grender draws
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
 * Usage: gripDemo [meshWidth] [meshHeight] [dim(2|3)] [screenshot.ppm]
 *
 * When a screenshot path is given, the demo runs the embedding for a fixed
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

// static gvizGraph buildRectMesh(size_t w, size_t h) {
//   gvizGraph g;
//   gvizGraphInitAtCapacity(&g, 0, w * h);
//   for (size_t i = 0; i < w * h; i++)
//     gvizGraphAddVertex(&g, NULL, NULL, NULL);
//   for (size_t y = 0; y < h; y++)
//     for (size_t x = 0; x < w; x++) {
//       if (x + 1 < w)
//         gvizGraphAddEdge(&g, y * w + x, y * w + x + 1);
//       if (y + 1 < h)
//         gvizGraphAddEdge(&g, y * w + x, (y + 1) * w + x);
//     }
//   return g;
// }
//
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
  // size_t meshW = argc > 1 ? (size_t)atoi(argv[1]) : 120;
  // size_t meshH = argc > 2 ? (size_t)atoi(argv[2]) : 80;
  size_t depth = argc > 1 ? (size_t)atoi(argv[1]) : 6;
  size_t dim = argc > 2 ? (size_t)atoi(argv[2]) : 2;
  const char *screenshotPath = argc > 4 ? argv[4] : NULL;
  if (dim != 2 && dim != 3) {
    fprintf(stderr, "dim must be 2 or 3\n");
    return 1;
  }

  // gvizGraph graph = buildRectMesh(meshW, meshH);
  // gvizGraph graph = build_sierpinski_carpet(depth);
  gvizGraph graph = createSierpinskiTetrahedron(depth, NULL);
  gvizGraphBuildLayout(&graph);
  gvizSubgraph sg = gvizSubgraphCreateFull(&graph);

  gvizGRIPState grip;
  // size_t diameter = (meshW - 1) + (meshH - 1);
  size_t diameter = pow(3, depth) + 64;
  
  if (gvizGRIPEmbedderInit(&grip, sg, diameter, dim) < 0) {
    fprintf(stderr, "GRIP init failed\n");
    return 1;
  }
  gvizEmbeddedGraph *eg = (gvizEmbeddedGraph *)&grip;
  gvizGRIPEmbedderConfigureK(&grip, 64, 32, 64, GVIZ_GRIP_K_PLACEMENT_DECAY);


  gvizGRIPEmbedderBegin(&grip);

  bool autoRefine = true;
  gvizEmbeddedGraphAddAction(eg, "demo.toggleAuto", actionToggleAuto,
                             &autoRefine);

  grRendererDesc desc;
  grRendererDescInit(&desc);
  desc.title = "grender - GRIP (R: refine, N: next stage, space: auto)";
  desc.nodeStyle.radius = 2.5f;
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

  const size_t roundsPerStage = SIZE_MAX;
  size_t frameCount = 0;
  while (grRendererFrame(r)) {
    // Drive the embedder from the frame loop; the renderer re-reads positions
    // automatically, no notification needed for position changes.
    if (autoRefine) {
      if (grip.currRound >= roundsPerStage && grip.currLayer > 0)
        beginNewStage(&grip);
      else if (grip.currRound < roundsPerStage)
        runRefinementRound(&grip);
    }

    if (screenshotPath && ++frameCount == 240) {
      grRendererFitView(r);       // reframe before capturing
      grRendererFrame(r);         // let the fit apply
      if (grRendererSaveScreenshot(r, screenshotPath) == 0)
        printf("screenshot saved to %s\n", screenshotPath);
      else
        fprintf(stderr, "screenshot failed\n");
      grRendererRequestClose(r);
    }
  }

  grRendererDestroy(r);
  gvizGRIPEmbedderRelease(&grip);
  gvizGraphRelease(&graph);
  return 0;
}
