/**
 * Live Barnes-Hut force embedding demo, using gviz's configurable
 * gvizForceEmbedder framework (LinLog or Fruchterman-Reingold, gravity,
 * Barnes-Hut approximation).
 *
 * A random spanning-tree-plus-extra-edges graph (edge connectivity
 * configurable) is laid out while grender draws every frame. The embedder
 * registers "forceEmbedder.step" on its embedded graph; this app merely
 * binds a key to that name without knowing what it does. Node radii are
 * scaled by degree via gvizForceEmbedderConfigureRadius, and the per-vertex
 * radii are handed to grRendererSetNodeSizes so what's drawn matches the
 * circles the physics can be told to keep apart. Overlap prevention (which
 * makes repulsion actually respect those radii) starts off and is toggled
 * live with 'O' once the layout has roughly settled, mirroring Gephi's
 * "Prevent Overlap" checkbox.
 *
 * With -G/--grow the graph is dynamic: the embedder is built over a
 * whole-graph vertex-induced view (no up-front edge-bitset layout), and 'A'
 * adds a vertex wired to one or two random existing vertices directly on
 * the gvizGraph, committed with gvizForceEmbedderSync -- the new vertex
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

#include "embedders/gvizEmbeddedGraph.h"
#include "grender/grender.h"

#include "ds/gvizGraph.h"
#include "ds/gvizSubgraph.h"
#include "embedders/gvizForceEmbedder.h"
#include "utils/graphLoader.h"
#include "utils/graphs.h"

#include <getopt.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

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

/* Renderer + scratch buffer needed to re-upload node sizes after a live
 * radius change; gvizEmbeddedGraphAddAction's userData is the natural way to
 * hand these to an action handler, since the handler only otherwise gets the
 * embedded graph itself. */
typedef struct RadiusControl {
  grRenderer *r;
  float *radii; /* scratch, sized fe.vertexCount, reused across updates */
} RadiusControl;

static void refreshNodeSizes(gvizForceEmbedderState *state, RadiusControl *ctl) {
  for (size_t i = 0; i < state->vertexCount; i++)
    ctl->radii[state->vertices[i]] = (float)gvizForceEmbedderVertexRadius(state, i);
  grRendererSetNodeSizes(ctl->r, ctl->radii, state->vertexCount);
}

static void actionToggleAuto(gvizEmbeddedGraph *eg, void *userData,
                             const gvizActionPayload *payload) {
  (void)eg;
  (void)payload;
  bool *autoStep = userData;
  *autoStep = !*autoStep;
  printf("auto step: %s\n", *autoStep ? "on" : "off");
}

static void actionGravityUp(gvizEmbeddedGraph *eg, void *userData,
                            const gvizActionPayload *payload) {
  (void)userData;
  (void)payload;
  gvizForceEmbedderState *state = (gvizForceEmbedderState *)eg;
  gvizForceEmbedderConfigureGravity(state, state->gravityK + DEMO_GRAVITY_K_STEP);
  printf("gravity k: %f\n", state->gravityK);
}

static void actionGravityDown(gvizEmbeddedGraph *eg, void *userData,
                              const gvizActionPayload *payload) {
  (void)userData;
  (void)payload;
  gvizForceEmbedderState *state = (gvizForceEmbedderState *)eg;
  double k = fmax(state->gravityK - DEMO_GRAVITY_K_STEP, 0.0);
  gvizForceEmbedderConfigureGravity(state, k);
  printf("gravity k: %f\n", state->gravityK);
}

static void actionEdgeLengthUp(gvizEmbeddedGraph *eg, void *userData,
                               const gvizActionPayload *payload) {
  (void)userData;
  (void)payload;
  gvizForceEmbedderState *state = (gvizForceEmbedderState *)eg;
  gvizForceEmbedderConfigure(state, state->edgeLength + DEMO_EDGE_LENGTH_STEP, 0);
  printf("edge length: %f\n", state->edgeLength);
}

static void actionEdgeLengthDown(gvizEmbeddedGraph *eg, void *userData,
                                 const gvizActionPayload *payload) {
  (void)userData;
  (void)payload;
  gvizForceEmbedderState *state = (gvizForceEmbedderState *)eg;
  double edgeLength =
      fmax(state->edgeLength - DEMO_EDGE_LENGTH_STEP, DEMO_EDGE_LENGTH_MIN);
  gvizForceEmbedderConfigure(state, edgeLength, 0);
  printf("edge length: %f\n", state->edgeLength);
}

