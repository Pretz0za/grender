/**
 * Reingold-Tilford tidy tree layout demo.
 *
 * Builds a directed k-ary tree and lays it out with gviz's gvizEmbeddedTree
 * embedder (Reingold-Tilford), then renders the result with grender.
 *
 * Usage: treeDemo [branching] [depth] [screenshot.ppm]
 *
 * Controls:
 *   F      - fit view
 *   drag   - pan
 *   scroll - zoom
 */

#include "grender/grender.h"

#include "ds/gvizGraph.h"
#include "ds/gvizSubgraph.h"
#include "embedders/gvizEmbeddedTree.h"

#include <stdio.h>
#include <stdlib.h>

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

static int addKarySubtree(gvizGraph *g, size_t parent, size_t branching,
                          size_t remainingDepth) {
  if (remainingDepth == 0)
    return 0;

  for (size_t i = 0; i < branching; i++) {
    size_t child = gvizGraphSize(g);
    if (gvizGraphAddVertex(g, NULL, NULL, NULL) < 0)
      return -1;
    if (gvizGraphAddEdge(g, parent, child) < 0)
      return -1;
    if (addKarySubtree(g, child, branching, remainingDepth - 1) < 0)
      return -1;
  }
  return 0;
}

static int buildKaryTree(gvizGraph *g, size_t branching, size_t depth) {
  size_t n = karyTreeVertexCount(branching, depth);
  if (gvizGraphInitAtCapacity(g, 1, n) < 0)
    return -1;
  if (gvizGraphAddVertex(g, NULL, NULL, NULL) < 0)
    return -1;
  return addKarySubtree(g, 0, branching, depth);
}

int main(int argc, char **argv) {
  size_t branching = argc > 1 ? (size_t)atoi(argv[1]) : 2;
  size_t depth = argc > 2 ? (size_t)atoi(argv[2]) : 7;
  const char *screenshotPath = argc > 3 ? argv[3] : NULL;

  if (branching == 0) {
    fprintf(stderr, "branching must be >= 1\n");
    return 1;
  }

  gvizGraph graph;
  if (buildKaryTree(&graph, branching, depth) < 0) {
    fprintf(stderr, "tree construction failed\n");
    return 1;
  }
  gvizGraphBuildLayout(&graph);

  gvizSubgraph sg = gvizSubgraphCreateFull(&graph);

  gvizEmbeddedTree tree = {0};
  if (gvizEmbeddedTreeRTInit(&tree, sg, 0) < 0) {
    fprintf(stderr, "tree embedder init failed (graph must be a directed tree)\n");
    gvizSubgraphRelease(&sg);
    gvizGraphRelease(&graph);
    return 1;
  }

  if (gvizEmbeddedTreeCalculateOffsets(&tree, 0, 0) < 0) {
    fprintf(stderr, "offset calculation failed\n");
    gvizEmbeddedTreeRTRelease(&tree);
    gvizSubgraphRelease(&sg);
    gvizGraphRelease(&graph);
    return 1;
  }

  double rootPos[2] = {0.0, 0.0};
  if (gvizEmbeddedTreeEmbed(&tree, 0, rootPos) < 0) {
    fprintf(stderr, "embedding failed\n");
    gvizEmbeddedTreeRTRelease(&tree);
    gvizSubgraphRelease(&sg);
    gvizGraphRelease(&graph);
    return 1;
  }

  fprintf(stderr, "embedded %zu-ary tree depth %zu (%zu vertices)\n", branching,
          depth, gvizGraphSize(&graph));
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
    gvizEmbeddedTreeRTRelease(&tree);
    gvizSubgraphRelease(&sg);
    gvizGraphRelease(&graph);
    return 1;
  }
  if (grRendererSetGraph(r, (gvizEmbeddedGraph *)&tree) < 0) {
    fprintf(stderr, "graph attach failed\n");
    grRendererDestroy(r);
    gvizEmbeddedTreeRTRelease(&tree);
    gvizSubgraphRelease(&sg);
    gvizGraphRelease(&graph);
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
  gvizEmbeddedTreeRTRelease(&tree);
  gvizGraphRelease(&graph);
  return 0;
}
