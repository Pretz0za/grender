/**
 * Stress test: renders a W x H grid graph (default 1000x1000 = 1M vertices,
 * ~2M edges) while rewriting every vertex position every frame (traveling
 * wave), i.e. the worst case for online rendering. Prints the average frame
 * time once per second. Uses a bare gviz::layout::EmbeddedGraph (no
 * algorithm) with manual GetVPosition/SetVPosition, per its "manual
 * positions" row in CLAUDE.md's embedder table.
 *
 * -G/--grow (the old streamed-grid-growth mode, one row added and committed
 * per frame via EmbeddedGraph::Sync) is REMOVED, not adapted: gviz's
 * EmbeddedGraph no longer has any dynamic-graph-growth/Sync() machinery at
 * all -- it is now a plain, GraphLike-agnostic base (vertex count + dimension
 * fixed at construction; no Structure(), no admission/commit model -- see
 * EmbeddedGraph.hpp's class doc) rather than something that owned a Subgraph
 * and grew with it. There is no replacement API for adding vertices to an
 * already-constructed EmbeddedGraph, so the flag now errors out instead of
 * silently doing something else. The traveling-wave position-rewrite stress
 * test (this file's actual point) is otherwise unaffected.
 *
 * Usage: millionDemo [gridW] [gridH] [screenshot.ppm]
 */

#include "grender/grender.h"

#include "gviz.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

static void printUsage(const char *prog) {
  fprintf(stderr, "Usage: %s [gridW] [gridH] [screenshot.ppm]\n", prog);
}

/** Appends grid row @p y (vertices, intra-row edges, edges up to row y-1)
 *  directly to @p graph. Positions are NOT set here: the embedding's slots
 *  for the new row only exist after EmbeddedGraph::Sync commits it. */
static void addGridRow(gviz::Graph &graph, size_t gridW, size_t y) {
  for (size_t x = 0; x < gridW; x++)
    graph.AddVertex();
  for (size_t x = 0; x < gridW; x++) {
    if (x + 1 < gridW)
      graph.AddEdge(y * gridW + x, y * gridW + x + 1, 1.0);
    if (y > 0)
      graph.AddEdge((y - 1) * gridW + x, y * gridW + x, 1.0);
  }
}

int main(int argc, char **argv) {
  const char *positional[3] = {NULL, NULL, NULL};
  size_t nPositional = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-G") == 0 || strcmp(argv[i], "--grow") == 0) {
      // See this file's header comment: gviz's EmbeddedGraph has no
      // dynamic-graph-growth/Sync() support anymore.
      fprintf(stderr,
              "-G/--grow is no longer supported: gviz's EmbeddedGraph has no "
              "dynamic-graph-growth/Sync() support anymore (an embedding is "
              "fixed once constructed) -- see this file's header comment\n");
      return 1;
    } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "unknown option: %s\n", argv[i]);
      printUsage(argv[0]);
      return 1;
    } else if (nPositional < 3) {
      positional[nPositional++] = argv[i];
    } else {
      printUsage(argv[0]);
      return 1;
    }
  }
  size_t gridW = positional[0] ? (size_t)strtoul(positional[0], NULL, 10) : 1000;
  size_t gridH = positional[1] ? (size_t)strtoul(positional[1], NULL, 10) : 1000;
  const char *screenshotPath = positional[2];
  if (gridW == 0 || gridH == 0) {
    fprintf(stderr, "gridW/gridH must be positive integers\n");
    printUsage(argv[0]);
    return 1;
  }
  size_t n = gridW * gridH;

  printf("building %zux%zu grid (%zu vertices)...\n", gridW, gridH, n);
  gviz::Graph graph(false, n);
  try {
    for (size_t y = 0; y < gridH; y++)
      addGridRow(graph, gridW, y);
  } catch (const std::exception &e) {
    fprintf(stderr, "graph construction failed: %s\n", e.what());
    return 1;
  }

  graph.BuildLayout();
  gviz::Subgraph sg = gviz::Subgraph::CreateFull(graph);

  // Bare EmbeddedGraph (gviz's "manual positions" row -- no algorithm, just
  // the shared base) no longer takes a Subgraph at all: it's now a plain
  // vertexCount+dimension buffer, entirely decoupled from any structure (see
  // EmbeddedGraph.hpp's class doc) -- `sg` above is kept alive separately
  // and handed to grRendererSetGraph so grender can still draw edges.
  std::optional<gviz::layout::EmbeddedGraph> eg;
  try {
    eg.emplace(graph.Size(), 2);
    for (size_t y = 0; y < gridH; y++)
      for (size_t x = 0; x < gridW; x++) {
        double *p = eg->GetVPosition(y * gridW + x);
        p[0] = (double)x;
        p[1] = (double)y;
      }
  } catch (const std::exception &e) {
    fprintf(stderr, "embedding init failed: %s\n", e.what());
    return 1;
  }

  grRendererDesc desc;
  grRendererDescInit(&desc);
  desc.title = "grender - 1M vertices";
  desc.nodeStyle.radius = 1.0f;
  desc.edgeStyle.color = GR_COLOR(0.45f, 0.55f, 0.75f, 0.25f);
  desc.vsync = false;

  grRenderer *r = grRendererCreate(&desc);
  if (!r || grRendererSetGraph(r, sg, *eg, &graph) < 0) {
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
        double *p = eg->GetVPosition(y * gridW + x);
        p[1] = (double)y + 20.0 * sin(0.03 * (double)x + 2.0 * t);
      }

    if (screenshotPath && totalFrames == 120) {
      if (grRendererSaveScreenshot(r, screenshotPath) == 0)
        printf("screenshot saved to %s\n", screenshotPath);
      grRendererRequestClose(r);
    }
  }

  grRendererDestroy(r);
  return 0;
}
