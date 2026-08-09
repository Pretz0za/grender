/**
 * Interactive WASM front-end for grender/gviz: unlike sierpinskiDemo.cpp
 * (one hardcoded graph, one hardcoded embedder), this app exposes no fixed
 * demo content of its own at all -- every graph and every embedder run is
 * driven entirely from JS through the EMSCRIPTEN_KEEPALIVE functions below.
 * The companion page is wasm/webDemo.html + wasm/webDemoUI.js.
 *
 * Two-phase graph/embedder lifecycle, chosen to avoid ever leaving the
 * renderer's attached-graph pointer dangling across a yield back to the
 * browser event loop (grRenderer has no "detach graph" call, so once
 * grRendererSetGraph has pointed it at something, that something must stay
 * alive until the NEXT successful grRendererSetGraph):
 *
 *   1. A webGraphStage*() call builds a fresh gviz::Graph into g.pending,
 *      entirely independent of whatever is currently attached to the
 *      renderer. Building or discarding a pending graph never touches the
 *      live embedder/renderer state.
 *   2. A webRun*() call builds the new embedder (Subgraph + embedder object)
 *      OVER g.pending into local temporaries first, entirely before
 *      touching any live state ("build into temporaries, publish only on
 *      success" -- the same pattern ForceAtlas::Sync uses internally, see
 *      ForceAtlas.hpp). Only once that construction (and, for embedders
 *      with a meaningful Begin()/Embed() step, that too) has succeeded does
 *      it tear down the previous embedder, publish the new graph/embedder
 *      into g, and call grRendererSetGraph. A failure (e.g. GRIP's
 *      InsufficientVerticesError, Tutte's PlanarNotPlanarError) leaves
 *      whatever was previously running completely untouched.
 *
 * All of this happens synchronously inside one JS-to-wasm call, so there is
 * no window during which the renderer's attached-graph pointer could be
 * observed dangling by the browser's requestAnimationFrame-driven frame
 * loop (grRendererFrame only ever runs between JS turns, via
 * emscripten_set_main_loop).
 *
 * Kind-dispatch instead of a shared base-class handle: EmbeddedGraph
 * deliberately has no virtual methods besides its destructor (see its class
 * comment -- "dispatch doesn't exist, a front-end picks a concrete type
 * once"), so this app -- being exactly that front-end -- holds one
 * std::optional<T> per concrete embedder type (mirroring every native demo's
 * std::optional<Embedder> local) and a Kind enum saying which one is
 * currently populated, rather than reaching for artificial virtual dispatch
 * gviz deliberately doesn't provide.
 */

#include "grender/grender.h"

#include "gviz.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

enum class Kind {
  None = 0,
  GRIP = 1,
  ForceAtlas = 2,
  Tutte = 3,
  SpringTutte = 4,
  ReingoldTilford = 5,
  // Note: Planar is deliberately not exposed here -- it's still WIP
  // elsewhere in gviz, not ready to surface through this UI.
};

struct AppState {
  grRenderer *renderer = nullptr;

  // The graph currently attached to the renderer (via whichever embedder is
  // live) -- must outlive that embedder, see the file header.
  std::unique_ptr<gviz::Graph> graph;
  // Staged by webGraphStage*(), consumed (moved into `graph`) by the next
  // successful webRun*() call. Independent of `graph`/the live embedder.
  std::unique_ptr<gviz::Graph> pending;

  Kind kind = Kind::None;
  std::optional<gviz::layout::GRIP> grip;
  std::optional<gviz::layout::ForceAtlas> forceAtlas;
  std::optional<gviz::layout::Tutte> tutte;
  std::optional<gviz::layout::SpringTutte> springTutte;
  std::optional<gviz::layout::ReingoldTilford> reingoldTilford;

  bool autoRun = false;

  // Scratch reused by RefreshForceAtlasRadii, mirroring forceEmbedderDemo's
  // DemoConfig::radii -- see that demo's comment on why compact index ==
  // raw vertex id here (this app always runs ForceAtlas over a full,
  // dense subgraph, never a grown/sparse one).
  std::vector<float> faRadii;

  std::string lastError;
};

AppState g;

// ---------------------------------------------------------------------------
// Graph staging helpers
// ---------------------------------------------------------------------------

/** Finalizes @p built (gives it a layout, matching every native demo's
 *  graph.BuildLayout() call right after construction) and installs it as
 *  the pending graph, discarding whatever was pending before. */
void SetPending(gviz::Graph &&built) {
  built.BuildLayout();
  g.pending = std::make_unique<gviz::Graph>(std::move(built));
}

