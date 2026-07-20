/**
 * Live GRIP embedding demo, configurable through the command line like
 * forceEmbedderDemo: pick a synthetic graph shape (and its size) or load one
 * from gviz/data, control embedding dimension, and tune the GRIP-specific
 * knobs (diameter hint, placement/refinement neighbor counts, k-policy,
 * stats collection) at init time.
 *
 * Demonstrates online rendering (spec 3) and creator-defined actions (spec
 * 4): the GRIP embedder registers "grip.refineRound" and "grip.nextStage" on
 * its embedded graph; this app merely binds keys to those names without
 * knowing what they do.
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
 * Usage: gripDemo [options]
 *   -t, --type NAME             sierpinski|sierpinski-tet|sierpinski-carpet|
 *                               tetra-mesh|rect-mesh|tri-mesh|
 *                               knotted-rect-mesh|mobius|klein-bottle|random
 *                               (default mobius; ignored with -g/--graph)
 *   --rows R                    rows, for mobius/klein-bottle/rect-mesh/
 *                               knotted-rect-mesh types (default 24)
 *   --cols C                    cols, for the same mesh types (default 48)
 *   --depth D                   depth, for sierpinski/sierpinski-tet/
 *                               sierpinski-carpet/tetra-mesh/tri-mesh types
 *                               (default 6)
 *   -n, --vertices N            vertex count, for the random type (default 500)
 *   -e, --edge-density D        edge density in [0, 1], for the random type
 *                               (default 0.01)
 *   -s, --seed SEED              RNG seed, for the random type (default
 *                               time-based)
 *   -g, --graph NAME             load <gviz-data>/NAME/data.gexf or
 *                               data.edges instead of a synthetic graph
 *   -d, --dim {2|3|4}            embedding dimension (default 3)
 *   --diameter D                 GRIP diameter hint sizing internal buffers;
 *                               0 = let GRIP pick a default (default 0)
 *   --k-max K                    placement and refinement neighbor cap
 *                               (default 128; overridden by the two below)
 *   --placement-k K              placement neighbor cap (default 128)
 *   --refinement-k K             refinement neighbor cap (default 128)
 *   --k-policy NAME               constant|layer-decay|layer-grow|
 *                               placement-decay|budget (default constant)
 *   --knn-capacity K              per-vertex KNN storage capacity, fixed at
 *                               init time; placement-k/refinement-k are
 *                               clamped to it (default 256)
 *   --no-stats                    disable GRIP stat collection
 *   -o, --screenshot PATH        save layer screenshots and exit
 *   -h, --help                    print this help and exit
 */

#include "grender/grender.h"

#include "ds/gvizGraph.h"
#include "ds/gvizSubgraph.h"
#include "embedders/gvizGRIPEmbedder.h"
#include "utils/graphLoader.h"
#include "utils/graphs.h"

#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#ifndef GRENDER_GVIZ_DATA_DIR
#error "GRENDER_GVIZ_DATA_DIR must be defined by CMake"
#endif

#define DEMO_ROWS_DEFAULT 24
#define DEMO_COLS_DEFAULT 48
#define DEMO_DEPTH_DEFAULT 6
#define DEMO_VERTICES_DEFAULT 500
#define DEMO_EDGE_DENSITY_DEFAULT 0.01
#define DEMO_KMAX_DEFAULT 128
#define DEMO_KNN_CAPACITY_DEFAULT 256

typedef enum DemoGraphType {
  DEMO_GRAPH_SIERPINSKI,
  DEMO_GRAPH_SIERPINSKI_TET,
  DEMO_GRAPH_SIERPINSKI_CARPET,
  DEMO_GRAPH_TETRA_MESH,
  DEMO_GRAPH_RECT_MESH,
  DEMO_GRAPH_TRI_MESH,
  DEMO_GRAPH_KNOTTED_RECT_MESH,
  DEMO_GRAPH_MOBIUS,
  DEMO_GRAPH_KLEIN_BOTTLE,
  DEMO_GRAPH_RANDOM,
} DemoGraphType;

static void actionToggleAuto(gvizEmbeddedGraph *eg, void *userData,
                             const gvizActionPayload *payload) {
  (void)eg, (void)payload;
  bool *autoRefine = userData;
  *autoRefine = !*autoRefine;
  printf("auto refine: %s\n", *autoRefine ? "on" : "off");
}

