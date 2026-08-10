/**
 * Live Barnes-Hut force embedding demo, using gviz's configurable
 * gviz::layout::ForceAtlas framework (LinLog or Fruchterman-Reingold,
 * gravity, Barnes-Hut approximation).
 *
 * A random spanning-tree-plus-extra-edges graph (edge connectivity
 * configurable) is laid out while grender draws every frame. The embedder
 * registers "forceEmbedder.step" on its embedded graph; this app merely
 * binds a key to that name without knowing what it does. Node radii are
 * scaled by degree via ForceAtlas::ConfigureRadius, and the per-vertex
 * radii are handed to grRendererSetNodeSizes so what's drawn matches the
 * circles the physics can be told to keep apart. Overlap prevention (which
 * makes repulsion actually respect those radii) starts off and is toggled
 * live with 'O' once the layout has roughly settled, mirroring Gephi's
 * "Prevent Overlap" checkbox.
 *
 * With -G/--grow the graph is dynamic: the embedder is built over a
 * whole-graph vertex-induced view (no up-front edge-bitset layout), and 'A'
 * adds a vertex wired to one or two random existing vertices directly on
 * the gviz::Graph, committed with ForceAtlas::Sync -- the new vertex
 * appears already placed near its neighbors, visible and simulated in the
 * same frame (gviz's commit model: nothing downstream of the graph sees a
 * mutation until Sync). Everything else works as in static mode:
 * click-highlighting (the renderer refreshes the pick layout on demand),
 * per-vertex sizes (preserved by the renderer across growth), and
 * --degree-alpha/--edge-weight-width (derived by the renderer from the
 * structure on every commit).
 *
 * Controls:
 *   R      - run one relaxation step
 *   space  - toggle continuous stepping
 *   A      - add a vertex and commit it (with -G/--grow)
 *   h/l    - decrease/increase ideal edge length
 *   j/k    - decrease/increase gravity k
 *   N/M    - decrease/increase Barnes-Hut theta
 *   O      - toggle overlap prevention (off at start; enable once the layout settles)
 *   [/]    - decrease/increase radius base (r(v) = base * (1 + perDegree*sqrt(degree(v))); scaling base preserves relative radius differences between vertices)
 *   F      - fit view
 *   S      - toggle stats overlay
 *   drag   - pan
 *   scroll - zoom
 *
 * Usage: forceEmbedderDemo [options]
 *   -n, --vertices N              number of vertices for a random graph
 *                                 (default 200; ignored with -g/--graph)
 *   -s, --seed SEED               RNG seed (default: time-based)
 *   -e, --edge-connectivity C     extra-edge probability in [0, 1] (default 0;
 *                                 ignored with -g/--graph)
 *   -g, --graph NAME              load <gviz-data>/NAME/data.gexf or
 *                                 data.edges instead of a random graph
 *   -d, --directed                interpret the -g/--graph file's edges as
 *                                 directed (default: undirected; ignored
 *                                 without -g/--graph)
 *   -G, --grow                    dynamic mode: vertex-induced embedding, 'A'
 *                                 grows the graph live (screenshot mode grows
 *                                 automatically while stepping)
 *   -m, --model {linlog|fr}       force model (default linlog)
 *   -o, --screenshot PATH         save a .ppm screenshot after settling and exit
 *       --degree-alpha            fade edges by max endpoint degree (default off)
 *       --no-degree-alpha         disable degree-based edge opacity
 *   -w, --edge-width WIDTH        base edge thickness (default 1.5); with
 *                                 --edge-weight-width, this is the thickness
 *                                 drawn for an average-weight edge
 *       --edge-weight-width       scale edge thickness by edge weight (default off)
 *       --no-edge-weight-width    disable weight-based edge thickness
 *   -h, --help                    print this help and exit
 */

#include "grender/grender.h"

#include "gviz.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <memory>
#include <optional>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <vector>

