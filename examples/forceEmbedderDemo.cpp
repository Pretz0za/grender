/**
 * Live Barnes-Hut force embedding demo, using gviz's configurable
 * gviz::layout::ForceAtlas framework (LinLog or Fruchterman-Reingold,
 * gravity, Barnes-Hut approximation).
 *
 * A random spanning-tree-plus-extra-edges graph (edge connectivity
 * configurable) is laid out while grender draws every frame. The embedder
 * registers "forceEmbedder.step" on its embedded graph; this app merely
 * binds a key to that name without knowing what it does. Node radii are
 * scaled by degree in two independent places that are kept in sync by
 * construction, not by convention: ForceAtlas::ConfigureRadius(base,
 * perDegree) drives the physics (what repulsion treats as each vertex's
 * personal-space circle), and grRendererSetNodeDegreeScale(true, perDegree)
 * with nodeStyle.radius = base drives what's actually drawn -- same
 * formula, same two numbers, passed to both from this demo's DemoConfig
 * (the one layer that legitimately knows about both gviz's physics and
 * grender's rendering; neither depends on the other). Overlap prevention
 * (which makes repulsion actually respect those radii) starts off and is
 * toggled live with 'O' once the layout has roughly settled, mirroring
 * Gephi's "Prevent Overlap" checkbox.
 *
 * -G/--grow (the live vertex-adding mode from before gviz's GraphLike/
 * DenseIndex refactor) is REMOVED, not adapted: gviz's embedders have no
 * dynamic-graph-growth/Sync() support at all anymore -- ForceAtlas<G> (like
 * every other embedder) sizes every per-vertex array once, at construction,
 * from its DenseIndex<G> over the structure as it stood then, and an
 * embedding is fixed for its whole lifetime once built (see
 * EmbeddedGraph.hpp and ForceAtlas.hpp's class docs). There is no
 * replacement API to add a vertex to an already-constructed embedder, so
 * the flag is now rejected with an explanatory error instead of silently
 * doing something else; the rest of this demo (click-highlighting,
 * degree-scaled node sizes, --degree-alpha/--edge-weight-width) is
 * otherwise unaffected.
 *
 * Controls:
 *   R      - run one relaxation step
 *   space  - toggle continuous stepping
 *   h/l    - decrease/increase ideal edge length
 *   j/k    - decrease/increase gravity k
 *   N/M    - decrease/increase Barnes-Hut theta
 *   O      - toggle overlap prevention (off at start; enable once the layout
 * settles)
 *   [/]    - decrease/increase radius base (r(v) = base * (1 +
 * perDegree*sqrt(degree(v))); scaling base preserves relative radius
 * differences between vertices) F      - fit view S      - toggle stats overlay
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
 *   -G, --grow                    REMOVED -- gviz has no dynamic-graph-growth
 *                                 support anymore; this flag now errors out
 *                                 (see the file header comment)
 *   -m, --model {linlog|fr}       force model (default linlog)
 *   -o, --screenshot PATH         save a .ppm screenshot after settling and
 * exit
 *       --degree-alpha            fade edges by max endpoint degree (default
 * off)
 *       --no-degree-alpha         disable degree-based edge opacity
 *   -w, --edge-width WIDTH        base edge thickness (default 1.5); with
 *                                 --edge-weight-width, this is the thickness
 *                                 drawn for an average-weight edge
 *       --edge-weight-width       scale edge thickness by edge weight (default
 * off)
 *       --no-edge-weight-width    disable weight-based edge thickness
 *   -h, --help                    print this help and exit
 */

#include "grender/grStepProfiling.h"
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
#define DEMO_RADIUS_BASE 1
#define DEMO_RADIUS_PER_DEGREE 5
#define DEMO_RADIUS_BASE_STEP 0.1
#define DEMO_RADIUS_BASE_MIN 0.0

using gviz::layout::ForceAtlas;

/*
 * Renderer + shadow config state needed by the live-tuning action handlers.
 * The old C ForceAtlas struct exposed gravityK/edgeLength/theta/radiusBase/
 * radiusPerDegree as plain fields the demo read back directly for its
 * printf readouts; the ported ForceAtlas class only offers one-way
 * Configure*() setters (see ForceAtlas.hpp), so this demo keeps its own
 * shadow copy of whatever it last configured instead -- a self-contained
 * demo-only adaptation, not a gap the renderer or gviz need to fill.
 *
 * nodeStyle is the renderer's own base style, shadowed here too: radiusBase
 * changing live via '['/']' has to update nodeStyle.radius and re-push it
 * with grRendererSetNodeStyle, and that call needs the rest of the style
 * (fill color, size mode, pixel limits, ...) intact, not just the radius.
 */