static int parseGraphType(const char *arg, DemoGraphType *out) {
  if (!arg || strcasecmp(arg, "mobius") == 0) {
    *out = DEMO_GRAPH_MOBIUS;
  } else if (strcasecmp(arg, "sierpinski") == 0) {
    *out = DEMO_GRAPH_SIERPINSKI;
  } else if (strcasecmp(arg, "sierpinski-tet") == 0) {
    *out = DEMO_GRAPH_SIERPINSKI_TET;
  } else if (strcasecmp(arg, "sierpinski-carpet") == 0) {
    *out = DEMO_GRAPH_SIERPINSKI_CARPET;
  } else if (strcasecmp(arg, "tetra-mesh") == 0) {
    *out = DEMO_GRAPH_TETRA_MESH;
  } else if (strcasecmp(arg, "rect-mesh") == 0) {
    *out = DEMO_GRAPH_RECT_MESH;
  } else if (strcasecmp(arg, "tri-mesh") == 0) {
    *out = DEMO_GRAPH_TRI_MESH;
  } else if (strcasecmp(arg, "knotted-rect-mesh") == 0) {
    *out = DEMO_GRAPH_KNOTTED_RECT_MESH;
  } else if (strcasecmp(arg, "klein-bottle") == 0) {
    *out = DEMO_GRAPH_KLEIN_BOTTLE;
  } else if (strcasecmp(arg, "random") == 0) {
    *out = DEMO_GRAPH_RANDOM;
  } else {
    return -1;
  }
  return 0;
}

static int parseKPolicy(const char *arg, gvizGRIPKPolicy *out) {
  if (!arg || strcasecmp(arg, "constant") == 0) {
    *out = GVIZ_GRIP_K_CONSTANT;
  } else if (strcasecmp(arg, "layer-decay") == 0) {
    *out = GVIZ_GRIP_K_LAYER_DECAY;
  } else if (strcasecmp(arg, "layer-grow") == 0) {
    *out = GVIZ_GRIP_K_LAYER_GROW;
  } else if (strcasecmp(arg, "placement-decay") == 0) {
    *out = GVIZ_GRIP_K_PLACEMENT_DECAY;
  } else if (strcasecmp(arg, "budget") == 0) {
    *out = GVIZ_GRIP_K_BUDGET;
  } else {
    return -1;
  }
  return 0;
}

static gvizGraph buildGraph(DemoGraphType type, size_t rows, size_t cols,
                           size_t depth, size_t numVertices,
                           double edgeDensity, unsigned int seed) {
  switch (type) {
  case DEMO_GRAPH_SIERPINSKI:
    return createSierpinski((int)depth, NULL);
  case DEMO_GRAPH_SIERPINSKI_TET:
    return createSierpinskiTetrahedron((int)depth, NULL);
  case DEMO_GRAPH_SIERPINSKI_CARPET:
    return build_sierpinski_carpet(depth);
  case DEMO_GRAPH_TETRA_MESH:
    return build_tetrahedral_mesh(depth);
  case DEMO_GRAPH_RECT_MESH:
    return build_rect_mesh(rows, cols);
  case DEMO_GRAPH_TRI_MESH:
    return build_equilateral_tri_mesh(depth);
  case DEMO_GRAPH_KNOTTED_RECT_MESH:
    return build_knotted_rect_mesh(rows, cols);
  case DEMO_GRAPH_MOBIUS:
    return build_mobius_strip(rows, cols);
  case DEMO_GRAPH_KLEIN_BOTTLE:
    return build_klein_bottle(rows, cols);
  case DEMO_GRAPH_RANDOM:
    return build_random_connected_graph(numVertices, edgeDensity, seed);
  }
  gvizGraph empty = {0};
  return empty;
}

static bool graphTypeUsesRowsCols(DemoGraphType type) {
  return type == DEMO_GRAPH_RECT_MESH || type == DEMO_GRAPH_KNOTTED_RECT_MESH ||
         type == DEMO_GRAPH_MOBIUS || type == DEMO_GRAPH_KLEIN_BOTTLE;
}

