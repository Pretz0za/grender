/**
 * GRIP embedding demo for graphs stored under the gviz data directory.
 *
 * Loads <gviz-data>/<dataset>/data.edges, keeps only the largest connected
 * component, embeds with GRIP in the requested dimension, and renders online
 * while refinement runs.
 *
 * Usage: datasetDemo <dataset> <dim(2|3|4)>
 *
 * Controls:
 *   R      - run one GRIP refinement round
 *   N      - advance to the next (finer) GRIP layer
 *   space  - toggle continuous refinement
 *   F      - fit view
 *   S      - toggle the stats overlay
 *   drag   - pan (2D) / orbit (3D)
 *   scroll - zoom
 */

#include "grender/grender.h"

#include "gviz.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

#ifndef GRENDER_GVIZ_DATA_DIR
#error "GRENDER_GVIZ_DATA_DIR must be defined by CMake"
#endif

static void actionToggleAuto(gviz::layout::EmbeddedGraph &eg, void *userData,
                             const gviz::layout::ActionPayload &payload) {
  (void)eg, (void)payload;
  bool *autoRefine = (bool *)userData;
  *autoRefine = !*autoRefine;
  printf("auto refine: %s\n", *autoRefine ? "on" : "off");
}

static size_t subgraphFirstVertex(const gviz::Subgraph &sg) {
  for (size_t v : sg)
    return v;
  return 0;
}

/* Two-pass BFS diameter estimate: BFS from an arbitrary start to find a far
 * vertex, then BFS from that far vertex to find the eccentricity. @p out
 * (the BFS tree) must be a full subgraph on the same parent as @p sg
 * (gviz::search::BreadthFirst's contract), so this needs @p graph as well
 * as @p sg -- Subgraph deliberately never hands back its own parent. */
static size_t estimateGraphDiameter(const gviz::Graph &graph,
                                    const gviz::Subgraph &sg) {
  size_t n = sg.ParentSize();
  if (n <= 1)
    return 1;

  std::vector<size_t> dist;
  size_t start = subgraphFirstVertex(sg);
  size_t far = start;
  size_t maxDist = 0;

  {
    gviz::Subgraph bfs = gviz::Subgraph::CreateEmpty(graph);
    if (gviz::search::BreadthFirst(sg, bfs, start, 0, &dist)) {
      for (size_t v : sg) {
        if (dist[v] != SIZE_MAX && dist[v] > maxDist) {
          maxDist = dist[v];
          far = v;
        }
      }
    }
  }

  maxDist = 0;
  {
    gviz::Subgraph bfs = gviz::Subgraph::CreateEmpty(graph);
    if (gviz::search::BreadthFirst(sg, bfs, far, 0, &dist)) {
      for (size_t v : sg) {
        if (dist[v] != SIZE_MAX && dist[v] > maxDist)
          maxDist = dist[v];
      }
    }
  }

  return maxDist > 0 ? maxDist : 1;
}

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

  printf("connected components: %zu (using largest: %zu vertices, "
         "ignoring %zu)\n",
         result.count, sizes[largest], n - sizes[largest]);

  gviz::Subgraph vs = gviz::Subgraph::CreateVertexInduced(graph);
  for (size_t v = 0; v < n; v++) {
    if (result.labels[v] == largest)
      vs.ShowVertex(v);
  }
  return vs;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <dataset> <dim(2|3|4)>\n", argv[0]);
    return 1;
  }

  const char *dataset = argv[1];
  size_t dim = (size_t)atoi(argv[2]);
  if (dim != 2 && dim != 3 && dim != 4) {
    fprintf(stderr, "dim must be 2, 3, or 4\n");
    return 1;
  }

  char path[512];
  snprintf(path, sizeof(path), "%s/%s/data.edges", GRENDER_GVIZ_DATA_DIR,
           dataset);

  printf("loading %s...\n", path);
  gviz::Graph graph = [&]() -> gviz::Graph {
    try {
      return gviz::io::LoadFromEdgesFile(path, gviz::io::EdgesFileOptions{});
    } catch (const std::exception &e) {
      fprintf(stderr, "failed to load graph from %s: %s\n", path, e.what());
      exit(1);
    }
  }();
  graph.BuildLayout();
  printf("loaded %zu vertices, %zu edges\n", graph.Size(), graph.EdgeCount());

  std::optional<gviz::Subgraph> sg;
  try {
    sg.emplace(largestComponentSubgraph(graph));
  } catch (const std::exception &e) {
    fprintf(stderr, "connected component analysis failed: %s\n", e.what());
    return 1;
  }
  printf("subgraph: %zu vertices, %zu edges\n", sg->VertexCount(),
         sg->EdgeCount());

  printf("estimating diameter...\n");
  size_t diameter = estimateGraphDiameter(graph, *sg);
  printf("diameter estimate: %zu\n", diameter);

  size_t sgVertexCount = sg->VertexCount();
  std::optional<gviz::layout::GRIP> grip;
  try {
    grip.emplace(std::move(*sg), diameter, dim);
  } catch (const std::exception &e) {
    fprintf(stderr, "GRIP init failed: %s\n", e.what());
    return 1;
  }
  grip->ConfigureK(256, 256, gviz::layout::GRIP::KPolicy::Constant);

  printf("building MIS filtration...\n");
  grip->Begin();
  printf("embedding (%zud)...\n", dim);

  bool autoRefine = true;
  grip->AddAction("demo.toggleAuto", actionToggleAuto, &autoRefine);

  grRendererDesc desc;
  grRendererDescInit(&desc);
  desc.title = "grender - GRIP dataset";
  desc.nodeStyle.radius = sgVertexCount > 100000 ? 1.0f : 2.5f;
  desc.nodeStyle.fillColor = GR_COLOR(0.55f, 0.78f, 1.0f, 1.0f);
  desc.edgeStyle.color = GR_COLOR(0.45f, 0.55f, 0.75f, 0.35f);

  grRenderer *r = grRendererCreate(&desc);
  if (!r) {
    fprintf(stderr, "renderer creation failed\n");
    return 1;
  }
  if (grRendererSetGraph(r, *grip, &graph) < 0) {
    fprintf(stderr, "graph attach failed\n");
    grRendererDestroy(r);
    return 1;
  }

  grRendererBindKey(r, 'R', "grip.refineRound");
  grRendererBindKey(r, 'N', "grip.nextStage");
  grRendererBindKey(r, GR_KEY_SPACE, "demo.toggleAuto");

  const size_t roundsPerStage = SIZE_MAX;
  while (grRendererFrame(r)) {
    if (!autoRefine)
      continue;
    if (grip->CurrentRound() >= roundsPerStage) {
      if (grip->CurrentLayer() == 0)
        autoRefine = false;
      else
        grip->NextStage();
    } else {
      grip->RefineRound();
    }
  }

  grRendererDestroy(r);
  return 0;
}