static void actionThetaUp(gvizEmbeddedGraph *eg, void *userData,
                          const gvizActionPayload *payload) {
  (void)userData;
  (void)payload;
  gvizForceEmbedderState *state = (gvizForceEmbedderState *)eg;
  gvizForceEmbedderConfigureBarnesHut(state, state->theta + DEMO_THETA_STEP, 0);
  printf("theta: %f\n", state->theta);
}

static void actionThetaDown(gvizEmbeddedGraph *eg, void *userData,
                            const gvizActionPayload *payload) {
  (void)userData;
  (void)payload;
  gvizForceEmbedderState *state = (gvizForceEmbedderState *)eg;
  double theta = fmax(state->theta - DEMO_THETA_STEP, DEMO_THETA_MIN);
  gvizForceEmbedderConfigureBarnesHut(state, theta, 0);
  printf("theta: %f\n", state->theta);
}

static void actionRadiusBaseUp(gvizEmbeddedGraph *eg, void *userData,
                               const gvizActionPayload *payload) {
  (void)payload;
  gvizForceEmbedderState *state = (gvizForceEmbedderState *)eg;
  RadiusControl *ctl = userData;
  gvizForceEmbedderConfigureRadius(state, state->radiusBase + DEMO_RADIUS_BASE_STEP,
                                   state->radiusPerDegree);
  refreshNodeSizes(state, ctl);
  printf("radius base: %f\n", state->radiusBase);
}

static void actionRadiusBaseDown(gvizEmbeddedGraph *eg, void *userData,
                                 const gvizActionPayload *payload) {
  (void)payload;
  gvizForceEmbedderState *state = (gvizForceEmbedderState *)eg;
  RadiusControl *ctl = userData;
  double base = fmax(state->radiusBase - DEMO_RADIUS_BASE_STEP, DEMO_RADIUS_BASE_MIN);
  gvizForceEmbedderConfigureRadius(state, base, state->radiusPerDegree);
  refreshNodeSizes(state, ctl);
  printf("radius base: %f\n", state->radiusBase);
}

static void actionToggleOverlapPrevention(gvizEmbeddedGraph *eg, void *userData,
                                          const gvizActionPayload *payload) {
  (void)userData;
  (void)payload;
  gvizForceEmbedderState *state = (gvizForceEmbedderState *)eg;
  gvizForceEmbedderSetPreventOverlapEnabled(state, !state->preventOverlap);
  printf("prevent overlap: %s\n", state->preventOverlap ? "on" : "off");
}

/* Everything actionGrowVertex needs: the mutable parent graph (mutated
 * directly, per gviz's commit model), an RNG stream of its own, and a flag
 * for the main loop to refresh node sizes AFTER the renderer has grown its
 * per-vertex buffers -- grRendererSetNodeSizes requires count to match the
 * renderer's position capacity, which only catches up on the frame
 * following the commit. */
typedef struct GrowControl {
  gvizGraph *graph;
  unsigned int rng;
  bool sizesDirty;
} GrowControl;

/* Adds one vertex wired to one or two random existing vertices directly on
 * the gvizGraph, then commits with gvizForceEmbedderSync: the vertex
 * becomes visible and simulated together, already placed near its
 * neighbors. Wiring the edges BEFORE the Sync is what makes that placement
 * possible (see gvizForceEmbedderSync's contract). */
static void actionGrowVertex(gvizEmbeddedGraph *eg, void *userData,
                             const gvizActionPayload *payload) {
  (void)payload;
  GrowControl *grow = userData;
  gvizForceEmbedderState *state = (gvizForceEmbedderState *)eg;

  size_t before = gvizGraphSize(grow->graph);
  if (before == 0 || gvizGraphAddVertex(grow->graph, NULL, NULL, NULL) < 0)
    return;
  size_t newId = before;
  size_t target = (size_t)rand_r(&grow->rng) % before;
  gvizGraphAddEdge(grow->graph, newId, target, 1.0);
  if (before > 1 && rand_r(&grow->rng) % 2 == 0) {
    size_t second = (size_t)rand_r(&grow->rng) % before;
    if (second != target)
      gvizGraphAddEdge(grow->graph, newId, second, 1.0);
  }

  if (gvizForceEmbedderSync(state, (unsigned int)rand_r(&grow->rng) | 1u) < 0) {
    fprintf(stderr, "sync failed; vertex %zu joins on a later sync\n", newId);
    return;
  }
  grow->sizesDirty = true;
  printf("added vertex %zu (%zu vertices)\n", newId,
         gvizGraphSize(grow->graph));
}