static bool graphTypeUsesDepth(DemoGraphType type) {
  return type == DEMO_GRAPH_SIERPINSKI || type == DEMO_GRAPH_SIERPINSKI_TET ||
         type == DEMO_GRAPH_SIERPINSKI_CARPET || type == DEMO_GRAPH_TETRA_MESH ||
         type == DEMO_GRAPH_TRI_MESH;
}

static int fileExists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/**
 * Loads <GRENDER_GVIZ_DATA_DIR>/<name>/data.gexf if present, else
 * <GRENDER_GVIZ_DATA_DIR>/<name>/data.edges. @p out must be uninitialized on
 * entry, matching gvizGraphLoadFromGexfFile/gvizGraphLoadFromEdgesFile.
 *
 * @return 0 on success, -1 if neither file exists or loading failed.
 */
static int loadNamedGraph(const char *name, gvizGraph *out) {
  char path[1024];

  snprintf(path, sizeof(path), "%s/%s/data.gexf", GRENDER_GVIZ_DATA_DIR, name);
  if (fileExists(path)) {
    printf("loading %s...\n", path);
    return gvizGraphLoadFromGexfFile(path, out);
  }

  snprintf(path, sizeof(path), "%s/%s/data.edges", GRENDER_GVIZ_DATA_DIR, name);
  if (fileExists(path)) {
    gvizEdgesFileOptions opts;
    gvizEdgesFileOptionsInit(&opts);
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
      "Live GRIP embedding demo.\n"
      "\n"
      "Options:\n"
      "  -t, --type NAME             sierpinski|sierpinski-tet|\n"
      "                              sierpinski-carpet|tetra-mesh|rect-mesh|\n"
      "                              tri-mesh|knotted-rect-mesh|mobius|\n"
      "                              klein-bottle|random\n"
      "                              (default mobius; ignored with -g/--graph)\n"
      "  --rows R                    rows, for mobius/klein-bottle/rect-mesh/\n"
      "                              knotted-rect-mesh types (default %d)\n"
      "  --cols C                    cols, for the same mesh types (default %d)\n"
      "  --depth D                   depth, for sierpinski/sierpinski-tet/\n"
      "                              sierpinski-carpet/tetra-mesh/tri-mesh\n"
      "                              types (default %d)\n"
      "  -n, --vertices N            vertex count, for the random type\n"
      "                              (default %d)\n"
      "  -e, --edge-density D        edge density in [0, 1], for the random\n"
      "                              type (default %.2f)\n"
      "  -s, --seed SEED             RNG seed, for the random type\n"
      "                              (default: time-based)\n"
      "  -g, --graph NAME            load <gviz-data>/NAME/data.gexf or\n"
      "                              data.edges instead of a synthetic graph\n"
      "  -d, --dim {2|3|4}           embedding dimension (default 3)\n"
      "  --diameter D                 GRIP diameter hint sizing internal\n"
      "                              buffers; 0 = let GRIP pick a default\n"
      "                              (default 0)\n"
      "  --k-max K                    placement and refinement neighbor cap\n"
      "                              (default %d; overridden by the two below)\n"
      "  --placement-k K              placement neighbor cap (default %d)\n"
      "  --refinement-k K             refinement neighbor cap (default %d)\n"
      "  --k-policy NAME               constant|layer-decay|layer-grow|\n"
      "                              placement-decay|budget (default constant)\n"
      "  --knn-capacity K              per-vertex KNN storage capacity, fixed\n"
      "                              at init time; placement-k/refinement-k\n"
      "                              are clamped to it (default %d)\n"
      "  --no-stats                    disable GRIP stat collection\n"
      "  -o, --screenshot PATH        save layer screenshots and exit\n"
      "  -h, --help                    print this help and exit\n"
      "\n"
      "Controls:\n"
      "  R      - run one GRIP refinement round\n"
      "  N      - advance to the next (finer) GRIP layer\n"
      "  space  - toggle continuous refinement\n"
      "  F      - fit view\n"
      "  S      - toggle the stats overlay\n"
      "  drag   - pan (2D) / orbit (3D)\n"
      "  scroll - zoom\n",
      prog, DEMO_ROWS_DEFAULT, DEMO_COLS_DEFAULT, DEMO_DEPTH_DEFAULT,
      DEMO_VERTICES_DEFAULT, DEMO_EDGE_DENSITY_DEFAULT, DEMO_KMAX_DEFAULT,
      DEMO_KMAX_DEFAULT, DEMO_KMAX_DEFAULT, DEMO_KNN_CAPACITY_DEFAULT);
}