#ifndef GRENDER_GVIZ_DATA_DIR
#error "GRENDER_GVIZ_DATA_DIR must be defined by CMake"
#endif

#define DEMO_GRAVITY_K_DEFAULT 1.00
#define DEMO_GRAVITY_K_STEP 0.10
#define DEMO_THETA_STEP 0.1
#define DEMO_THETA_MIN 0.1
#define DEMO_EDGE_LENGTH_STEP 0.2
#define DEMO_EDGE_LENGTH_MIN 0.2
#define DEMO_RADIUS_BASE 0.5
#define DEMO_RADIUS_PER_DEGREE 5
#define DEMO_RADIUS_BASE_STEP 0.1
#define DEMO_RADIUS_BASE_MIN 0.0

using gviz::layout::ForceAtlas;

/*
 * Renderer + scratch buffer + shadow config state needed by the live-tuning
 * action handlers. The old C ForceAtlas struct exposed gravityK/edgeLength/
 * theta/radiusBase/radiusPerDegree as plain fields the demo read back
 * directly for its printf readouts; the ported ForceAtlas class only offers
 * one-way Configure*() setters (see ForceAtlas.hpp), so this demo keeps its
 * own shadow copy of whatever it last configured instead -- a self-contained
 * demo-only adaptation, not a gap the renderer or gviz need to fill.
 */
typedef struct DemoConfig {
  grRenderer *r = nullptr;
  std::vector<float> radii; // scratch, sized to vertex count, reused across updates
  double gravityK = DEMO_GRAVITY_K_DEFAULT;
  double edgeLength = ForceAtlas::kEdgeLengthDefault;
  double theta = ForceAtlas::kThetaDefault;
  double radiusBase = DEMO_RADIUS_BASE;
  double radiusPerDegree = DEMO_RADIUS_PER_DEGREE;
} DemoConfig;

/*
 * ForceAtlas doesn't expose its compact-index -> raw-vertex-id table
 * (vertices_ was a private old-C-struct field this demo used to read
 * directly): this demo always keeps every graph vertex admitted into a
 * dense [0, PositionCount()) subgraph (full mode, or a vertex-induced
 * "grow" mode that shows every vertex as it's added), so compact index i
 * and raw vertex id i coincide here, and PositionCount() stands in for the
 * old vertexCount. That equivalence is specific to this demo's usage, not a
 * general library guarantee.
 */
static void refreshNodeSizes(ForceAtlas &fa, DemoConfig &ctl) {
  size_t n = fa.PositionCount();
  if (ctl.radii.size() < n)
    ctl.radii.resize(n);
  for (size_t i = 0; i < n; i++)
    ctl.radii[i] = (float)fa.VertexRadius(i);
  grRendererSetNodeSizes(ctl.r, ctl.radii.data(), n);
}

static void actionToggleAuto(gviz::layout::EmbeddedGraph &eg, void *userData,
                             const gviz::layout::ActionPayload &payload) {
  (void)eg;
  (void)payload;
  bool *autoStep = (bool *)userData;
  *autoStep = !*autoStep;
  printf("auto step: %s\n", *autoStep ? "on" : "off");
}

static void actionGravityUp(gviz::layout::EmbeddedGraph &eg, void *userData,
                            const gviz::layout::ActionPayload &payload) {
  (void)payload;
  DemoConfig *cfg = (DemoConfig *)userData;
  auto &fa = static_cast<ForceAtlas &>(eg);
  cfg->gravityK += DEMO_GRAVITY_K_STEP;
  fa.ConfigureGravity(cfg->gravityK);
  printf("gravity k: %f\n", cfg->gravityK);
}

static void actionGravityDown(gviz::layout::EmbeddedGraph &eg, void *userData,
                              const gviz::layout::ActionPayload &payload) {
  (void)payload;
  DemoConfig *cfg = (DemoConfig *)userData;
  auto &fa = static_cast<ForceAtlas &>(eg);
  cfg->gravityK = fmax(cfg->gravityK - DEMO_GRAVITY_K_STEP, 0.0);
  fa.ConfigureGravity(cfg->gravityK);
  printf("gravity k: %f\n", cfg->gravityK);
}

