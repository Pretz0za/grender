/**
 * Stress test: renders a W x H grid graph (default 1000x1000 = 1M vertices,
 * ~2M edges) while rewriting every vertex position every frame (traveling
 * wave), i.e. the worst case for online rendering. Prints the average frame
 * time once per second.
 *
 * Usage: millionDemo [gridW] [gridH] [screenshot.ppm]
 */

#include "grender/grender.h"

#include "ds/gvizGraph.h"
#include "ds/gvizSubgraph.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv) {
  size_t gridW = argc > 1 ? (size_t)atoi(argv[1]) : 1000;
  size_t gridH = argc > 2 ? (size_t)atoi(argv[2]) : 1000;
  const char *screenshotPath = argc > 3 ? argv[3] : NULL;
  size_t n = gridW * gridH;

  printf("building %zux%zu grid (%zu vertices)...\n", gridW, gridH, n);
  gvizGraph graph;
  gvizGraphInitAtCapacity(&graph, 0, n);
  for (size_t i = 0; i < n; i++)
    gvizGraphAddVertex(&graph, NULL, NULL, NULL);
  for (size_t y = 0; y < gridH; y++)
    for (size_t x = 0; x < gridW; x++) {
      if (x + 1 < gridW)
        gvizGraphAddEdge(&graph, y * gridW + x, y * gridW + x + 1);
      if (y + 1 < gridH)
        gvizGraphAddEdge(&graph, y * gridW + x, (y + 1) * gridW + x);
    }
  gvizGraphBuildLayout(&graph);
  gvizSubgraph sg = gvizSubgraphCreateFull(&graph);

  gvizEmbeddedGraph eg;
  if (gvizEmbeddedGraphInit(&eg, sg, 2) < 0) {
    fprintf(stderr, "embedding init failed\n");
    return 1;
  }
  for (size_t y = 0; y < gridH; y++)
    for (size_t x = 0; x < gridW; x++) {
      double *p = gvizEmbeddedGraphGetVPosition(&eg, y * gridW + x);
      p[0] = (double)x;
      p[1] = (double)y;
    }

  grRendererDesc desc;
  grRendererDescInit(&desc);
  desc.title = "grender - 1M vertices";
  desc.nodeStyle.radius = 1.0f;
  desc.edgeStyle.color = GR_COLOR(0.45f, 0.55f, 0.75f, 0.25f);
  desc.vsync = false;

  grRenderer *r = grRendererCreate(&desc);
  if (!r || grRendererSetGraph(r, &eg) < 0) {
    fprintf(stderr, "renderer setup failed\n");
    return 1;
  }

  double t = 0.0, statAccum = 0.0;
  size_t statFrames = 0, totalFrames = 0;
  while (grRendererFrame(r)) {
    double dt = grRendererDeltaTime(r);
    t += dt;
    statAccum += dt;
    statFrames++;
    totalFrames++;
    if (statAccum >= 1.0) {
      printf("avg frame: %.2f ms (%.0f fps)\n",
             1000.0 * statAccum / statFrames, statFrames / statAccum);
      statAccum = 0.0;
      statFrames = 0;
    }

    // Rewrite every position: traveling wave across the grid.
    for (size_t y = 0; y < gridH; y++)
      for (size_t x = 0; x < gridW; x++) {
        double *p = gvizEmbeddedGraphGetVPosition(&eg, y * gridW + x);
        p[1] = (double)y + 20.0 * sin(0.03 * (double)x + 2.0 * t);
      }

    if (screenshotPath && totalFrames == 120) {
      if (grRendererSaveScreenshot(r, screenshotPath) == 0)
        printf("screenshot saved to %s\n", screenshotPath);
      grRendererRequestClose(r);
    }
  }

  grRendererDestroy(r);
  gvizEmbeddedGraphRelease(&eg);
  gvizGraphRelease(&graph);
  return 0;
}
