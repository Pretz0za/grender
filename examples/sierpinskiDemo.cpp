/**
 * 2D Sierpinski triangle demo, laid out live by gviz's GRIP embedder --
 * the normal grender workflow (see gripDemo.cpp): grender never sees or
 * sets a single position itself, it only reads whatever GRIP has computed
 * each frame (spec 3, "online rendering") and dispatches the "grip.*"
 * actions GRIP registers on itself (spec 4, "creator-defined actions").
 *
 * Usage: sierpinskiDemo [depth] [screenshot.ppm]
 *
 * Controls:
 *   R      - run one GRIP refinement round
 *   N      - advance to the next (finer) GRIP layer
 *   space  - toggle continuous refinement
 *   F      - fit view
 *   drag   - pan
 *   scroll - zoom
 */

#include "grender/grender.h"

#include "gviz.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

namespace {

void actionToggleAuto(gviz::layout::EmbeddedGraph &eg, void *userData,
                      const gviz::layout::ActionPayload &payload) {
  (void)eg, (void)payload;
  bool *autoRefine = (bool *)userData;
  *autoRefine = !*autoRefine;
  printf("auto refine: %s\n", *autoRefine ? "on" : "off");
}

struct AppState {
  grRenderer *renderer;
  gviz::layout::GRIP<gviz::Subgraph> *grip;
  bool *autoRefine;
  size_t roundsPerStage;
  size_t frames = 0;
  const char *screenshotPath = nullptr;
};

bool tick(AppState &state) {
  if (!grRendererFrame(state.renderer))
    return false;

  if (*state.autoRefine) {
    if (state.grip->CurrentRound() >= state.roundsPerStage) {
      if (state.grip->CurrentLayer() == 0) {
        if (state.screenshotPath) {
          grRendererFitView(state.renderer);
          grRendererFrame(state.renderer);
          if (grRendererSaveScreenshot(state.renderer, state.screenshotPath) == 0)
            printf("screenshot saved to %s\n", state.screenshotPath);
          else
            fprintf(stderr, "screenshot failed\n");
          grRendererRequestClose(state.renderer);
        }
      } else {
        state.grip->NextStage();
      }
    } else {
      state.grip->RefineRound();
    }
  }
  return true;
}

#ifdef __EMSCRIPTEN__
void emscriptenTick(void *arg) {
  AppState *state = (AppState *)arg;
  if (!tick(*state)) {
    grRendererDestroy(state->renderer);
    delete state;
    emscripten_cancel_main_loop();
  }
}
#endif

} // namespace

int main(int argc, char **argv) {
  int depth = argc > 1 ? atoi(argv[1]) : 7;
  const char *screenshotPath = argc > 2 ? argv[2] : NULL;

  if (depth < 0) {
    fprintf(stderr, "depth must be >= 0\n");
    return 1;
  }

  gviz::Graph graph = gviz::graphs::CreateSierpinski(depth);
  graph.BuildLayout();
  printf("sierpinski triangle depth %d (%zu vertices, %zu edges)\n", depth,
         graph.Size(), graph.EdgeCount());
  fflush(stdout);

  gviz::Subgraph sg = gviz::Subgraph::CreateFull(graph);

  std::optional<gviz::layout::GRIP<gviz::Subgraph>> grip;
  try {
    grip.emplace(std::move(sg), /*diameter=*/0, /*dimension=*/2);
  } catch (const std::exception &e) {
    fprintf(stderr, "GRIP init failed: %s\n", e.what());
    return 1;
  }
  grip->Begin();

  bool autoRefine = true;
  grip->AddAction("demo.toggleAuto", actionToggleAuto, &autoRefine);

  grRendererDesc desc;
  grRendererDescInit(&desc);
  desc.title = "grender - Sierpinski triangle (GRIP)";
  desc.nodeStyle.radius = 2.0f;
  desc.nodeStyle.fillColor = GR_COLOR(0.95f, 0.65f, 0.35f, 1.0f);
  desc.edgeStyle.color = GR_COLOR(0.95f, 0.75f, 0.45f, 0.65f);
  desc.edgeStyle.width = 1.0f;

  grRenderer *r = grRendererCreate(&desc);
  if (!r) {
    fprintf(stderr, "renderer creation failed\n");
    return 1;
  }
  if (grRendererSetGraph(r, grip->Structure(), *grip, &graph) < 0) {
    fprintf(stderr, "graph attach failed\n");
    grRendererDestroy(r);
    return 1;
  }

  grRendererBindKey(r, 'R', "grip.refineRound");
  grRendererBindKey(r, 'N', "grip.nextStage");
  grRendererBindKey(r, GR_KEY_SPACE, "demo.toggleAuto");

#ifdef __EMSCRIPTEN__
  AppState *state =
      new AppState{r, &*grip, &autoRefine, 60, 0, screenshotPath};
  emscripten_set_main_loop_arg(emscriptenTick, state, 0, true);
#else
  AppState state{r, &*grip, &autoRefine, 60, 0, screenshotPath};
  while (tick(state)) {
  }
  grRendererDestroy(r);
#endif

  return 0;
}
