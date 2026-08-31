/**
 * Live Kamada-Kawai graph-distance embedding demo, using gviz's
 * gviz::layout::KamadaKawai embedder (Kamada & Kawai 1989: minimizes a
 * global energy defined by all-pairs graph-theoretic distance rather than a
 * continuous physical simulation).
 *
 * Precompute-then-iterate-to-convergence, same shape as tutteDemo: Begin()
 * verifies the graph is connected (KamadaKawai's energy is undefined across
 * components), builds the O(V^2) all-pairs distance table, and seeds an
 * initial circular layout; each Step() then Newton-Raphson-refines whatever
 * single vertex currently has the largest gradient. Since Begin() has to
 * rebuild that whole table from scratch, there is no live vertex-adding mode
 * here (unlike forceEmbedderDemo's -G/--grow) -- the embedder just doesn't
 * fit that model.
 *
 * Because of the O(V^2) memory/step cost, this demo defaults to a modest
 * vertex count (50) rather than forceEmbedderDemo's 200, and is intended for
 * small/medium graphs only -- pass a large -g/--graph dataset at your own
 * risk.
 *
 * The embedder registers "kamadaKawai.step" and stat series
 * "kamadaKawai.maxGradient" (StatChartKind::LineLog) on itself at
 * construction; this demo just binds a key to the action name.
 *
 * Controls:
 *   R      - run one Kamada-Kawai refinement step
 *   space  - toggle continuous stepping
 *   j/k    - decrease/increase spring stiffness
 *   F      - fit view
 *   S      - toggle stats overlay
 *   drag   - pan
 *   scroll - zoom
 *
 * Usage: kamadaKawaiDemo [options]
 *   -n, --vertices N            number of vertices for a random graph
 *                               (default 50; ignored with -g/--graph)
 *   -s, --seed SEED             RNG seed (default: time-based)
 *   -e, --edge-connectivity C   extra-edge probability in [0, 1] (default 0;
 *                               ignored with -g/--graph)
 *   -g, --graph NAME            load <gviz-data>/NAME/data.gexf or
 *                               data.edges instead of a random graph (must
 *                               be connected -- KamadaKawai::Begin() throws
 *                               otherwise)
 *   -d, --directed               interpret the -g/--graph file's edges as
 *                               directed (default: undirected; ignored
 *                               without -g/--graph)
 *   -l, --edge-length L          desired unit edge length (default 100)
 *   -o, --screenshot PATH        save a .ppm screenshot after settling and exit
 *   -h, --help                    print this help and exit
 */

#include "grender/grender.h"
#include "grender/grStepProfiling.h"

#include "gviz.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <optional>
#include <sys/stat.h>
#include <time.h>
#include <vector>

#ifndef GRENDER_GVIZ_DATA_DIR
#error "GRENDER_GVIZ_DATA_DIR must be defined by CMake"
#endif

#define DEMO_VERTICES_DEFAULT 50
#define DEMO_STIFFNESS_STEP 0.1
#define DEMO_STIFFNESS_MIN 0.1

using gviz::layout::KamadaKawai;

static void actionToggleAuto(gviz::layout::EmbeddedGraph &eg, void *userData,
                             const gviz::layout::ActionPayload &payload) {
  (void)eg;
  (void)payload;
  bool *autoStep = (bool *)userData;
  *autoStep = !*autoStep;
  printf("auto step: %s\n", *autoStep ? "on" : "off");
}

static void actionStiffnessUp(gviz::layout::EmbeddedGraph &eg, void *userData,
                              const gviz::layout::ActionPayload &payload) {
  (void)userData;
  (void)payload;
  auto &kk = static_cast<KamadaKawai<gviz::Subgraph> &>(eg);
  kk.SetStiffness(kk.Stiffness() + DEMO_STIFFNESS_STEP);
  printf("stiffness: %f\n", kk.Stiffness());
}

static void actionStiffnessDown(gviz::layout::EmbeddedGraph &eg, void *userData,
                                const gviz::layout::ActionPayload &payload) {
  (void)userData;
  (void)payload;
  auto &kk = static_cast<KamadaKawai<gviz::Subgraph> &>(eg);
  double next = kk.Stiffness() - DEMO_STIFFNESS_STEP;
  if (next < DEMO_STIFFNESS_MIN)
    next = DEMO_STIFFNESS_MIN;
  kk.SetStiffness(next);
  printf("stiffness: %f\n", kk.Stiffness());
}

static int fileExists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/**
 * Loads <GRENDER_GVIZ_DATA_DIR>/<name>/data.gexf if present, else
 * <GRENDER_GVIZ_DATA_DIR>/<name>/data.edges, interpreting edges as directed
 * iff @p directed is true.
 *
 * @return the loaded graph, or std::nullopt if neither file exists or
 * loading failed.
 */