typedef struct DemoConfig {
  grRenderer *r = nullptr;
  grNodeStyle nodeStyle{};
  double gravityK = DEMO_GRAVITY_K_DEFAULT;
  double edgeLength = ForceAtlas<gviz::Subgraph>::kEdgeLengthDefault;
  double theta = ForceAtlas<gviz::Subgraph>::kThetaDefault;
  double radiusBase = DEMO_RADIUS_BASE;
  double radiusPerDegree = DEMO_RADIUS_PER_DEGREE;
} DemoConfig;

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
  auto &fa = static_cast<ForceAtlas<gviz::Subgraph> &>(eg);
  cfg->gravityK += DEMO_GRAVITY_K_STEP;
  fa.ConfigureGravity(cfg->gravityK);
  printf("gravity k: %f\n", cfg->gravityK);
}

static void actionGravityDown(gviz::layout::EmbeddedGraph &eg, void *userData,
                              const gviz::layout::ActionPayload &payload) {
  (void)payload;
  DemoConfig *cfg = (DemoConfig *)userData;
  auto &fa = static_cast<ForceAtlas<gviz::Subgraph> &>(eg);
  cfg->gravityK = fmax(cfg->gravityK - DEMO_GRAVITY_K_STEP, 0.0);
  fa.ConfigureGravity(cfg->gravityK);
  printf("gravity k: %f\n", cfg->gravityK);
}

static void actionEdgeLengthUp(gviz::layout::EmbeddedGraph &eg, void *userData,
                               const gviz::layout::ActionPayload &payload) {
  (void)payload;
  DemoConfig *cfg = (DemoConfig *)userData;
  auto &fa = static_cast<ForceAtlas<gviz::Subgraph> &>(eg);
  cfg->edgeLength += DEMO_EDGE_LENGTH_STEP;
  fa.Configure(cfg->edgeLength, 0);
  printf("edge length: %f\n", cfg->edgeLength);
}

static void actionEdgeLengthDown(gviz::layout::EmbeddedGraph &eg,
                                 void *userData,
                                 const gviz::layout::ActionPayload &payload) {
  (void)payload;
  DemoConfig *cfg = (DemoConfig *)userData;
  auto &fa = static_cast<ForceAtlas<gviz::Subgraph> &>(eg);
  cfg->edgeLength =
      fmax(cfg->edgeLength - DEMO_EDGE_LENGTH_STEP, DEMO_EDGE_LENGTH_MIN);
  fa.Configure(cfg->edgeLength, 0);
  printf("edge length: %f\n", cfg->edgeLength);
}

static void actionThetaUp(gviz::layout::EmbeddedGraph &eg, void *userData,
                          const gviz::layout::ActionPayload &payload) {
  (void)payload;
  DemoConfig *cfg = (DemoConfig *)userData;
  auto &fa = static_cast<ForceAtlas<gviz::Subgraph> &>(eg);
  cfg->theta += DEMO_THETA_STEP;
  fa.ConfigureBarnesHut(cfg->theta, 0);
  printf("theta: %f\n", cfg->theta);
}

static void actionThetaDown(gviz::layout::EmbeddedGraph &eg, void *userData,
                            const gviz::layout::ActionPayload &payload) {
  (void)payload;
  DemoConfig *cfg = (DemoConfig *)userData;
  auto &fa = static_cast<ForceAtlas<gviz::Subgraph> &>(eg);
  cfg->theta = fmax(cfg->theta - DEMO_THETA_STEP, DEMO_THETA_MIN);
  fa.ConfigureBarnesHut(cfg->theta, 0);
  printf("theta: %f\n", cfg->theta);
}

static void actionRadiusBaseUp(gviz::layout::EmbeddedGraph &eg, void *userData,
                               const gviz::layout::ActionPayload &payload) {
  (void)payload;
  DemoConfig *cfg = (DemoConfig *)userData;
  auto &fa = static_cast<ForceAtlas<gviz::Subgraph> &>(eg);
  cfg->radiusBase += DEMO_RADIUS_BASE_STEP;
  fa.ConfigureRadius(cfg->radiusBase, cfg->radiusPerDegree);
  cfg->nodeStyle.radius = (float)cfg->radiusBase;
  grRendererSetNodeStyle(cfg->r, &cfg->nodeStyle);
  printf("radius base: %f\n", cfg->radiusBase);
}

