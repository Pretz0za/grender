/**
 * Stress test: renders a W x H grid graph (default 1000x1000 = 1M vertices,
 * ~2M edges) while rewriting every vertex position every frame (traveling
 * wave), i.e. the worst case for online rendering. Prints the average frame
 * time once per second.
 *
 * With -G/--grow the grid is streamed instead of prebuilt: the graph starts
 * as a single row and one full row of vertices + edges is added and
 * committed per frame via EmbeddedGraph::Sync -- no embedder involved, so
 * this is the pure embedding-level commit path (subgraph admission,
 * position-buffer growth, synced-CSR rebuild) exercised at full scale, on
 * top of the per-frame position-rewrite worst case.
 *
 * Usage: millionDemo [-G|--grow] [gridW] [gridH] [screenshot.ppm]
 */

#include "grender/grender.h"

#include "gviz.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

static void printUsage(const char *prog) {
  fprintf(stderr, "Usage: %s [-G|--grow] [gridW] [gridH] [screenshot.ppm]\n",
          prog);
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
  bool grow = false;
  const char *positional[3] = {NULL, NULL, NULL};
  size_t nPositional = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-G") == 0 || strcmp(argv[i], "--grow") == 0) {
      grow = true;
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

  printf(grow ? "streaming %zux%zu grid (%zu vertices), one row per frame...\n"
              : "building %zux%zu grid (%zu vertices)...\n",
         gridW, gridH, n);
  gviz::Graph graph(false, n);
  size_t builtRows = grow ? 1 : gridH;
  try {
    for (size_t y = 0; y < builtRows; y++)
      addGridRow(graph, gridW, y);
  } catch (const std::exception &e) {
    fprintf(stderr, "graph construction failed: %s\n", e.what());
    return 1;
  }

  std::optional<gviz::Subgraph> sg;
  if (grow) {
    /* Whole-graph vertex-induced view: the dynamic embedding shape (see
     * EmbeddedGraph.hpp's GROWTH & SYNC section). No layout is built up
     * front; click-picking refreshes one on demand. */
    sg.emplace(gviz::Subgraph::CreateVertexInduced(graph));
    for (size_t i = 0; i < graph.Size(); i++)
      sg->ShowVertex(i);
  } else {
    graph.BuildLayout();
    sg.emplace(gviz::Subgraph::CreateFull(graph));
  }

  std::optional<gviz::layout::EmbeddedGraph> eg;
  try {
    eg.emplace(std::move(*sg), 2);
    if (grow)
      eg->Sync();
    for (size_t y = 0; y < builtRows; y++)
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
  desc.title = grow ? "grender - 1M vertices (streaming)" : "grender - 1M vertices";
  desc.nodeStyle.radius = 1.0f;
  desc.edgeStyle.color = GR_COLOR(0.45f, 0.55f, 0.75f, 0.25f);
  desc.vsync = false;

  grRenderer *r = grRendererCreate(&desc);
  if (!r || grRendererSetGraph(r, *eg, &graph) < 0) {
    fprintf(stderr, "renderer setup failed\n");
    return 1;
  }

  double t = 0.0, statAccum = 0.0;
  size_t statFrames = 0, totalFrames = 0;
  size_t committedRows = builtRows;
  while (grRendererFrame(r)) {
    double dt = grRendererDeltaTime(r);
    t += dt;
    statAccum += dt;
    statFrames++;
    totalFrames++;
    if (statAccum >= 1.0) {
      printf("avg frame: %.2f ms (%.0f fps), %zu/%zu rows\n",
             1000.0 * statAccum / statFrames, statFrames / statAccum,
             committedRows, gridH);
      statAccum = 0.0;
      statFrames = 0;
    }

    // Stream one row per frame: mutate the graph directly, commit with
    // EmbeddedGraph::Sync, then place the row -- its position slots exist
    // only once the commit admits it.
    if (grow && committedRows < gridH) {
      size_t y = committedRows;
      try {
        addGridRow(graph, gridW, y);
        eg->Sync();
        for (size_t x = 0; x < gridW; x++) {
          double *p = eg->GetVPosition(y * gridW + x);
          p[0] = (double)x;
          p[1] = (double)y;
        }
        committedRows++;
        if (committedRows == gridH)
          printf("grid complete: %zu vertices\n", graph.Size());
      } catch (const std::exception &e) {
        fprintf(stderr, "row %zu growth failed: %s\n", y, e.what());
      }
    }

    // Rewrite every committed position: traveling wave across the grid.
    for (size_t y = 0; y < committedRows; y++)
      for (size_t x = 0; x < gridW; x++) {
        double *p = eg->GetVPosition(y * gridW + x);
        p[1] = (double)y + 20.0 * sin(0.03 * (double)x + 2.0 * t);
      }

    if (screenshotPath) {
      if (grow && totalFrames == 118)
        grRendererFitView(r); // frame the rows grown so far before shooting
      if (totalFrames == 120) {
        if (grRendererSaveScreenshot(r, screenshotPath) == 0)
          printf("screenshot saved to %s\n", screenshotPath);
        grRendererRequestClose(r);
      }
    }
  }

  grRendererDestroy(r);
  return 0;
}
