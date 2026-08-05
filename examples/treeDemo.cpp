/**
 * Reingold-Tilford tidy tree layout demo.
 *
 * Builds a directed k-ary tree and lays it out with gviz's
 * gviz::layout::ReingoldTilford embedder, then renders the result with
 * grender.
 *
 * Usage: treeDemo [branching] [depth] [screenshot.ppm]
 *
 * Controls:
 *   F      - fit view
 *   drag   - pan
 *   scroll - zoom
 */

#include "grender/grender.h"

#include "gviz.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>

static size_t karyTreeVertexCount(size_t branching, size_t depth) {
  if (branching <= 1)
    return depth + 1;

  size_t count = 0;
  size_t levelSize = 1;
  for (size_t d = 0; d <= depth; d++) {
    count += levelSize;
    levelSize *= branching;
  }
  return count;
}

static void addKarySubtree(gviz::Graph &g, size_t parent, size_t branching,
                           size_t remainingDepth) {
  if (remainingDepth == 0)
    return;

  for (size_t i = 0; i < branching; i++) {
    size_t child = g.Size();
    g.AddVertex();
    g.AddEdge(parent, child, 1.0);
    addKarySubtree(g, child, branching, remainingDepth - 1);
  }
}

static gviz::Graph buildKaryTree(size_t branching, size_t depth) {
  size_t n = karyTreeVertexCount(branching, depth);
  gviz::Graph g(true, n);
  g.AddVertex();
  addKarySubtree(g, 0, branching, depth);
  return g;
}

int main(int argc, char **argv) {
  size_t branching = argc > 1 ? (size_t)atoi(argv[1]) : 2;
  size_t depth = argc > 2 ? (size_t)atoi(argv[2]) : 7;
  const char *screenshotPath = argc > 3 ? argv[3] : NULL;

  if (branching == 0) {
    fprintf(stderr, "branching must be >= 1\n");
    return 1;
  }

  gviz::Graph graph(true, 1);
  try {
    graph = buildKaryTree(branching, depth);
  } catch (const std::exception &e) {
    fprintf(stderr, "tree construction failed: %s\n", e.what());
    return 1;
  }
  graph.BuildLayout();

  std::optional<gviz::layout::ReingoldTilford> tree;
  try {
    tree.emplace(graph, 0);
  } catch (const gviz::NotATreeError &e) {
    fprintf(stderr, "tree embedder init failed (graph must be a directed tree): %s\n",
            e.what());
    return 1;
  }

  tree->CalculateOffsets(0, 0);
  double rootPos[2] = {0.0, 0.0};
  tree->Embed(0, rootPos);

  fprintf(stderr, "embedded %zu-ary tree depth %zu (%zu vertices)\n", branching,
          depth, graph.Size());
  fflush(stderr);

  grRendererDesc desc;
  grRendererDescInit(&desc);
  desc.title = "grender - Reingold-Tilford tree";
  desc.nodeStyle.radius = 4.0f;
  desc.nodeStyle.fillColor = GR_COLOR(0.55f, 0.82f, 0.65f, 1.0f);
  desc.edgeStyle.color = GR_COLOR(0.45f, 0.60f, 0.55f, 0.55f);
  desc.edgeStyle.width = 1.5f;

  grRenderer *r = grRendererCreate(&desc);
  if (!r) {
    fprintf(stderr, "renderer creation failed\n");
    return 1;
  }
  if (grRendererSetGraph(r, *tree, &graph) < 0) {
    fprintf(stderr, "graph attach failed\n");
    grRendererDestroy(r);
    return 1;
  }

  grRendererFitView(r);

  size_t frames = 0;
  while (grRendererFrame(r)) {
    frames++;
    if (screenshotPath && frames == 30) {
      if (grRendererSaveScreenshot(r, screenshotPath) == 0)
        printf("screenshot saved to %s\n", screenshotPath);
      else
        fprintf(stderr, "screenshot failed\n");
      grRendererRequestClose(r);
    }
  }

  grRendererDestroy(r);
  return 0;
}