static void actionRadiusBaseDown(gviz::layout::EmbeddedGraph &eg,
                                 void *userData,
                                 const gviz::layout::ActionPayload &payload) {
  (void)payload;
  DemoConfig *cfg = (DemoConfig *)userData;
  auto &fa = static_cast<ForceAtlas<gviz::Subgraph> &>(eg);
  cfg->radiusBase =
      fmax(cfg->radiusBase - DEMO_RADIUS_BASE_STEP, DEMO_RADIUS_BASE_MIN);
  fa.ConfigureRadius(cfg->radiusBase, cfg->radiusPerDegree);
  cfg->nodeStyle.radius = (float)cfg->radiusBase;
  grRendererSetNodeStyle(cfg->r, &cfg->nodeStyle);
  printf("radius base: %f\n", cfg->radiusBase);
}

static void
actionToggleOverlapPrevention(gviz::layout::EmbeddedGraph &eg, void *userData,
                              const gviz::layout::ActionPayload &payload) {
  (void)userData;
  (void)payload;
  auto &fa = static_cast<ForceAtlas<gviz::Subgraph> &>(eg);
  fa.SetPreventOverlapEnabled(!fa.PreventOverlapEnabled());
  printf("prevent overlap: %s\n", fa.PreventOverlapEnabled() ? "on" : "off");
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
static std::optional<gviz::Graph> loadNamedGraph(const char *name,
                                                 bool directed) {
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
      "  -e, --edge-connectivity C   extra-edge probability in [0, 1] (default "
      "0;\n"
      "                              ignored with -g/--graph)\n"
      "  -g, --graph NAME            load <gviz-data>/NAME/data.gexf or\n"
      "                              data.edges instead of a random graph\n"
      "  -d, --directed              interpret the -g/--graph file's edges as\n"
      "                              directed (default: undirected; ignored\n"
      "                              without -g/--graph)\n"
      "  -G, --grow                  REMOVED -- gviz has no dynamic-graph-\n"
      "                              growth support anymore; errors out\n"
      "  -m, --model {linlog|fr}     force model (default linlog)\n"
      "  -o, --screenshot PATH       save a .ppm screenshot after settling and "
      "exit\n"
      "      --degree-alpha          fade edges by max endpoint degree "
      "(default off)\n"
      "      --no-degree-alpha       disable degree-based edge opacity\n"
      "  -w, --edge-width WIDTH      base edge thickness (default 1.5); with\n"
      "                              --edge-weight-width, this is the "
      "thickness\n"
      "                              drawn for an average-weight edge\n"
      "      --edge-weight-width     scale edge thickness by edge weight "
      "(default off)\n"
      "      --no-edge-weight-width  disable weight-based edge thickness\n"
      "      --no-barnes-hut         exact O(V^2) repulsion instead of the "
      "Barnes-Hut\n"
      "                              approximation (default: Barnes-Hut on)\n"
      "  -h, --help                  print this help and exit\n"
      "\n"
      "Controls:\n"
      "  R      - run one relaxation step\n"
      "  space  - toggle continuous stepping\n"
      "  h/l    - decrease/increase ideal edge length\n"
      "  j/k    - decrease/increase gravity k\n"
      "  N/M    - decrease/increase Barnes-Hut theta\n"
      "  O      - toggle overlap prevention (off at start; enable once the "
      "layout settles)\n"
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
  OPT_NO_BARNES_HUT,
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
    {"no-barnes-hut", no_argument, NULL, OPT_NO_BARNES_HUT},
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
  bool barnesHut = true;

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
    case OPT_NO_BARNES_HUT:
      barnesHut = false;
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
  if (grow) {
    // See this file's header comment: gviz's embedders have no
    // dynamic-graph-growth/Sync() support anymore, so there is no way to
    // add a vertex to an already-constructed ForceAtlas.
    fprintf(stderr,
            "-G/--grow is no longer supported: gviz's embedders have no "
            "dynamic-graph-growth/Sync() support anymore (an embedding is "
            "fixed once constructed) -- see this file's header comment\n");
    return 1;
  }

  std::optional<gviz::Graph> graphOpt;
  if (graphName) {
    graphOpt = loadNamedGraph(graphName, directed);
    if (!graphOpt)
      return 1;
  } else {
    graphOpt =
        gviz::graphs::BuildRandomConnectedGraph(N, edgeConnectivity, seed);
  }
  gviz::Graph &graph = *graphOpt;

  graph.BuildLayout();
  if (graphName)
    printf("loaded %zu vertices, %zu edges\n", graph.Size(), graph.EdgeCount());
  std::optional<gviz::Subgraph> sg;
  sg.emplace(gviz::Subgraph::CreateFull(graph));

  std::unique_ptr<gviz::layout::ForceModel> model =
      useLinLog ? std::unique_ptr<gviz::layout::ForceModel>(
                      std::make_unique<gviz::layout::LinLog>())
                : std::unique_ptr<gviz::layout::ForceModel>(
                      std::make_unique<gviz::layout::FruchtermanReingold>());

  std::optional<ForceAtlas<gviz::Subgraph>> fe;
  try {
    fe.emplace(std::move(*sg), 2, std::move(model));
  } catch (const std::exception &e) {
    fprintf(stderr, "force embedder init failed: %s\n", e.what());
    return 1;
  }
  fe->SetBarnesHutEnabled(barnesHut);

  DemoConfig cfg;
  cfg.gravityK = DEMO_GRAVITY_K_DEFAULT;
  cfg.radiusBase = DEMO_RADIUS_BASE;
  cfg.radiusPerDegree = DEMO_RADIUS_PER_DEGREE;
  fe->ConfigureGravity(cfg.gravityK);
  fe->ConfigureRadius(cfg.radiusBase, cfg.radiusPerDegree);
  fe->Begin(seed);

  bool autoStep = false;
  fe->AddAction("demo.toggleAuto", actionToggleAuto, &autoStep);
  fe->AddAction("demo.gravityUp", actionGravityUp, &cfg);
  fe->AddAction("demo.gravityDown", actionGravityDown, &cfg);
  fe->AddAction("demo.edgeLengthUp", actionEdgeLengthUp, &cfg);
  fe->AddAction("demo.edgeLengthDown", actionEdgeLengthDown, &cfg);
  fe->AddAction("demo.thetaUp", actionThetaUp, &cfg);
  fe->AddAction("demo.thetaDown", actionThetaDown, &cfg);
  fe->AddAction("demo.toggleOverlapPrevention", actionToggleOverlapPrevention,
                NULL);
  fe->AddAction("demo.radiusBaseUp", actionRadiusBaseUp, &cfg);
  fe->AddAction("demo.radiusBaseDown", actionRadiusBaseDown, &cfg);

  grRendererDesc desc;
  grRendererDescInit(&desc);
  desc.title = "grender - Force Embedder (R: step, space: auto, h/l: edge len, "
               "j/k: gravity, N/M: theta, O: overlap)";
  desc.nodeStyle.radius = (float)cfg.radiusBase;
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
  desc.edgeStyle.width = edgeWidth;
  desc.edgeDegreeAlpha = degreeAlpha;
  desc.edgeWeightWidth = edgeWeightWidth;

  grRenderer *r = grRendererCreate(&desc);
  if (!r) {
    fprintf(stderr, "renderer creation failed\n");
    return 1;
  }

  if (grRendererSetGraph(r, fe->Structure(), *fe, &graph) < 0) {
    fprintf(stderr, "graph attach failed\n");
    grRendererDestroy(r);
    return 1;
  }

  // Vertex string data (gexf attributes) is only present when -g/--graph
  // loaded a .gexf file, in which case each non-null entry is a
  // heap-allocated std::string* (see gviz::io::LoadFromGexfFile); entries
  // are NULL otherwise, which the overlay simply skips. Freed alongside
  // graph teardown below, once the renderer (the only reader of these
  // pointers) is destroyed. grRendererSetVertexLabels is LOCAL-indexed (see
  // grRendererSetGraph's INDEXING CONVENTION); translate to the graph's own
  // raw id first.
  size_t vertexLabelCount = fe->PositionCount();
  std::vector<const char *> vertexLabels(vertexLabelCount ? vertexLabelCount
                                                          : 1);
  for (size_t i = 0; i < vertexLabelCount; i++) {
    void *data = graph.GetVertexData(grRendererLocalToRaw(r, i));
    vertexLabels[i] =
        data ? static_cast<const std::string *>(data)->c_str() : NULL;
  }
  grRendererSetVertexLabels(r, vertexLabels.data(), vertexLabelCount);

  cfg.r = r;
  cfg.nodeStyle = desc.nodeStyle;
  grRendererSetNodeDegreeScale(r, true, (float)cfg.radiusPerDegree);
  grRendererParseVertexColors(r);


  /* --degree-alpha / --edge-weight-width / degree-scaled node sizes need no
   * uploads here: with the flags set above, the renderer derives degrees
   * and per-edge weights from the graph itself on every structural change,
   * so they stay correct as the graph grows -- radiusBase changing live via
   * '['/']' only needs grRendererSetNodeStyle (see actionRadiusBaseUp/Down),
   * not a per-vertex re-derivation. */

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
  grRendererFitView(r);

  const size_t stepsBeforeShot = screenshotPath ? 300 : SIZE_MAX;
  size_t totalSteps = 0;

  while (grRendererFrame(r)) {
    if (autoStep) {
      for (size_t i = 0; i < 100; i++) {
        GR_PROF_STEP_BEGIN();
        fe->Step();
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