static void actionEdgeLengthUp(gviz::layout::EmbeddedGraph &eg, void *userData,
                               const gviz::layout::ActionPayload &payload) {
  (void)payload;
  DemoConfig *cfg = (DemoConfig *)userData;
  auto &fa = static_cast<ForceAtlas &>(eg);
  cfg->edgeLength += DEMO_EDGE_LENGTH_STEP;
  fa.Configure(cfg->edgeLength, 0);
  printf("edge length: %f\n", cfg->edgeLength);
}

static void actionEdgeLengthDown(gviz::layout::EmbeddedGraph &eg, void *userData,
                                 const gviz::layout::ActionPayload &payload) {
  (void)payload;
  DemoConfig *cfg = (DemoConfig *)userData;
  auto &fa = static_cast<ForceAtlas &>(eg);
  cfg->edgeLength = fmax(cfg->edgeLength - DEMO_EDGE_LENGTH_STEP, DEMO_EDGE_LENGTH_MIN);
  fa.Configure(cfg->edgeLength, 0);
  printf("edge length: %f\n", cfg->edgeLength);
}

static void actionThetaUp(gviz::layout::EmbeddedGraph &eg, void *userData,
                          const gviz::layout::ActionPayload &payload) {
  (void)payload;
  DemoConfig *cfg = (DemoConfig *)userData;
  auto &fa = static_cast<ForceAtlas &>(eg);
  cfg->theta += DEMO_THETA_STEP;
  fa.ConfigureBarnesHut(cfg->theta, 0);
  printf("theta: %f\n", cfg->theta);
}

static void actionThetaDown(gviz::layout::EmbeddedGraph &eg, void *userData,
                            const gviz::layout::ActionPayload &payload) {
  (void)payload;
  DemoConfig *cfg = (DemoConfig *)userData;
  auto &fa = static_cast<ForceAtlas &>(eg);
  cfg->theta = fmax(cfg->theta - DEMO_THETA_STEP, DEMO_THETA_MIN);
  fa.ConfigureBarnesHut(cfg->theta, 0);
  printf("theta: %f\n", cfg->theta);
}

static void actionRadiusBaseUp(gviz::layout::EmbeddedGraph &eg, void *userData,
                               const gviz::layout::ActionPayload &payload) {
  (void)payload;
  DemoConfig *cfg = (DemoConfig *)userData;
  auto &fa = static_cast<ForceAtlas &>(eg);
  cfg->radiusBase += DEMO_RADIUS_BASE_STEP;
  fa.ConfigureRadius(cfg->radiusBase, cfg->radiusPerDegree);
  refreshNodeSizes(fa, *cfg);
  printf("radius base: %f\n", cfg->radiusBase);
}

static void actionRadiusBaseDown(gviz::layout::EmbeddedGraph &eg, void *userData,
                                 const gviz::layout::ActionPayload &payload) {
  (void)payload;
  DemoConfig *cfg = (DemoConfig *)userData;
  auto &fa = static_cast<ForceAtlas &>(eg);
  cfg->radiusBase = fmax(cfg->radiusBase - DEMO_RADIUS_BASE_STEP, DEMO_RADIUS_BASE_MIN);
  fa.ConfigureRadius(cfg->radiusBase, cfg->radiusPerDegree);
  refreshNodeSizes(fa, *cfg);
  printf("radius base: %f\n", cfg->radiusBase);
}

static void actionToggleOverlapPrevention(gviz::layout::EmbeddedGraph &eg, void *userData,
                                          const gviz::layout::ActionPayload &payload) {
  (void)userData;
  (void)payload;
  auto &fa = static_cast<ForceAtlas &>(eg);
  fa.SetPreventOverlapEnabled(!fa.PreventOverlapEnabled());
  printf("prevent overlap: %s\n", fa.PreventOverlapEnabled() ? "on" : "off");
}