/** Tears down whichever embedder is currently live (if any) and detaches
 *  auto-run. Does NOT touch g.graph/g.pending or the renderer's attached
 *  pointer -- callers that are about to publish a replacement do so
 *  immediately afterward in the same call, per the file header. */
void TeardownEmbedder() {
  g.grip.reset();
  g.forceAtlas.reset();
  g.tutte.reset();
  g.springTutte.reset();
  g.reingoldTilford.reset();
  g.kind = Kind::None;
  g.autoRun = false;
}

/** Per-kind node/edge styling, mirroring each native demo's grRendererDesc
 *  choices. Cheap (see grRendererSetNodeStyle's doc comment), so unlike
 *  those demos' one-time desc setup, this is applied fresh after every run. */
void ApplyStyle(Kind kind) {
  grNodeStyle ns{};
  grEdgeStyle es{};
  switch (kind) {
  case Kind::GRIP:
    ns.fillColor = GR_COLOR(0.55f, 0.78f, 1.0f, 1.0f);
    ns.radius = 2.5f;
    ns.sizeMode = GR_SIZE_PIXELS;
    es.color = GR_COLOR(0.45f, 0.55f, 0.75f, 0.35f);
    es.width = 1.0f;
    break;
  case Kind::ForceAtlas:
    ns.fillColor = GR_COLOR(0.55f, 0.78f, 1.0f, 1.0f);
    ns.radius = 0.5f;
    ns.sizeMode = GR_SIZE_WORLD;
    ns.minPixelRadius = 2.0f; // see forceEmbedderDemo's comment on drift
    es.color = GR_COLOR(0.45f, 0.55f, 0.75f, 0.45f);
    es.width = 1.5f;
    break;
  case Kind::Tutte:
  case Kind::SpringTutte:
    ns.fillColor = GR_COLOR(0.55f, 0.78f, 1.0f, 1.0f);
    ns.radius = 3.0f;
    ns.sizeMode = GR_SIZE_PIXELS;
    es.color = GR_COLOR(0.45f, 0.55f, 0.75f, 0.45f);
    es.width = 1.5f;
    break;
  case Kind::ReingoldTilford:
    ns.fillColor = GR_COLOR(0.55f, 0.82f, 0.65f, 1.0f);
    ns.radius = 4.0f;
    ns.sizeMode = GR_SIZE_PIXELS;
    es.color = GR_COLOR(0.45f, 0.60f, 0.55f, 0.55f);
    es.width = 1.5f;
    break;
  case Kind::None:
    return;
  }
  grRendererSetNodeStyle(g.renderer, &ns);
  grRendererSetEdgeStyle(g.renderer, &es);
}

/** Common tail of every successful webRun*(): stats/vertex-list default off
 *  per the task's wasm-defaults requirement (grRendererSetGraph doesn't
 *  reset these flags itself, but calling them again is idempotent and cheap
 *  -- see grRendererShowStats/ShowVertexList's early-return-if-unchanged
 *  bodies), then apply per-kind styling and fit the camera. */
void FinishAttach(Kind kind) {
  grRendererShowStats(g.renderer, false);
  grRendererShowVertexList(g.renderer, false);
  ApplyStyle(kind);
  grRendererFitView(g.renderer);
}

void RefreshForceAtlasRadii() {
  if (!g.forceAtlas)
    return;
  size_t n = g.forceAtlas->PositionCount();
  if (g.faRadii.size() < n)
    g.faRadii.resize(n);
  for (size_t i = 0; i < n; i++)
    g.faRadii[i] = (float)g.forceAtlas->VertexRadius(i);
  grRendererSetNodeSizes(g.renderer, g.faRadii.data(), n);
}

} // namespace

