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
 * Usage: tutteDemo [rows] [cols] [obj] [screenshot.ppm]
 *
 * When [obj] is given, its topology also drives the Tutte embedding (as
 * before), and the same file is additionally loaded into grender's object
 * overlay: a small always-rotating 3D preview of the source mesh in the
 * bottom-left corner, independent of the main 2D Tutte view and its camera.
 */

#include "grender/grender.h"

#include "gviz.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

static void actionToggleAuto(gviz::layout::EmbeddedGraph &eg, void *userData,
                             const gviz::layout::ActionPayload &payload) {
  (void)eg;
  (void)payload;
  bool *autoStep = (bool *)userData;
  *autoStep = !*autoStep;
  printf("auto step: %s\n", *autoStep ? "on" : "off");
}

// Unused by main() below (also true of the old C original) but kept as a
// faithful port: builds a vertex-induced subgraph over the largest connected
// component of @p graph.
static gviz::Subgraph largestComponentSubgraph(const gviz::Graph &graph) {
  size_t n = graph.Size();
  gviz::Subgraph full = gviz::Subgraph::CreateFull(graph);

  gviz::search::Components result = gviz::search::ConnectedComponents(full);
  if (result.count == 0)
    throw std::runtime_error("graph has no connected components");

  std::vector<size_t> sizes =
      gviz::search::ConnectedComponentSizes(result.labels, result.count);

  size_t largest = 0;
  for (size_t c = 1; c < result.count; c++) {
    if (sizes[c] > sizes[largest])
      largest = c;
  }

  printf("components=%zu largest=%zu (%.1f%% of vertices)\n", result.count,
         sizes[largest], 100.0 * (double)sizes[largest] / (double)n);

  gviz::Subgraph vs = gviz::Subgraph::CreateVertexInduced(graph);
  for (size_t v = 0; v < n; v++) {
    if (result.labels[v] == largest)
      vs.ShowVertex(v);
  }
  return vs;
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

  gviz::Graph graph = [&]() -> gviz::Graph {
    if (obj) {
      try {
        return gviz::io::LoadFromObjFile(obj);
      } catch (const std::exception &e) {
        fprintf(stderr, "failed to load '%s': %s\n", obj, e.what());
        exit(1);
      }
    }
    return gviz::graphs::BuildRectMesh(rows, cols);
  }();
  graph.BuildLayout();

  gviz::Subgraph sg = gviz::Subgraph::CreateFull(graph);
  std::optional<gviz::layout::Tutte> tutte;
  tutte.emplace(graph, std::move(sg), 2);

  try {
    tutte->Begin();
  } catch (const std::exception &e) {
    fprintf(stderr, "Tutte begin failed (graph may be non-planar): %s\n",
            e.what());
    return 1;
  }

  bool autoStep = true;
  tutte->AddAction("demo.toggleAuto", actionToggleAuto, &autoStep);

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
    return 1;
  }

  if (grRendererSetGraph(r, *tutte, &graph) < 0) {
    fprintf(stderr, "graph attach failed\n");
    grRendererDestroy(r);
    return 1;
  }

  grRendererBindKey(r, 'R', "tutte.step");
  grRendererBindKey(r, 'B', "tutte.fixOuterFace");
  grRendererBindKey(r, GR_KEY_SPACE, "demo.toggleAuto");
  grRendererBindMouse(r, GR_MOUSE_BUTTON_RIGHT, GR_ACTION_PICK_FACE);

  if (obj && grRendererLoadObjOverlay(r, obj) < 0)
    fprintf(stderr, "object overlay: failed to load '%s'\n", obj);

  const size_t stepsBeforeShot = screenshotPath ? 300 : SIZE_MAX;
  size_t totalSteps = 0;

  while (grRendererFrame(r)) {
    if (autoStep && !tutte->Converged()) {
      double dt = grRendererDeltaTime(r);
      for (size_t i = 0; i < 20; i++) {
        tutte->Step(dt);
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
  return 0;
}