/* Everything actionGrowVertex needs: the mutable parent graph (mutated
 * directly, per gviz's commit model), an RNG stream of its own, and a flag
 * for the main loop to refresh node sizes AFTER the renderer has grown its
 * per-vertex buffers -- grRendererSetNodeSizes requires count to match the
 * renderer's position capacity, which only catches up on the frame
 * following the commit. */
typedef struct GrowControl {
  gviz::Graph *graph;
  unsigned int rng;
  bool sizesDirty;
} GrowControl;

/* Adds one vertex wired to one or two random existing vertices directly on
 * the gviz::Graph, then commits with ForceAtlas::Sync: the vertex becomes
 * visible and simulated together, already placed near its neighbors. Wiring
 * the edges BEFORE the Sync is what makes that placement possible (see
 * ForceAtlas::Sync's contract). */
static void actionGrowVertex(gviz::layout::EmbeddedGraph &eg, void *userData,
                             const gviz::layout::ActionPayload &payload) {
  (void)payload;
  GrowControl *grow = (GrowControl *)userData;
  auto &fa = static_cast<ForceAtlas &>(eg);

  size_t before = grow->graph->Size();
  if (before == 0)
    return;
  size_t newId = grow->graph->AddVertex();
  size_t target = (size_t)rand_r(&grow->rng) % before;
  grow->graph->AddEdge(newId, target, 1.0);
  if (before > 1 && rand_r(&grow->rng) % 2 == 0) {
    size_t second = (size_t)rand_r(&grow->rng) % before;
    if (second != target)
      grow->graph->AddEdge(newId, second, 1.0);
  }

  try {
    fa.Sync((unsigned int)rand_r(&grow->rng) | 1u);
  } catch (const std::exception &e) {
    fprintf(stderr, "sync failed; vertex %zu joins on a later sync: %s\n", newId,
            e.what());
    return;
  }
  grow->sizesDirty = true;
  printf("added vertex %zu (%zu vertices)\n", newId, grow->graph->Size());
}

static int parseModel(const char *arg, bool *outLinLog) {
  if (!arg || strcasecmp(arg, "linlog") == 0) {
    *outLinLog = true;
    return 0;
  }
  if (strcasecmp(arg, "fr") == 0) {
    *outLinLog = false;
    return 0;
  }
  return -1;
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
      "Live Barnes-Hut force embedding demo.\n"
      "\n"
      "Options:\n"
      "  -n, --vertices N            number of vertices for a random graph\n"
      "                              (default 200; ignored with -g/--graph)\n"
      "  -s, --seed SEED             RNG seed (default: time-based)\n"
      "  -e, --edge-connectivity C   extra-edge probability in [0, 1] (default 0;\n"
      "                              ignored with -g/--graph)\n"
      "  -g, --graph NAME            load <gviz-data>/NAME/data.gexf or\n"
      "                              data.edges instead of a random graph\n"
      "  -d, --directed              interpret the -g/--graph file's edges as\n"
      "                              directed (default: undirected; ignored\n"
      "                              without -g/--graph)\n"
      "  -G, --grow                  dynamic mode: 'A' adds a vertex live,\n"
      "                              committed via ForceAtlas::Sync\n"
      "  -m, --model {linlog|fr}     force model (default linlog)\n"
      "  -o, --screenshot PATH       save a .ppm screenshot after settling and exit\n"
      "      --degree-alpha          fade edges by max endpoint degree (default off)\n"
      "      --no-degree-alpha       disable degree-based edge opacity\n"
      "  -w, --edge-width WIDTH      base edge thickness (default 1.5); with\n"
      "                              --edge-weight-width, this is the thickness\n"
      "                              drawn for an average-weight edge\n"
      "      --edge-weight-width     scale edge thickness by edge weight (default off)\n"
      "      --no-edge-weight-width  disable weight-based edge thickness\n"
      "  -h, --help                  print this help and exit\n"
      "\n"
      "Controls:\n"
      "  R      - run one relaxation step\n"
      "  space  - toggle continuous stepping\n"
      "  A      - add a vertex and commit it (with -G/--grow)\n"
      "  h/l    - decrease/increase ideal edge length\n"
      "  j/k    - decrease/increase gravity k\n"
      "  N/M    - decrease/increase Barnes-Hut theta\n"
      "  O      - toggle overlap prevention (off at start; enable once the layout settles)\n"
      "  [/]    - decrease/increase radius base\n"
      "  F      - fit view\n"
      "  S      - toggle stats overlay\n"
      "  drag   - pan\n"
      "  scroll - zoom\n",
      prog);
}