static int parseModel(const char *arg, gvizForceModelKind *out) {
  if (!arg || strcasecmp(arg, "linlog") == 0) {
    *out = GVIZ_FORCE_MODEL_LINLOG;
    return 0;
  }
  if (strcasecmp(arg, "fr") == 0) {
    *out = GVIZ_FORCE_MODEL_FRUCHTERMAN_REINGOLD;
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
 * iff @p directed is non-zero. @p out must be uninitialized on entry,
 * matching gvizGraphLoadFromGexfFile/gvizGraphLoadFromEdgesFile.
 *
 * @return 0 on success, -1 if neither file exists or loading failed.
 */
static int loadNamedGraph(const char *name, int directed, gvizGraph *out) {
  char path[1024];

  snprintf(path, sizeof(path), "%s/%s/data.gexf", GRENDER_GVIZ_DATA_DIR, name);
  if (fileExists(path)) {
    printf("loading %s...\n", path);
    return gvizGraphLoadFromGexfFile(path, directed, out);
  }

  snprintf(path, sizeof(path), "%s/%s/data.edges", GRENDER_GVIZ_DATA_DIR, name);
  if (fileExists(path)) {
    gvizEdgesFileOptions opts;
    gvizEdgesFileOptionsInit(&opts);
    opts.directed = directed;
    printf("loading %s...\n", path);
    return gvizGraphLoadFromEdgesFile(path, &opts, out);
  }

  fprintf(stderr, "no data.gexf or data.edges found under %s/%s\n",
          GRENDER_GVIZ_DATA_DIR, name);
  return -1;
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
      "                              committed via gvizForceEmbedderSync\n"
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
  gvizForceModelKind model = GVIZ_FORCE_MODEL_LINLOG;
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
      if (parseModel(optarg, &model) < 0) {
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
  gvizGraph graph;
  if (graphName) {
    if (loadNamedGraph(graphName, directed, &graph) < 0)
      return 1;
  } else {
    graph = build_random_connected_graph(N, edgeConnectivity, seed);
  }

  gvizSubgraph sg;
  if (grow) {
    /* Dynamic mode: whole-graph vertex-induced view. No edge-bitset layout
     * is built up front, so each growth commit is amortized O(1) capacity
     * work plus the embedding's CSR rebuild (see gvizEmbeddedGraph.h's
     * GROWTH & SYNC section). Click-picking still works: the renderer
     * refreshes the layout on demand (gvizGraphEnsureLayout) when a click
     * needs a pick subgraph. */
    gvizVertexSubset vs = gvizVertexSubsetCreateEmpty(&graph);
    sg = gvizSubgraphCreateVertexInduced(&graph, vs);
    for (size_t i = 0; i < gvizGraphSize(&graph); i++)
      gvizSubgraphShowVertex(&sg, i);
    if (graphName)
      printf("loaded %zu vertices\n", gvizGraphSize(&graph));
  } else {
    gvizGraphBuildLayout(&graph);
    if (graphName)
      printf("loaded %zu vertices, %zu edges\n", gvizGraphSize(&graph),
             gvizGraphEdgeCount(&graph));
    sg = gvizSubgraphCreateFull(&graph);
  }

  gvizForceEmbedderState fe = {0};
  if (gvizForceEmbedderInit(&fe, sg, 2, model) < 0) {
    fprintf(stderr, "force embedder init failed\n");
    gvizGraphFreeVertexDataStrings(&graph);
    gvizGraphRelease(&graph);
    return 1;
  }
  gvizForceEmbedderSetBarnesHutEnabled(&fe, 1);
  gvizForceEmbedderConfigureGravity(&fe, DEMO_GRAVITY_K_DEFAULT);
  gvizForceEmbedderConfigureRadius(&fe, DEMO_RADIUS_BASE, DEMO_RADIUS_PER_DEGREE);
  gvizForceEmbedderBegin(&fe, seed);

  gvizEmbeddedGraph *eg = (gvizEmbeddedGraph *)&fe;
  bool autoStep = false;
  RadiusControl radiusControl = {0};
  GrowControl growControl = {&graph, seed ^ 0x9e3779b9u, false};
  if (grow)
    gvizEmbeddedGraphAddAction(eg, "demo.growVertex", actionGrowVertex,
                               &growControl);
  gvizEmbeddedGraphAddAction(eg, "demo.toggleAuto", actionToggleAuto,
                             &autoStep);
  gvizEmbeddedGraphAddAction(eg, "demo.gravityUp", actionGravityUp, NULL);
  gvizEmbeddedGraphAddAction(eg, "demo.gravityDown", actionGravityDown, NULL);
  gvizEmbeddedGraphAddAction(eg, "demo.edgeLengthUp", actionEdgeLengthUp, NULL);
  gvizEmbeddedGraphAddAction(eg, "demo.edgeLengthDown", actionEdgeLengthDown,
                             NULL);
  gvizEmbeddedGraphAddAction(eg, "demo.thetaUp", actionThetaUp, NULL);
  gvizEmbeddedGraphAddAction(eg, "demo.thetaDown", actionThetaDown, NULL);
  gvizEmbeddedGraphAddAction(eg, "demo.toggleOverlapPrevention",
                             actionToggleOverlapPrevention, NULL);
  gvizEmbeddedGraphAddAction(eg, "demo.radiusBaseUp", actionRadiusBaseUp,
                             &radiusControl);
  gvizEmbeddedGraphAddAction(eg, "demo.radiusBaseDown", actionRadiusBaseDown,
                             &radiusControl);

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
    gvizForceEmbedderRelease(&fe);
    gvizGraphFreeVertexDataStrings(&graph);
    gvizGraphRelease(&graph);
    return 1;
  }

  if (grRendererSetGraph(r, eg) < 0) {
    fprintf(stderr, "graph attach failed\n");
    grRendererDestroy(r);
    gvizForceEmbedderRelease(&fe);
    gvizGraphFreeVertexDataStrings(&graph);
    gvizGraphRelease(&graph);
    return 1;
  }

  // Vertex string data (gexf attributes) is only present when -g/--graph
  // loaded a .gexf file; entries are NULL otherwise, which the overlay
  // simply skips. Freed alongside graph teardown below, once the renderer
  // (the only reader of these pointers) is destroyed.
  size_t vertexLabelCount = gvizEmbeddedGraphPositionCount(eg);
  const char **vertexLabels =
      malloc(sizeof(char *) * (vertexLabelCount ? vertexLabelCount : 1));
  if (vertexLabels) {
    for (size_t i = 0; i < vertexLabelCount; i++)
      vertexLabels[i] = gvizGraphGetVertexData(&graph, i);
    grRendererSetVertexLabels(r, vertexLabels, vertexLabelCount);
  }

  /* Degree is fixed for the embedder's lifetime, but radiusBase can change
   * live via '['/']', so this buffer is kept around (not freed) and reused
   * by refreshNodeSizes on every such change instead of being a one-shot
   * upload. */
  radiusControl.r = r;
  radiusControl.radii =
      malloc(sizeof(float) * gvizEmbeddedGraphPositionCount(eg));
  if (!radiusControl.radii) {
    fprintf(stderr, "radii allocation failed\n");
    free(vertexLabels);
    grRendererDestroy(r);
    gvizForceEmbedderRelease(&fe);
    gvizGraphFreeVertexDataStrings(&graph);
    gvizGraphRelease(&graph);
    return 1;
  }
  refreshNodeSizes(&fe, &radiusControl);

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
      for (size_t i = 0; i < 10; i++)
        gvizForceEmbedderStep(&fe);
    }

    /* Deferred by actionGrowVertex: the renderer's per-vertex buffers only
     * grew during the grRendererFrame call above, so this is the earliest
     * the (raw-id-indexed) radius buffer can be resized and re-uploaded at
     * the matching count. */
    if (growControl.sizesDirty) {
      float *grownRadii = realloc(
          radiusControl.radii,
          sizeof(float) * gvizEmbeddedGraphPositionCount(eg));
      if (grownRadii) {
        radiusControl.radii = grownRadii;
        refreshNodeSizes(&fe, &radiusControl);
        growControl.sizesDirty = false;
      }
    }

    /* Screenshot mode doubles as the dynamic pipeline's end-to-end check:
     * keep stepping and grow a vertex every 30 frames, so the saved frame
     * shows grown vertices that were placed, drawn, and simulated through
     * the mutate-then-commit path. */
    if (grow && screenshotPath) {
      gvizForceEmbedderStep(&fe);
      if (totalSteps % 30 == 29)
        gvizEmbeddedGraphInvokeAction(eg, "demo.growVertex", NULL);
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

  free(radiusControl.radii);
  grRendererDestroy(r);
  free(vertexLabels);
  gvizForceEmbedderRelease(&fe);
  gvizGraphFreeVertexDataStrings(&graph);
  gvizGraphRelease(&graph);
  return 0;
}