static std::optional<gviz::Graph> loadNamedGraph(const char *name, bool directed) {
  char path[1024];

  snprintf(path, sizeof(path), "%s/%s/data.gexf", GRENDER_GVIZ_DATA_DIR, name);
  if (fileExists(path)) {
    printf("loading %s...\n", path);
    try {
      return gviz::io::LoadFromGexfFile(path, directed);
    } catch (const std::exception &e) {
      fprintf(stderr, "failed to load '%s': %s\n", path, e.what());
      return std::nullopt;
    }
  }

  snprintf(path, sizeof(path), "%s/%s/data.edges", GRENDER_GVIZ_DATA_DIR, name);
  if (fileExists(path)) {
    printf("loading %s...\n", path);
    gviz::io::EdgesFileOptions opts;
    opts.directed = directed;
    try {
      return gviz::io::LoadFromEdgesFile(path, opts);
    } catch (const std::exception &e) {
      fprintf(stderr, "failed to load '%s': %s\n", path, e.what());
      return std::nullopt;
    }
  }

  fprintf(stderr, "no data.gexf or data.edges found under %s/%s\n",
          GRENDER_GVIZ_DATA_DIR, name);
  return std::nullopt;
}

static void printUsage(const char *prog) {
  printf(
      "Usage: %s [options]\n"
      "\n"
      "Live Kamada-Kawai graph-distance embedding demo. Intended for\n"
      "small/medium graphs (O(V^2) memory/step cost).\n"
      "\n"
      "Options:\n"
      "  -n, --vertices N            number of vertices for a random graph\n"
      "                              (default %d; ignored with -g/--graph)\n"
      "  -s, --seed SEED             RNG seed (default: time-based)\n"
      "  -e, --edge-connectivity C   extra-edge probability in [0, 1] (default 0;\n"
      "                              ignored with -g/--graph)\n"
      "  -g, --graph NAME            load <gviz-data>/NAME/data.gexf or\n"
      "                              data.edges instead of a random graph\n"
      "                              (must be connected)\n"
      "  -d, --directed              interpret the -g/--graph file's edges as\n"
      "                              directed (default: undirected; ignored\n"
      "                              without -g/--graph)\n"
      "  -l, --edge-length L          desired unit edge length (default %.1f)\n"
      "  -o, --screenshot PATH        save a .ppm screenshot after settling and exit\n"
      "  -h, --help                    print this help and exit\n"
      "\n"
      "Controls:\n"
      "  R      - run one Kamada-Kawai refinement step\n"
      "  space  - toggle continuous stepping\n"
      "  j/k    - decrease/increase spring stiffness\n"
      "  F      - fit view\n"
      "  S      - toggle stats overlay\n"
      "  drag   - pan\n"
      "  scroll - zoom\n",
      prog, DEMO_VERTICES_DEFAULT,
      KamadaKawai<gviz::Subgraph>::kDefaultEdgeLength);
}