extern "C" {

// ---------------------------------------------------------------------------
// GRAPH STAGING
//
// Every graph source (generator / file upload / JS-authored code) funnels
// through webGraphStageJSON: JS parses whatever it has into flat
// {directed, vertexCount, edgeU[], edgeV[], edgeWeight[]} arrays and this
// function builds a gviz::Graph directly via AddVertex/AddEdge, mirroring
// what gviz::io's file-path-based loaders do internally but without needing
// wasm virtual-FS plumbing. See wasm/webDemoUI.js for the JSON schema.
// ---------------------------------------------------------------------------

/**
 * Builds a graph from flat arrays and stages it (see SetPending). @p
 * edgeWeight may be NULL, in which case every edge gets weight 1.0.
 * @return 0 on success, -1 on failure (negative counts, an edge endpoint >=
 * vertexCount, or an allocation failure) -- webLastError() explains why.
 */
EMSCRIPTEN_KEEPALIVE
int webGraphStageJSON(int directed, int vertexCount, const uint32_t *edgeU,
                       const uint32_t *edgeV, const double *edgeWeight,
                       int edgeCount) {
  if (vertexCount < 0 || edgeCount < 0) {
    g.lastError = "vertex/edge count must be >= 0";
    return -1;
  }
  try {
    gviz::Graph built(directed != 0, (size_t)std::max(vertexCount, 1));
    for (int i = 0; i < vertexCount; i++)
      built.AddVertex();
    for (int i = 0; i < edgeCount; i++) {
      uint32_t u = edgeU[i], v = edgeV[i];
      if (u >= (uint32_t)vertexCount || v >= (uint32_t)vertexCount) {
        g.lastError = "edge endpoint out of range";
        return -1;
      }
      built.AddEdge(u, v, edgeWeight ? edgeWeight[i] : 1.0);
    }
    SetPending(std::move(built));
    g.lastError.clear();
    return 0;
  } catch (const std::exception &e) {
    g.lastError = e.what();
    return -1;
  }
}

#define WEB_GRAPH_GEN(name, expr)                                            \
  EMSCRIPTEN_KEEPALIVE                                                       \
  int name {                                                                 \
    try {                                                                    \
      SetPending(expr);                                                     \
      g.lastError.clear();                                                   \
      return 0;                                                              \
    } catch (const std::exception &e) {                                      \
      g.lastError = e.what();                                                \
      return -1;                                                             \
    }                                                                        \
  }

WEB_GRAPH_GEN(webGraphStageSierpinski(int depth),
              gviz::graphs::CreateSierpinski(depth, nullptr))
WEB_GRAPH_GEN(webGraphStageSierpinskiTet(int depth),
              gviz::graphs::CreateSierpinskiTetrahedron(depth, nullptr))
WEB_GRAPH_GEN(webGraphStageSierpinskiCarpet(int depth),
              gviz::graphs::BuildSierpinskiCarpet((size_t)std::max(depth, 0)))
WEB_GRAPH_GEN(webGraphStageTetraMesh(int depth),
              gviz::graphs::BuildTetrahedralMesh((size_t)std::max(depth, 0)))
WEB_GRAPH_GEN(webGraphStageRectMesh(int rows, int cols),
              gviz::graphs::BuildRectMesh((size_t)std::max(rows, 0),
                                          (size_t)std::max(cols, 0)))
WEB_GRAPH_GEN(webGraphStageTriMesh(int depth),
              gviz::graphs::BuildEquilateralTriMesh((size_t)std::max(depth, 0)))
WEB_GRAPH_GEN(webGraphStageKnottedRectMesh(int rows, int cols),
              gviz::graphs::BuildKnottedRectMesh((size_t)std::max(rows, 0),
                                                 (size_t)std::max(cols, 0)))
WEB_GRAPH_GEN(webGraphStageMobius(int rows, int cols),
              gviz::graphs::BuildMobiusStrip((size_t)std::max(rows, 0),
                                             (size_t)std::max(cols, 0)))
WEB_GRAPH_GEN(webGraphStageKlein(int rows, int cols),
              gviz::graphs::BuildKleinBottle((size_t)std::max(rows, 0),
                                             (size_t)std::max(cols, 0)))
WEB_GRAPH_GEN(webGraphStageRandom(int numVertices, double density,
                                  unsigned int seed),
              gviz::graphs::BuildRandomConnectedGraph(
                  (size_t)std::max(numVertices, 0), density, seed))

#undef WEB_GRAPH_GEN

/**
 * Stages a graph loaded from @p path, a file the caller has already written
 * into Emscripten's virtual filesystem (see wasm/webDemoUI.js's upload
 * handler: FileReader -> Module.FS.writeFile -> this call -> FS.unlink).
 * This is the one place webDemo.cpp reaches for gviz::io's file-path-based
 * loaders instead of the flat-array webGraphStageJSON path -- .edges/.gexf
 * are real file formats with their own parsers already in gviz, unlike the
 * ad hoc JSON schema used for upload/JS-code otherwise.
 *
 * @p format: 0 = .edges (gviz::io::LoadFromEdgesFile), 1 = .gexf
 * (gviz::io::LoadFromGexfFile). @p directed applies to both; @p skipHeader
 * applies only to .edges (EdgesFileOptions::skipHeader).
 *
 * A .gexf load populates each vertex's data pointer with a heap-allocated
 * std::string* (see LoadFromGexfFile's doc comment); this app has no use
 * for that label data (unlike the native demos, which forward it to
 * grRendererSetVertexLabels), so it's freed immediately via
 * FreeVertexDataStrings rather than kept alive for the graph's lifetime.
 *
 * @return 0 on success, -1 on failure (bad path, malformed file, ...) --
 * webLastError() explains why.
 */
EMSCRIPTEN_KEEPALIVE
int webStageFromFile(const char *path, int format, int directed, int skipHeader) {
  try {
    gviz::Graph built = [&]() -> gviz::Graph {
      if (format == 1) {
        gviz::Graph gexf = gviz::io::LoadFromGexfFile(path, directed != 0);
        gviz::io::FreeVertexDataStrings(gexf);
        return gexf;
      }
      gviz::io::EdgesFileOptions opts;
      opts.directed = directed != 0;
      opts.skipHeader = skipHeader != 0;
      return gviz::io::LoadFromEdgesFile(path, opts);
    }();
    SetPending(std::move(built));
    g.lastError.clear();
    return 0;
  } catch (const std::exception &e) {
    g.lastError = e.what();
    return -1;
  }
}

/** Vertex/edge count of the currently staged (not yet run) graph, for UI
 *  feedback. Both -1 if nothing is staged. */
EMSCRIPTEN_KEEPALIVE int webStagedVertexCount() {
  return g.pending ? (int)g.pending->Size() : -1;
}
EMSCRIPTEN_KEEPALIVE int webStagedEdgeCount() {
  return g.pending ? (int)g.pending->EdgeCount() : -1;
}

/** The last error message from a failed webGraphStage-or-webRun call (empty
 *  string if the last call succeeded or none has been made yet). Owned by
 *  the module; valid until the next call into any exported function here. */
EMSCRIPTEN_KEEPALIVE const char *webLastError() { return g.lastError.c_str(); }

// ---------------------------------------------------------------------------
// RUN: consumes the staged graph and starts a fresh embedder over it. See
// the file header for the "build into temporaries, publish only on success"
// sequencing every one of these follows.
// ---------------------------------------------------------------------------

/** @p kPolicy: 0=Constant 1=LayerDecay 2=LayerGrow 3=PlacementDecay 4=Budget
 *  (gviz::layout::GRIP::KPolicy's declaration order). @p knnCapacity <= 0
 *  keeps GRIP's own default (256). */
EMSCRIPTEN_KEEPALIVE
int webRunGRIP(int dimension, int diameter, int knnCapacity, int statsEnabled,
               int placementKMax, int refinementKMax, int kPolicy) {
  if (!g.pending) {
    g.lastError = "no graph staged";
    return -1;
  }
  std::optional<gviz::layout::GRIP> next;
  try {
    gviz::Subgraph sg = gviz::Subgraph::CreateFull(*g.pending);
    gviz::layout::GRIP::Config config;
    if (knnCapacity > 0)
      config.knnCapacity = (size_t)knnCapacity;
    config.statsEnabled = statsEnabled != 0;
    next.emplace(std::move(sg), (size_t)std::max(diameter, 0),
                 (size_t)dimension, config);
    next->ConfigureK((size_t)std::max(placementKMax, 0),
                     (size_t)std::max(refinementKMax, 0),
                     static_cast<gviz::layout::GRIP::KPolicy>(
                         std::clamp(kPolicy, 0, 4)));
    next->Begin();
  } catch (const std::exception &e) {
    g.lastError = e.what();
    return -1;
  }
  TeardownEmbedder();
  g.graph = std::move(g.pending);
  g.grip.emplace(std::move(*next));
  g.kind = Kind::GRIP;
  // Unlike ForceAtlas/Tutte/SpringTutte (continuous physics stepping is the
  // expected default there), GRIP starts PAUSED: at 60fps, free-running
  // RefineRound() blows through a layer's rounds near-instantly with
  // nothing to actually watch, even though the whole point of GRIP's
  // layer-by-layer refinement is visible in the manual R/N buttons
  // (webGripRefineRound/webGripNextStage). The generic auto-run toggle
  // (webSetAutoRun, wired to the page's Pause/Resume button) only ever
  // free-runs RefineRound() on the CURRENT layer when enabled (see Tick's
  // Kind::GRIP case) -- it never calls NextStage() itself, at any round
  // count. Moving to the next layer is exclusively a user action, whether
  // auto-run is on or off.
  g.autoRun = false;
  if (grRendererSetGraph(g.renderer, *g.grip, g.graph.get()) < 0) {
    g.lastError = "renderer attach failed";
    return -1;
  }
  // Native-demo key parity (gripDemo.cpp): the page's buttons drive the same
  // actions, this is just an extra convenience once the canvas has focus.
  grRendererBindKey(g.renderer, 'R', "grip.refineRound");
  grRendererBindKey(g.renderer, 'N', "grip.nextStage");
  FinishAttach(Kind::GRIP);
  g.lastError.clear();
  return 0;
}

/** @p model: 0=LinLog 1=FruchtermanReingold. */
EMSCRIPTEN_KEEPALIVE
int webRunForceAtlas(int model, double gravityK, double edgeLength,
                      double theta, double radiusBase, double radiusPerDegree,
                      int preventOverlap, unsigned int seed) {
  if (!g.pending) {
    g.lastError = "no graph staged";
    return -1;
  }
  std::optional<gviz::layout::ForceAtlas> next;
  try {
    gviz::Subgraph sg = gviz::Subgraph::CreateFull(*g.pending);
    std::unique_ptr<gviz::layout::ForceModel> fm =
        model == 1 ? std::unique_ptr<gviz::layout::ForceModel>(
                         std::make_unique<gviz::layout::FruchtermanReingold>())
                   : std::unique_ptr<gviz::layout::ForceModel>(
                         std::make_unique<gviz::layout::LinLog>());
    next.emplace(std::move(sg), 2, std::move(fm));
    next->ConfigureGravity(gravityK);
    if (edgeLength > 0)
      next->Configure(edgeLength, 0);
    if (theta > 0)
      next->ConfigureBarnesHut(theta, 0);
    next->ConfigureRadius(radiusBase, radiusPerDegree);
    next->SetPreventOverlapEnabled(preventOverlap != 0);
    next->Begin(seed);
  } catch (const std::exception &e) {
    g.lastError = e.what();
    return -1;
  }
  TeardownEmbedder();
  g.graph = std::move(g.pending);
  g.forceAtlas.emplace(std::move(*next));
  g.kind = Kind::ForceAtlas;
  g.autoRun = true;
  if (grRendererSetGraph(g.renderer, *g.forceAtlas, g.graph.get()) < 0) {
    g.lastError = "renderer attach failed";
    return -1;
  }
  grRendererBindKey(g.renderer, 'R', "forceEmbedder.step");
  FinishAttach(Kind::ForceAtlas);
  RefreshForceAtlasRadii();
  g.lastError.clear();
  return 0;
}

EMSCRIPTEN_KEEPALIVE
int webRunTutte(double epsilon, int gaussSeidel) {
  if (!g.pending) {
    g.lastError = "no graph staged";
    return -1;
  }
  gviz::Graph *gp = g.pending.get();
  std::optional<gviz::layout::Tutte> next;
  try {
    gviz::Subgraph sg = gviz::Subgraph::CreateFull(*gp);
    next.emplace(*gp, std::move(sg), 2,
                 epsilon > 0 ? epsilon : gviz::layout::Tutte::kDefaultEpsilon);
    next->SetGaussSeidelEnabled(gaussSeidel != 0);
    next->Begin();
  } catch (const std::exception &e) {
    g.lastError = e.what();
    return -1;
  }
  TeardownEmbedder();
  g.graph = std::move(g.pending);
  g.tutte.emplace(std::move(*next));
  g.kind = Kind::Tutte;
  g.autoRun = true;
  if (grRendererSetGraph(g.renderer, *g.tutte, g.graph.get()) < 0) {
    g.lastError = "renderer attach failed";
    return -1;
  }
  grRendererBindKey(g.renderer, 'R', "tutte.step");
  grRendererBindKey(g.renderer, 'B', "tutte.fixOuterFace");
  grRendererBindMouse(g.renderer, GR_MOUSE_BUTTON_RIGHT, GR_ACTION_PICK_FACE);
  FinishAttach(Kind::Tutte);
  g.lastError.clear();
  return 0;
}

EMSCRIPTEN_KEEPALIVE
int webRunSpringTutte(double epsilon, double stiffness, double damping) {
  if (!g.pending) {
    g.lastError = "no graph staged";
    return -1;
  }
  gviz::Graph *gp = g.pending.get();
  std::optional<gviz::layout::SpringTutte> next;
  try {
    gviz::Subgraph sg = gviz::Subgraph::CreateFull(*gp);
    next.emplace(*gp, std::move(sg), 2,
                 epsilon > 0 ? epsilon
                             : gviz::layout::SpringTutte::kDefaultEpsilon);
    next->Configure(stiffness, damping);
    next->Begin();
  } catch (const std::exception &e) {
    g.lastError = e.what();
    return -1;
  }
  TeardownEmbedder();
  g.graph = std::move(g.pending);
  g.springTutte.emplace(std::move(*next));
  g.kind = Kind::SpringTutte;
  g.autoRun = true;
  if (grRendererSetGraph(g.renderer, *g.springTutte, g.graph.get()) < 0) {
    g.lastError = "renderer attach failed";
    return -1;
  }
  grRendererBindKey(g.renderer, 'R', "springTutte.step");
  grRendererBindKey(g.renderer, 'B', "springTutte.fixOuterFace");
  grRendererBindMouse(g.renderer, GR_MOUSE_BUTTON_RIGHT, GR_ACTION_PICK_FACE);
  FinishAttach(Kind::SpringTutte);
  g.lastError.clear();
  return 0;
}

/** ReingoldTilford is one-shot (see its class doc): this runs the whole
 *  construct -> CalculateOffsets -> Embed sequence immediately.
 *  @p root must be the tree's actual root (the vertex with no parent) --
 *  the constructor throws NotATreeError otherwise, surfaced via
 *  webLastError(). The staged graph must be directed. */
EMSCRIPTEN_KEEPALIVE
int webRunReingoldTilford(int root) {
  if (!g.pending) {
    g.lastError = "no graph staged";
    return -1;
  }
  gviz::Graph *gp = g.pending.get();
  std::optional<gviz::layout::ReingoldTilford> next;
  try {
    next.emplace(*gp, (size_t)std::max(root, 0));
    next->CalculateOffsets((size_t)std::max(root, 0), 0);
    double origin[2] = {0.0, 0.0};
    next->Embed((size_t)std::max(root, 0), origin);
  } catch (const std::exception &e) {
    g.lastError = e.what();
    return -1;
  }
  TeardownEmbedder();
  g.graph = std::move(g.pending);
  g.reingoldTilford.emplace(std::move(*next));
  g.kind = Kind::ReingoldTilford;
  g.autoRun = false; // one-shot, nothing to auto-advance
  if (grRendererSetGraph(g.renderer, *g.reingoldTilford, g.graph.get()) < 0) {
    g.lastError = "renderer attach failed";
    return -1;
  }
  FinishAttach(Kind::ReingoldTilford);
  g.lastError.clear();
  return 0;
}

// Note: no webRunPlanar() -- gviz::layout::Planar is still WIP elsewhere in
// gviz (its SchnyderWood-based Embed() has known implementation gaps, see
// SchnyderWood.hpp), so it's deliberately not exposed through this UI yet.

// ---------------------------------------------------------------------------
// LIVE STATE / RUNTIME CONTROLS
//
// Every setter below is a no-op (not an error) when called against the
// wrong kind or with no embedder running -- JS only ever shows the controls
// matching webGetKind(), but a stray call from a stale UI panel (e.g. a
// slider event still in flight from just before Run switched kinds) should
// never crash or misapply to the new embedder.
// ---------------------------------------------------------------------------

/** -1 = nothing running, else a Kind value (see the enum above; kept in
 *  sync with wasm/webDemoUI.js's own copy of this numbering). */
EMSCRIPTEN_KEEPALIVE int webGetKind() {
  return g.kind == Kind::None ? -1 : (int)g.kind;
}

/** Whether the running embedder (if any) has a Step-like relaxation loop at
 *  all -- false for ReingoldTilford, which is one-shot. */
EMSCRIPTEN_KEEPALIVE int webIsIterative() {
  return g.kind == Kind::GRIP || g.kind == Kind::ForceAtlas ||
         g.kind == Kind::Tutte || g.kind == Kind::SpringTutte;
}

EMSCRIPTEN_KEEPALIVE void webSetAutoRun(int enabled) {
  if (webIsIterative())
    g.autoRun = enabled != 0;
}
EMSCRIPTEN_KEEPALIVE int webGetAutoRun() { return g.autoRun ? 1 : 0; }

EMSCRIPTEN_KEEPALIVE void webFitView() {
  if (g.renderer)
    grRendererFitView(g.renderer);
}

/**
 * Saves the current frame to @p path (a virtual-FS path the caller reads
 * back afterward, e.g. via Module.FS.readFile -- see wasm/webDemoUI.js).
 * @return 0 on success, -1 on failure or no renderer yet.
 */
EMSCRIPTEN_KEEPALIVE int webSaveScreenshot(const char *path) {
  if (!g.renderer)
    return -1;
  return grRendererSaveScreenshot(g.renderer, path);
}

// RENDERING (renderer-level, independent of which/whether an embedder is
// attached -- the renderer exists before any graph is ever staged, so these
// are always callable and always show in the page's "Rendering" section,
// not gated behind a running embedder the way the per-kind live controls
// are). Wrap grRendererSetEdgeWeightWidth/SetEdgeDegreeAlpha, both off by
// default to match grRendererDescInit's own default (see grender.h).

EMSCRIPTEN_KEEPALIVE void webSetEdgeWeightWidth(int enabled) {
  if (g.renderer)
    grRendererSetEdgeWeightWidth(g.renderer, enabled != 0);
}
EMSCRIPTEN_KEEPALIVE int webGetEdgeWeightWidth() {
  return g.renderer ? (grRendererEdgeWeightWidth(g.renderer) ? 1 : 0) : 0;
}
EMSCRIPTEN_KEEPALIVE void webSetEdgeDegreeAlpha(int enabled) {
  if (g.renderer)
    grRendererSetEdgeDegreeAlpha(g.renderer, enabled != 0);
}
EMSCRIPTEN_KEEPALIVE int webGetEdgeDegreeAlpha() {
  return g.renderer ? (grRendererEdgeDegreeAlpha(g.renderer) ? 1 : 0) : 0;
}

EMSCRIPTEN_KEEPALIVE void webGripRefineRound() {
  if (g.kind == Kind::GRIP && g.grip)
    g.grip->RefineRound();
}
EMSCRIPTEN_KEEPALIVE void webGripNextStage() {
  if (g.kind == Kind::GRIP && g.grip)
    g.grip->NextStage();
}
EMSCRIPTEN_KEEPALIVE void webGripConfigureK(int placementKMax,
                                            int refinementKMax, int kPolicy) {
  if (g.kind == Kind::GRIP && g.grip)
    g.grip->ConfigureK(
        (size_t)std::max(placementKMax, 0), (size_t)std::max(refinementKMax, 0),
        static_cast<gviz::layout::GRIP::KPolicy>(std::clamp(kPolicy, 0, 4)));
}
EMSCRIPTEN_KEEPALIVE int webGripLayerCount() {
  return (g.kind == Kind::GRIP && g.grip) ? (int)g.grip->LayerCount() : -1;
}
EMSCRIPTEN_KEEPALIVE int webGripCurrentLayer() {
  return (g.kind == Kind::GRIP && g.grip) ? (int)g.grip->CurrentLayer() : -1;
}
EMSCRIPTEN_KEEPALIVE int webGripCurrentRound() {
  return (g.kind == Kind::GRIP && g.grip) ? (int)g.grip->CurrentRound() : -1;
}

EMSCRIPTEN_KEEPALIVE void webForceAtlasStep() {
  if (g.kind == Kind::ForceAtlas && g.forceAtlas)
    g.forceAtlas->Step();
}
EMSCRIPTEN_KEEPALIVE void webForceAtlasConfigure(double edgeLength,
                                                 double gravityK, double theta,
                                                 double radiusBase,
                                                 double radiusPerDegree,
                                                 int preventOverlap) {
  if (!(g.kind == Kind::ForceAtlas && g.forceAtlas))
    return;
  auto &fa = *g.forceAtlas;
  if (edgeLength > 0)
    fa.Configure(edgeLength, 0);
  fa.ConfigureGravity(gravityK);
  if (theta > 0)
    fa.ConfigureBarnesHut(theta, 0);
  fa.ConfigureRadius(radiusBase, radiusPerDegree);
  fa.SetPreventOverlapEnabled(preventOverlap != 0);
  RefreshForceAtlasRadii();
}

EMSCRIPTEN_KEEPALIVE void webTutteStep(double dt) {
  if (g.kind == Kind::Tutte && g.tutte)
    g.tutte->Step(dt);
}
EMSCRIPTEN_KEEPALIVE void webTutteSetGaussSeidel(int enabled) {
  if (g.kind == Kind::Tutte && g.tutte)
    g.tutte->SetGaussSeidelEnabled(enabled != 0);
}
EMSCRIPTEN_KEEPALIVE void webTutteFixOuterFace() {
  if (g.kind == Kind::Tutte && g.tutte)
    g.tutte->FixOuterFace();
}
EMSCRIPTEN_KEEPALIVE int webTutteConverged() {
  return (g.kind == Kind::Tutte && g.tutte) ? (g.tutte->Converged() ? 1 : 0)
                                            : -1;
}

EMSCRIPTEN_KEEPALIVE void webSpringTutteStep(double dt) {
  if (g.kind == Kind::SpringTutte && g.springTutte)
    g.springTutte->Step(dt);
}
EMSCRIPTEN_KEEPALIVE void webSpringTutteConfigure(double stiffness,
                                                  double damping) {
  if (g.kind == Kind::SpringTutte && g.springTutte)
    g.springTutte->Configure(stiffness, damping);
}
EMSCRIPTEN_KEEPALIVE void webSpringTutteFixOuterFace() {
  if (g.kind == Kind::SpringTutte && g.springTutte)
    g.springTutte->FixOuterFace();
}
EMSCRIPTEN_KEEPALIVE int webSpringTutteConverged() {
  return (g.kind == Kind::SpringTutte && g.springTutte)
             ? (g.springTutte->Converged() ? 1 : 0)
             : -1;
}

} // extern "C"