enum {
  OPT_ROWS = 256,
  OPT_COLS,
  OPT_DIAMETER,
  OPT_KMAX,
  OPT_PLACEMENT_K,
  OPT_REFINEMENT_K,
  OPT_KPOLICY,
  OPT_KNN_CAPACITY,
  OPT_NO_STATS,
};

static const struct option kLongOptions[] = {
    {"type", required_argument, NULL, 't'},
    {"rows", required_argument, NULL, OPT_ROWS},
    {"cols", required_argument, NULL, OPT_COLS},
    {"depth", required_argument, NULL, 'D'},
    {"vertices", required_argument, NULL, 'n'},
    {"edge-density", required_argument, NULL, 'e'},
    {"seed", required_argument, NULL, 's'},
    {"graph", required_argument, NULL, 'g'},
    {"dim", required_argument, NULL, 'd'},
    {"diameter", required_argument, NULL, OPT_DIAMETER},
    {"k-max", required_argument, NULL, OPT_KMAX},
    {"placement-k", required_argument, NULL, OPT_PLACEMENT_K},
    {"refinement-k", required_argument, NULL, OPT_REFINEMENT_K},
    {"k-policy", required_argument, NULL, OPT_KPOLICY},
    {"knn-capacity", required_argument, NULL, OPT_KNN_CAPACITY},
    {"no-stats", no_argument, NULL, OPT_NO_STATS},
    {"screenshot", required_argument, NULL, 'o'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};

int main(int argc, char **argv) {
  DemoGraphType type = DEMO_GRAPH_MOBIUS;
  size_t rows = DEMO_ROWS_DEFAULT;
  size_t cols = DEMO_COLS_DEFAULT;
  size_t depth = DEMO_DEPTH_DEFAULT;
  size_t numVertices = DEMO_VERTICES_DEFAULT;
  double edgeDensity = DEMO_EDGE_DENSITY_DEFAULT;
  unsigned int seed = (unsigned int)time(NULL);
  const char *graphName = NULL;
  size_t dim = 3;
  size_t diameter = 0;
  size_t kMax = DEMO_KMAX_DEFAULT;
  size_t placementK = 0;
  size_t refinementK = 0;
  size_t knnCapacity = 0;
  gvizGRIPKPolicy kPolicy = GVIZ_GRIP_K_CONSTANT;
  bool gripStats = true;
  const char *screenshotPath = NULL;

  int opt;
  while ((opt = getopt_long(argc, argv, "t:n:e:s:g:d:o:h", kLongOptions,
                            NULL)) != -1) {
    switch (opt) {
    case 't':
      if (parseGraphType(optarg, &type) < 0) {
        fprintf(stderr, "unknown graph type \"%s\"\n", optarg);
        return 1;
      }
      break;
    case OPT_ROWS:
      rows = (size_t)atoi(optarg);
      break;
    case OPT_COLS:
      cols = (size_t)atoi(optarg);
      break;
    case 'D':
      depth = (size_t)atoi(optarg);
      break;
    case 'n':
      numVertices = (size_t)atoi(optarg);
      break;
    case 'e':
      edgeDensity = strtod(optarg, NULL);
      break;
    case 's':
      seed = (unsigned int)atoi(optarg);
      break;
    case 'g':
      graphName = optarg;
      break;
    case 'd':
      dim = (size_t)atoi(optarg);
      break;
    case OPT_DIAMETER:
      diameter = (size_t)atoi(optarg);
      break;
    case OPT_KMAX:
      kMax = (size_t)atoi(optarg);
      break;
    case OPT_PLACEMENT_K:
      placementK = (size_t)atoi(optarg);
      break;
    case OPT_REFINEMENT_K:
      refinementK = (size_t)atoi(optarg);
      break;
    case OPT_KPOLICY:
      if (parseKPolicy(optarg, &kPolicy) < 0) {
        fprintf(stderr,
                "unknown k-policy \"%s\", expected constant|layer-decay|"
                "layer-grow|placement-decay|budget\n",
                optarg);
        return 1;
      }
      break;
    case OPT_KNN_CAPACITY:
      knnCapacity = (size_t)atoi(optarg);
      break;
    case OPT_NO_STATS:
      gripStats = false;
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

  if (dim != 2 && dim != 3 && dim != 4) {
    fprintf(stderr, "dim must be 2, 3, or 4\n");
    return 1;
  }
  if (!graphName && graphTypeUsesRowsCols(type) && (rows < 2 || cols < 2)) {
    fprintf(stderr, "rows and cols must be >= 2 for this graph type\n");
    return 1;
  }
  if (!graphName && type == DEMO_GRAPH_RANDOM && numVertices == 0) {
    fprintf(stderr, "vertices must be >= 1\n");
    return 1;
  }
  if (!graphName && type == DEMO_GRAPH_RANDOM &&
      (edgeDensity < 0.0 || edgeDensity > 1.0)) {
    fprintf(stderr, "edge density must be in [0, 1]\n");
    return 1;
  }
  if (placementK == 0)
    placementK = kMax;
  if (refinementK == 0)
    refinementK = kMax;
  size_t effectiveKnnCapacity =
      knnCapacity > 0 ? knnCapacity : DEMO_KNN_CAPACITY_DEFAULT;
  if (placementK > effectiveKnnCapacity || refinementK > effectiveKnnCapacity) {
    fprintf(stderr,
            "placement-k/refinement-k (%zu/%zu) must be <= knn-capacity (%zu)\n",
            placementK, refinementK, effectiveKnnCapacity);
    return 1;
  }

  gvizGraph graph;
  if (graphName) {
    if (loadNamedGraph(graphName, &graph) < 0)
      return 1;
  } else {
    graph = buildGraph(type, rows, cols, depth, numVertices, edgeDensity, seed);
    if (!graph.vertices.arr) {
      fprintf(stderr, "graph construction failed\n");
      return 1;
    }
  }
  gvizGraphBuildLayout(&graph);
  printf("graph: %zu vertices, %zu edges\n", gvizGraphSize(&graph),
         gvizGraphEdgeCount(&graph));
  gvizSubgraph sg = gvizSubgraphCreateFull(&graph);

  gvizGRIPState grip = {0};
  if (!gripStats)
    gvizGRIPEmbedderConfigureStats(&grip, false);
  if (knnCapacity > 0)
    gvizGRIPEmbedderConfigureKnnCapacity(&grip, knnCapacity);
  if (gvizGRIPEmbedderInit(&grip, sg, diameter, dim) < 0) {
    fprintf(stderr, "GRIP init failed\n");
    gvizGraphRelease(&graph);
    return 1;
  }
  gvizEmbeddedGraph *eg = (gvizEmbeddedGraph *)&grip;
  gvizGRIPEmbedderConfigureK(&grip, placementK, refinementK, kPolicy);

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
    gvizGRIPEmbedderRelease(&grip);
    gvizGraphRelease(&graph);
    return 1;
  }
  if (grRendererSetGraph(r, eg) < 0) {
    fprintf(stderr, "graph attach failed\n");
    grRendererDestroy(r);
    gvizGRIPEmbedderRelease(&grip);
    gvizGraphRelease(&graph);
    return 1;
  }

  grRendererBindKey(r, 'R', "grip.refineRound");
  grRendererBindKey(r, 'N', "grip.nextStage");
  grRendererBindKey(r, GR_KEY_SPACE, "demo.toggleAuto");

  const size_t roundsPerStage = screenshotPath ? 150 : SIZE_MAX;
  while (grRendererFrame(r)) {
    if (autoRefine) {
      if (grip.currRound >= roundsPerStage) {
        if (screenshotPath) {
          char path[512];
          snprintf(path, sizeof(path), "%s.layer%zu.ppm", screenshotPath,
                   grip.currLayer);
          grRendererFitView(r);
          grRendererFrame(r);
          if (grRendererSaveScreenshot(r, path) == 0)
            printf("screenshot saved to %s\n", path);
          else
            fprintf(stderr, "screenshot failed\n");
        }
        if (grip.currLayer == 0)
          grRendererRequestClose(r);
        else
          gvizGRIPEmbedderNextStage(&grip);
      } else {
        gvizGRIPEmbedderRefineRound(&grip);
      }
    }
  }

  grRendererDestroy(r);
  gvizGRIPEmbedderRelease(&grip);
  gvizGraphRelease(&graph);
  return 0;
}