static const struct option kLongOptions[] = {
    {"vertices", required_argument, NULL, 'n'},
    {"seed", required_argument, NULL, 's'},
    {"edge-connectivity", required_argument, NULL, 'e'},
    {"graph", required_argument, NULL, 'g'},
    {"directed", no_argument, NULL, 'd'},
    {"edge-length", required_argument, NULL, 'l'},
    {"screenshot", required_argument, NULL, 'o'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};

int main(int argc, char **argv) {
  size_t N = DEMO_VERTICES_DEFAULT;
  unsigned int seed = (unsigned int)time(NULL);
  double edgeConnectivity = 0.0;
  const char *graphName = NULL;
  bool directed = false;
  double edgeLength = KamadaKawai<gviz::Subgraph>::kDefaultEdgeLength;
  const char *screenshotPath = NULL;

  int opt;
  while ((opt = getopt_long(argc, argv, "n:s:e:g:dl:o:h", kLongOptions,
                            NULL)) != -1) {
    switch (opt) {
    case 'n':
      N = (size_t)atoi(optarg);
      break;
    case 's':
      seed = (unsigned int)atoi(optarg);
      break;
    case 'e':
      edgeConnectivity = strtod(optarg, NULL);
      break;
    case 'g':
      graphName = optarg;
      break;
    case 'd':
      directed = true;
      break;
    case 'l':
      edgeLength = strtod(optarg, NULL);
      break;
    case 'o':
      screenshotPath = optarg;
      break;
    case 'h':
      printUsage(argv[0]);
      return 0;
    default:
      printUsage(argv[0]);
      return 1;
    }
  }

  if (!graphName && N == 0) {
    fprintf(stderr, "N must be >= 1\n");
    return 1;
  }
  if (!graphName && (edgeConnectivity < 0.0 || edgeConnectivity > 1.0)) {
    fprintf(stderr, "edge connectivity must be in [0, 1]\n");
    return 1;
  }
  if (!graphName && directed) {
    fprintf(stderr, "--directed requires -g/--graph\n");
    return 1;
  }

  std::optional<gviz::Graph> graphOpt;
  if (graphName) {
    graphOpt = loadNamedGraph(graphName, directed);
    if (!graphOpt)
      return 1;
  } else {
    // BuildRandomConnectedGraph guarantees connectivity (spanning tree plus
    // extra edges), which KamadaKawai::Begin() requires.
    graphOpt = gviz::graphs::BuildRandomConnectedGraph(N, edgeConnectivity, seed);
  }
  gviz::Graph &graph = *graphOpt;
  graph.BuildLayout();
  printf("graph: %zu vertices, %zu edges\n", graph.Size(), graph.EdgeCount());

  gviz::Subgraph sg = gviz::Subgraph::CreateFull(graph);

  std::optional<KamadaKawai<gviz::Subgraph>> kk;
  try {
    kk.emplace(std::move(sg), 2, edgeLength);
  } catch (const std::exception &e) {
    fprintf(stderr, "KamadaKawai init failed: %s\n", e.what());
    return 1;
  }

  try {
    kk->Begin();
  } catch (const std::exception &e) {
    fprintf(stderr,
            "KamadaKawai begin failed (graph may be disconnected): %s\n",
            e.what());
    return 1;
  }

  bool autoStep = true;
  kk->AddAction("demo.toggleAuto", actionToggleAuto, &autoStep);
  kk->AddAction("demo.stiffnessUp", actionStiffnessUp, NULL);
  kk->AddAction("demo.stiffnessDown", actionStiffnessDown, NULL);

  grRendererDesc desc;
  grRendererDescInit(&desc);
  desc.title = "grender - Kamada-Kawai (R: step, space: auto, j/k: stiffness)";
  desc.nodeStyle.radius = 4.0f;
  desc.nodeStyle.fillColor = GR_COLOR(0.55f, 0.78f, 1.0f, 1.0f);
  desc.edgeStyle.color = GR_COLOR(0.45f, 0.55f, 0.75f, 0.45f);
  desc.edgeStyle.width = 1.5f;

  grRenderer *r = grRendererCreate(&desc);
  if (!r) {
    fprintf(stderr, "renderer creation failed\n");
    return 1;
  }

  if (grRendererSetGraph(r, kk->Structure(), *kk, &graph) < 0) {
    fprintf(stderr, "graph attach failed\n");
    grRendererDestroy(r);
    return 1;
  }

  // Vertex string data (gexf attributes) is only present when -g/--graph
  // loaded a .gexf file, in which case each non-null entry is a
  // heap-allocated std::string* (see gviz::io::LoadFromGexfFile); entries
  // are NULL otherwise, which the overlay simply skips. Freed alongside
  // graph teardown below, once the renderer (the only reader of these
  // pointers) is destroyed.
  // grRendererSetVertexLabels is LOCAL-indexed (see grRendererSetGraph's
  // INDEXING CONVENTION); translate to the graph's own raw id first.
  size_t vertexLabelCount = kk->PositionCount();
  std::vector<const char *> vertexLabels(vertexLabelCount ? vertexLabelCount : 1);
  for (size_t i = 0; i < vertexLabelCount; i++) {
    void *data = graph.GetVertexData(grRendererLocalToRaw(r, i));
    vertexLabels[i] = data ? static_cast<const std::string *>(data)->c_str() : NULL;
  }
  grRendererSetVertexLabels(r, vertexLabels.data(), vertexLabelCount);

  grRendererBindKey(r, 'R', "kamadaKawai.step");
  grRendererBindKey(r, GR_KEY_SPACE, "demo.toggleAuto");
  grRendererBindKey(r, 'K', "demo.stiffnessUp");
  grRendererBindKey(r, 'J', "demo.stiffnessDown");
  grRendererFitView(r);

  const size_t stepsBeforeShot = screenshotPath ? 300 : SIZE_MAX;
  size_t totalSteps = 0;

  while (grRendererFrame(r)) {
    if (autoStep && !kk->Converged()) {
      for (size_t i = 0; i < 20; i++) {
        GR_PROF_STEP_BEGIN();
        kk->Step();
        GR_PROF_STEP_END();
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
  gviz::io::FreeVertexDataStrings(graph);
  return 0;
}