// ---------------------------------------------------------------------------
// Main loop -- drives the renderer every frame and, while auto-run is on,
// advances whichever embedder is live by the same per-frame amount of work
// its native demo counterpart uses (gripDemo's 60-rounds-per-layer,
// forceEmbedderDemo's 10 steps/frame, tutteDemo/its SpringTutte sibling's 20
// steps/frame while not yet converged).
// ---------------------------------------------------------------------------

namespace {

bool Tick() {
  if (!grRendererFrame(g.renderer))
    return false;

  if (g.autoRun) {
    switch (g.kind) {
    case Kind::GRIP:
      // Auto-run only ever refines rounds on the CURRENT layer, forever --
      // it never advances to the next layer by itself, no matter how many
      // rounds have run. NextStage() is exclusively a user action (the
      // "Next stage" button/N key/webGripNextStage), same as RefineRound()
      // is when auto-run is off. An earlier version of this switched layers
      // automatically past a round-count threshold; that surprised users
      // who expected Resume to mean "keep refining" and nothing more.
      if (g.grip)
        g.grip->RefineRound();
      break;
    case Kind::ForceAtlas:
      if (g.forceAtlas)
        for (int i = 0; i < 10; i++)
          g.forceAtlas->Step();
      break;
    case Kind::Tutte:
      if (g.tutte && !g.tutte->Converged()) {
        double dt = grRendererDeltaTime(g.renderer);
        for (int i = 0; i < 20; i++)
          g.tutte->Step(dt);
      }
      break;
    case Kind::SpringTutte:
      if (g.springTutte && !g.springTutte->Converged()) {
        double dt = grRendererDeltaTime(g.renderer);
        for (int i = 0; i < 20; i++)
          g.springTutte->Step(dt);
      }
      break;
    case Kind::ReingoldTilford:
    case Kind::None:
      break;
    }
  }
  return true;
}

#ifdef __EMSCRIPTEN__
void EmTick() {
  if (!Tick()) {
    grRendererDestroy(g.renderer);
    emscripten_cancel_main_loop();
  }
}
#endif

} // namespace

int main() {
  grRendererDesc desc;
  grRendererDescInit(&desc);
  desc.title = "grender interactive";

  grRenderer *r = grRendererCreate(&desc);
  if (!r) {
    fprintf(stderr, "renderer creation failed\n");
    return 1;
  }
  g.renderer = r;

  // Task requirement: stats/vertex-search overlays default off on the web
  // build (the underlying capability stays available -- see grender.h --
  // just not surfaced by this app's UI in this pass).
  grRendererShowStats(r, false);
  grRendererShowVertexList(r, false);

#ifdef __EMSCRIPTEN__
  // No fixed content to show yet -- the loop starts immediately with no
  // graph attached (grRendererFrame tolerates r->graph == NULL fine) and
  // waits for the page's UI to call a webGraphStage*/webRun* pair.
  emscripten_set_main_loop(EmTick, 0, true);
#else
  // Native build: same renderer/window, but nothing ever calls the
  // webGraphStage*/webRun* entry points (there is no JS host here), so this
  // just idles with an empty window -- useful for confirming the app still
  // links and starts up natively, not a general-purpose native front-end.
  while (Tick()) {
  }
  grRendererDestroy(r);
#endif

  return 0;
}