enum {
  OPT_DEGREE_ALPHA = 256,
  OPT_NO_DEGREE_ALPHA,
  OPT_EDGE_WEIGHT_WIDTH,
  OPT_NO_EDGE_WEIGHT_WIDTH,
};

static const struct option kLongOptions[] = {
    {"vertices", required_argument, NULL, 'n'},
    {"seed", required_argument, NULL, 's'},
    {"edge-connectivity", required_argument, NULL, 'e'},
    {"graph", required_argument, NULL, 'g'},
    {"directed", no_argument, NULL, 'd'},
    {"grow", no_argument, NULL, 'G'},
    {"model", required_argument, NULL, 'm'},
    {"screenshot", required_argument, NULL, 'o'},
    {"degree-alpha", no_argument, NULL, OPT_DEGREE_ALPHA},
    {"no-degree-alpha", no_argument, NULL, OPT_NO_DEGREE_ALPHA},
    {"edge-width", required_argument, NULL, 'w'},
    {"edge-weight-width", no_argument, NULL, OPT_EDGE_WEIGHT_WIDTH},
    {"no-edge-weight-width", no_argument, NULL, OPT_NO_EDGE_WEIGHT_WIDTH},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};

int main(int argc, char **argv) {
  size_t N = 200;
  unsigned int seed = (unsigned int)time(NULL);
  double edgeConnectivity = 0.0;
  const char *graphName = NULL;
  bool directed = false;
  bool grow = false;
  bool useLinLog = true;
  const char *screenshotPath = NULL;
  bool degreeAlpha = false;
  float edgeWidth = 1.5f;
  bool edgeWeightWidth = false;

  int opt;
  while ((opt = getopt_long(argc, argv, "n:s:e:g:dGm:o:w:h", kLongOptions,
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
    case 'G':
      grow = true;
      break;
    case 'm':
      if (parseModel(optarg, &useLinLog) < 0) {
        fprintf(stderr, "unknown model \"%s\", expected \"linlog\" or \"fr\"\n",
                optarg);
        return 1;
      }
      break;
    case 'o':
      screenshotPath = optarg;
      break;
    case OPT_DEGREE_ALPHA:
      degreeAlpha = true;
      break;
    case OPT_NO_DEGREE_ALPHA:
      degreeAlpha = false;
      break;
    case 'w':
      edgeWidth = strtof(optarg, NULL);
      break;
    case OPT_EDGE_WEIGHT_WIDTH:
      edgeWeightWidth = true;
      break;
    case OPT_NO_EDGE_WEIGHT_WIDTH:
      edgeWeightWidth = false;
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
    graphOpt = gviz::graphs::BuildRandomConnectedGraph(N, edgeConnectivity, seed);
  }
  gviz::Graph &graph = *graphOpt;

  std::optional<gviz::Subgraph> sg;
  if (grow) {
    /* Dynamic mode: whole-graph vertex-induced view. No edge-bitset layout
     * is built up front, so each growth commit is amortized O(1) capacity
     * work plus the embedding's CSR rebuild (see EmbeddedGraph.hpp's
     * GROWTH & SYNC section). Click-picking still works: the renderer
     * refreshes the layout on demand (Graph::EnsureLayout) when a click
     * needs a pick subgraph. */
    sg.emplace(gviz::Subgraph::CreateVertexInduced(graph));
    for (size_t i = 0; i < graph.Size(); i++)
      sg->ShowVertex(i);
    if (graphName)
      printf("loaded %zu vertices\n", graph.Size());
  } else {
    graph.BuildLayout();
    if (graphName)
      printf("loaded %zu vertices, %zu edges\n", graph.Size(), graph.EdgeCount());
    sg.emplace(gviz::Subgraph::CreateFull(graph));
  }

  std::unique_ptr<gviz::layout::ForceModel> model =
      useLinLog ? std::unique_ptr<gviz::layout::ForceModel>(
                      std::make_unique<gviz::layout::LinLog>())
                : std::unique_ptr<gviz::layout::ForceModel>(
                      std::make_unique<gviz::layout::FruchtermanReingold>());

  std::optional<ForceAtlas> fe;
  try {
    fe.emplace(std::move(*sg), 2, std::move(model));
  } catch (const std::exception &e) {
    fprintf(stderr, "force embedder init failed: %s\n", e.what());
    return 1;
  }
  fe->SetBarnesHutEnabled(true);

  DemoConfig cfg;
  cfg.gravityK = DEMO_GRAVITY_K_DEFAULT;
  cfg.radiusBase = DEMO_RADIUS_BASE;
  cfg.radiusPerDegree = DEMO_RADIUS_PER_DEGREE;
  fe->ConfigureGravity(cfg.gravityK);
  fe->ConfigureRadius(cfg.radiusBase, cfg.radiusPerDegree);
  fe->Begin(seed);

  bool autoStep = false;
  GrowControl growControl = {&graph, seed ^ 0x9e3779b9u, false};
  if (grow)
    fe->AddAction("demo.growVertex", actionGrowVertex, &growControl);
  fe->AddAction("demo.toggleAuto", actionToggleAuto, &autoStep);
  fe->AddAction("demo.gravityUp", actionGravityUp, &cfg);
  fe->AddAction("demo.gravityDown", actionGravityDown, &cfg);
  fe->AddAction("demo.edgeLengthUp", actionEdgeLengthUp, &cfg);
  fe->AddAction("demo.edgeLengthDown", actionEdgeLengthDown, &cfg);
  fe->AddAction("demo.thetaUp", actionThetaUp, &cfg);
  fe->AddAction("demo.thetaDown", actionThetaDown, &cfg);
  fe->AddAction("demo.toggleOverlapPrevention", actionToggleOverlapPrevention, NULL);
  fe->AddAction("demo.radiusBaseUp", actionRadiusBaseUp, &cfg);
  fe->AddAction("demo.radiusBaseDown", actionRadiusBaseDown, &cfg);

  grRendererDesc desc;
  grRendererDescInit(&desc);
  desc.title =
      "grender - Force Embedder (R: step, space: auto, h/l: edge len, j/k: gravity, N/M: theta, O: overlap)";
  desc.nodeStyle.radius = (float)DEMO_RADIUS_BASE;
  desc.nodeStyle.sizeMode = GR_SIZE_WORLD;
  /* World-space radius keeps nodes correctly sized relative to the layout at
   * any fixed zoom, but this LinLog layout drifts outward for a long time
   * before settling (a separate, pre-existing issue) -- as the camera fits a
   * growing bounding box, the same world radius covers fewer pixels. Floor
   * the drawn size so nodes stay visible/legible regardless (this also
   * covers radiusBase == 0, where every node's world radius is exactly 0
   * and the floor is what draws them as uniform dots). No ceiling: degree
   * scales world radius by up to ~sqrt(maxDegree)*perDegree, and clamping
   * pixel size breaks that relative sizing once any node's world radius
   * would map past the ceiling -- nodes "catch up" to and visually merge
   * with already-clamped hubs as base or zoom increases. */
  desc.nodeStyle.minPixelRadius = 2.0f;
  desc.nodeStyle.maxPixelRadius = 0.0f;
  desc.nodeStyle.fillColor = GR_COLOR(0.55f, 0.78f, 1.0f, 1.0f);
  desc.edgeStyle.color = GR_COLOR(0.45f, 0.55f, 0.75f, 0.45f);
  desc.edgeStyle.width = edgeWidth;
  desc.edgeDegreeAlpha = degreeAlpha;
  desc.edgeWeightWidth = edgeWeightWidth;

  grRenderer *r = grRendererCreate(&desc);
  if (!r) {
    fprintf(stderr, "renderer creation failed\n");
    return 1;
  }

  if (grRendererSetGraph(r, *fe, &graph) < 0) {
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
  size_t vertexLabelCount = fe->PositionCount();
  std::vector<const char *> vertexLabels(vertexLabelCount ? vertexLabelCount : 1);
  for (size_t i = 0; i < vertexLabelCount; i++) {
    void *data = graph.GetVertexData(i);
    vertexLabels[i] = data ? static_cast<const std::string *>(data)->c_str() : NULL;
  }
  grRendererSetVertexLabels(r, vertexLabels.data(), vertexLabelCount);

  /* Degree is fixed for the embedder's lifetime, but radiusBase can change
   * live via '['/']', so this buffer is kept around (not freed) and reused
   * by refreshNodeSizes on every such change instead of being a one-shot
   * upload. */
  cfg.r = r;
  refreshNodeSizes(*fe, cfg);

  /* --degree-alpha / --edge-weight-width need no uploads here: with the
   * desc flags set, the renderer derives degrees and per-edge weights from
   * the graph itself on every structural change, so they stay correct as
   * the graph grows. */

  grRendererBindKey(r, 'R', "forceEmbedder.step");
  grRendererBindKey(r, GR_KEY_SPACE, "demo.toggleAuto");
  grRendererBindKey(r, 'L', "demo.edgeLengthUp");
  grRendererBindKey(r, 'H', "demo.edgeLengthDown");
  grRendererBindKey(r, 'K', "demo.gravityUp");
  grRendererBindKey(r, 'J', "demo.gravityDown");
  grRendererBindKey(r, 'M', "demo.thetaUp");
  grRendererBindKey(r, 'N', "demo.thetaDown");
  grRendererBindKey(r, 'O', "demo.toggleOverlapPrevention");
  grRendererBindKey(r, ']', "demo.radiusBaseUp");
  grRendererBindKey(r, '[', "demo.radiusBaseDown");
  if (grow)
    grRendererBindKey(r, 'A', "demo.growVertex");
  grRendererFitView(r);

  const size_t stepsBeforeShot = screenshotPath ? 300 : SIZE_MAX;
  size_t totalSteps = 0;

  while (grRendererFrame(r)) {
    if (autoStep) {
      for (size_t i = 0; i < 1; i++)
        fe->Step();
    }

    /* Deferred by actionGrowVertex: the renderer's per-vertex buffers only
     * grew during the grRendererFrame call above, so this is the earliest
     * the (raw-id-indexed) radius buffer can be resized and re-uploaded at
     * the matching count. */
    if (growControl.sizesDirty) {
      refreshNodeSizes(*fe, cfg);
      growControl.sizesDirty = false;
    }

    /* Screenshot mode doubles as the dynamic pipeline's end-to-end check:
     * keep stepping and grow a vertex every 30 frames, so the saved frame
     * shows grown vertices that were placed, drawn, and simulated through
     * the mutate-then-commit path. */
    if (grow && screenshotPath) {
      fe->Step();
      if (totalSteps % 30 == 29)
        fe->InvokeAction("demo.growVertex");
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
