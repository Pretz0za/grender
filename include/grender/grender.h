#ifndef _GRENDER_H_
#define _GRENDER_H_

/**
 * grender - a GPU renderer for gviz embedded graphs.
 *
 * grender sits strictly on the consumer side of the gviz abstraction barrier:
 * it only reads embedded graphs through the public gviz::layout::EmbeddedGraph
 * / gviz::Subgraph API and is agnostic to which embedding algorithm produced
 * the positions. It supports 2D and 3D embeddings directly, and 4D embeddings
 * projected to 3D with PCA for display. It renders millions of vertices
 * and edges in two instanced draw calls, and re-reads vertex positions every
 * frame so a live embedder (force-directed, GRIP rounds, ...) is rendered
 * online.
 *
 * Interaction is split in two layers:
 *  - Built-in navigation owned by the renderer: mouse pan/zoom (2D) or
 *    orbit/pan/dolly (3D), the Q/E keys rotating the 2D camera in fixed
 *    steps (no-op on 3D embeddings), and grRendererFitView /
 *    grRendererFocusVertex framing.
 *  - Named actions owned by the embedded graph creator (see
 *    gviz::layout::EmbeddedGraph::AddAction). The application binds input
 *    keys to action names with grRendererBindKey; the renderer dispatches
 *    payloads without knowing anything about the embedder.
 */

// gviz::layout::EmbeddedGraph.hpp no longer transitively pulls in
// Graph.hpp/Subgraph.hpp the way the pre-refactor version did (it used to
// hold a gviz::Subgraph directly; the current one is GraphLike-agnostic --
// see EmbeddedGraph.hpp's class doc). This header uses gviz::Graph,
// gviz::Subgraph, and gviz::GraphLike directly (grRendererSetGraph<G>,
// grRendererSetHighlight, ...), so they're included explicitly here rather
// than relied on transitively -- this header must stay self-contained
// regardless of #include order relative to gviz.hpp at any call site.
#include "EmbeddedGraph.hpp"
#include "Graph.hpp"
#include "GraphLike.hpp"
#include "Subgraph.hpp"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grRenderer grRenderer;

typedef struct grColor {
  float r, g, b, a;
} grColor;

#define GR_COLOR(R, G, B, A) ((grColor){(R), (G), (B), (A)})

/** Packs 8-bit RGBA channels into the uint32 format used by the per-element
 *  color arrays (grRendererSetNodeColors / grRendererSetEdgeColors). */
#define GR_RGBA8(R, G, B, A)                                                   \
  ((uint32_t)(R) | ((uint32_t)(G) << 8) | ((uint32_t)(B) << 16) |              \
   ((uint32_t)(A) << 24))

/** How node radius / edge width are interpreted. */
typedef enum grSizeMode {
  GR_SIZE_PIXELS = 0, /**< Constant screen-space size regardless of zoom. */
  GR_SIZE_WORLD = 1,  /**< Sized in embedding coordinates, scales with zoom. */
} grSizeMode;

typedef struct grNodeStyle {
  grColor fillColor;   /**< Global fill; overridden by per-node colors. */
  grColor strokeColor; /**< Outline color, used when strokeWidth > 0. */
  float radius;        /**< Node radius, interpreted per sizeMode. */
  float strokeWidth;   /**< Outline width in the same unit as radius. */
  grSizeMode sizeMode;
  /**
   * Floor/ceiling on the on-screen node radius in pixels, applied only when
   * sizeMode is GR_SIZE_WORLD and only at draw time: the world-space radius
   * itself (global or per-node, see grRendererSetNodeSizes) is never
   * altered, so anything else reading it -- e.g. physics using it as a
   * collision radius -- is unaffected. Use these to keep nodes visible when
   * zoomed far out or reasonably sized when zoomed far in, without giving up
   * world-space sizing the rest of the time. 0 disables each independently
   * (the default for both).
   */
  float minPixelRadius;
  float maxPixelRadius;
  /**
   * When true, nodes are drawn as rounded squares instead of circles -- a
   * fragment-shader shape swap only, computed from the same radius/
   * strokeWidth/sizeMode already used for circles (including per-node
   * overrides via grRendererSetNodeSizes), so every other node-styling
   * feature keeps working unchanged. Off by default (circle), for every
   * existing consumer's visuals to stay pixel-identical.
   */
  bool roundedSquare;
  /**
   * Corner radius as a fraction of the node's drawn radius, in [0, 0.5]
   * (0 = sharp square corners, 0.5 = the square's corners are as round as
   * its half-size allows). Only read when roundedSquare is true. Clamped
   * at draw time; out-of-range values are not an error.
   */
  float cornerRadiusFraction;
} grNodeStyle;

typedef struct grEdgeStyle {
  grColor color; /**< Global color; overridden by per-edge colors. */
  float width;   /**< Edge width, interpreted per sizeMode. */
  grSizeMode sizeMode;
} grEdgeStyle;

typedef struct grRendererDesc {
  const char *title;  /**< Window title. NULL for a default. */
  uint32_t width;     /**< Initial window width in screen points. */
  uint32_t height;    /**< Initial window height in screen points. */
  grColor clearColor; /**< Background color. */
  grNodeStyle nodeStyle;
  grEdgeStyle edgeStyle;
  bool vsync; /**< true: FIFO present (default), false: immediate. */
  /**
   * When true, the edge shader scales each edge's alpha by the higher
   * degree of its endpoints so hub edges fade. The renderer derives and
   * re-uploads the per-vertex degrees itself on every structural change
   * (from grender's own adjacency snapshot, rebuilt on
   * grRendererGraphStructureChanged -- see grTopology in grInternal.h),
   * so this stays correct as the attached structure's visible/hidden set
   * changes -- no grRendererSetNodeDegrees calls needed (that
   * API remains for custom values, but the automatic refresh overwrites
   * them on the next structural change). Per-edge colors (including
   * highlights) are unaffected and keep their uploaded alpha. Off by
   * default; can also be toggled with grRendererSetEdgeDegreeAlpha.
   */
  bool edgeDegreeAlpha;
  /**
   * When true, the edge shader scales each edge's drawn width by its
   * gviz::Graph edge weight relative to the mean weight, so e.g. an edge
   * weighing twice the mean is drawn twice as thick -- edgeStyle.width is
   * the base thickness an average-weight edge is drawn at. The renderer
   * derives and re-uploads the weights itself on every structural change,
   * in the edge buffer's exact order, so this stays correct as a dynamic
   * graph grows -- no grRendererSetEdgeWeights calls needed (same
   * custom-value caveat as edgeDegreeAlpha). Off by default; can also be
   * toggled with grRendererSetEdgeWeightWidth.
   */
  bool edgeWeightWidth;
} grRendererDesc;

/** Fills @p desc with sensible defaults (dark background, white nodes). */
void grRendererDescInit(grRendererDesc *desc);

/**
 * Creates a window, a GPU device, and the render pipelines.
 * Returns NULL on failure (no GPU adapter, window creation failed, ...).
 */
grRenderer *grRendererCreate(const grRendererDesc *desc);

/** Destroys the renderer, its window, and all GPU resources. Does not touch
 *  the attached embedded graph. */
void grRendererDestroy(grRenderer *r);

#ifdef __cplusplus
} // extern "C" -- grRendererSetGraph below is a template (see its doc
  // comment for why), which cannot have C linkage; reopened below after it
  // and its two translation helpers. This file has never been C-callable
  // (see grRendererSetHighlight etc. below, already C++-only via gviz
  // reference/class-type parameters) -- the extern "C" here is purely for
  // linkage-name stability of the ordinary (non-template) functions.
#endif

/**
 * Attaches the embedded graph to render, together with the gviz::GraphLike
 * @p structure its embedder was built over. The renderer re-reads positions
 * through EmbeddedGraph::Positions every frame, so position changes made by
 * the caller (or by actions) between frames are always visible. 2D and 3D
 * embeddings are rendered directly; 4D embeddings are PCA-projected to 3D
 * each frame.
 *
 * WHY @p structure IS SEPARATE FROM @p embedding: gviz::layout::EmbeddedGraph
 * -- the common base every embedder (ForceAtlas<G>, GRIP<G>, Tutte<G>,
 * SpringTutte<G>, KamadaKawai<G>, Planar, ReingoldTilford, SchnyderWood)
 * publicly inherits, and the only type this function used to need -- carries
 * no structural information of its own: no Structure(), no adjacency, no
 * highlight (see EmbeddedGraph.hpp), only a local-index-addressed position
 * buffer, actions, stats, and a draw mask. Structure now lives solely on the
 * concrete, compile-time-typed embedder (`G structure_`), so a renderer that
 * wants to draw edges -- and this one draws exactly two instanced calls, one
 * of them for edges -- has to be told @p structure explicitly. @p G is
 * deduced from your argument and must be gviz::Graph or gviz::Subgraph (the
 * only two types gviz ships that satisfy gviz::GraphLike); pass:
 *   - `embedder.Structure()` for any templated embedder (ForceAtlas<G>,
 *     GRIP<G>, Tutte<G>, SpringTutte<G>, KamadaKawai<G>) -- the exact G it
 *     owns, by reference;
 *   - your own retained `Graph&` for Planar/ReingoldTilford/SchnyderWood --
 *     they hold their graph privately and never expose it back out, so you
 *     must keep the reference you originally constructed them with (their
 *     constructors all take `Graph&`, never move it in, precisely so you
 *     can).
 * Passing a structure other than the one @p embedding's own embedder was
 * built over produces a mismatched (garbage, not memory-unsafe) render, not
 * a crash: both are walked independently, once, at attach time.
 *
 * INDEXING CONVENTION: every by-index part of this API that used to be
 * documented as "indexed by parent-graph vertex id" (grRendererSetNodeColors/
 * SetNodeSizes/SetVertexLabels/SetNodeDegrees, picking results,
 * grRendererSetAccentVertex/FocusVertex, the console's "find <id>",
 * grRendererGetEdges) is now indexed by LOCAL index instead --
 * [0, grRendererVertexCount()) -- matching
 * gviz::layout::EmbeddedGraph::Positions()'s own addressing exactly, since
 * that's what gets uploaded to the GPU every frame. This is also a strictly
 * better fit than the old convention for a Subgraph-backed embedding: no
 * more sizing every per-vertex GPU array to the parent graph's whole id
 * range just to address a handful of sparsely-scattered view vertices,
 * mirroring the same sizing fix gviz's own DenseIndex made upstream. Use
 * grRendererRawToLocal / grRendererLocalToRaw to translate to/from
 * @p structure's own native vertex handles, e.g. to correlate with an
 * application's own id-indexed data; for a Graph-backed embedding (the
 * common whole-graph case) the two spaces coincide
 * (gviz::Graph::kDenseVertexHandles), so translation is a no-op.
 *
 * @p backingGraph is the parent gviz::Graph to use for the raw-graph-only
 * features (EnsureLayout, GetEdgeWeight, planar face picking) that
 * gviz::Subgraph deliberately never exposes on its own -- see
 * grRendererSetHighlight, grRendererSetHighlightCycle, and
 * GR_ACTION_PICK_FACE/GR_ACTION_PICK_VERTEX's doc comments. Those features
 * simply no-op (return failure / find nothing) when it's NULL. Only
 * meaningful when @p G is gviz::Subgraph: when @p G is gviz::Graph, @p
 * structure already IS that graph, so a null @p backingGraph is
 * automatically treated as @p structure -- you never need to pass it
 * explicitly for a whole-graph embedding.
 *
 * @return 0 on success, -1 on unsupported dimension or GPU allocation failure.
 */
template <gviz::GraphLike G>
int grRendererSetGraph(grRenderer *r, G &structure,
                       gviz::layout::EmbeddedGraph &embedding,
                       gviz::Graph *backingGraph = nullptr);

/** Number of vertices in the attached embedding, == its Positions() count.
 *  Every by-LOCAL-index part of this API (see grRendererSetGraph's doc
 *  comment) is indexed in [0, this). 0 when no graph is attached. */
size_t grRendererVertexCount(const grRenderer *r);

/** Translates the attached structure's native vertex handle @p raw (as
 *  passed to grRendererSetGraph) to the local index this API uses
 *  everywhere else. Unchecked: @p raw must be a handle the attached
 *  structure actually has. 0 (no crash) if no graph is attached. */
size_t grRendererRawToLocal(const grRenderer *r, size_t raw);

/** Translates a local index (as used throughout this API) back to the
 *  attached structure's native vertex handle. Unchecked: @p local must be
 *  < grRendererVertexCount(). 0 if no graph is attached. */
size_t grRendererLocalToRaw(const grRenderer *r, size_t local);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Notifies the renderer that the *structure* of the attached graph changed
 * (vertices/edges added, removed, hidden or shown). Topology buffers are
 * rebuilt lazily before the next frame. Pure position changes and draw-mask
 * updates (via EmbeddedGraph::SetDrawMaskEdgePolicy and friends) never
 * require this call.
 */
void grRendererGraphStructureChanged(grRenderer *r);

// STYLING: --------------------------------------------------------------------

/** Replaces the global node/edge styles. Cheap; takes effect next frame. */
void grRendererSetNodeStyle(grRenderer *r, const grNodeStyle *style);
void grRendererSetEdgeStyle(grRenderer *r, const grEdgeStyle *style);

/**
 * Enables or disables shader-side degree-based edge opacity. When enabled
 * (and degrees are present via grRendererSetNodeDegrees), each edge's alpha
 * is scaled from the geometric mean of its endpoint degrees (log-normalized
 * by the graph max degree). Highlighted edges (per-edge colors that differ
 * from the global edge style) bypass the fade and are drawn at alpha 1.0;
 * ordinary edges keep fading even while a highlight is active. Off by default.
 */
void grRendererSetEdgeDegreeAlpha(grRenderer *r, bool enabled);

/** Returns whether degree-based edge opacity is currently enabled. */
bool grRendererEdgeDegreeAlpha(const grRenderer *r);

/**
 * Enables or disables shader-side weight-based edge thickness. When enabled
 * (and weights are present via grRendererSetEdgeWeights), each edge's drawn
 * width is edgeStyle.width scaled by that edge's weight divided by the mean
 * of all uploaded weights -- so relative thickness directly reflects
 * relative weight (an edge weighing twice the mean is drawn twice as thick),
 * with edgeStyle.width acting as the configurable base thickness for an
 * average-weight edge. Off by default.
 */
void grRendererSetEdgeWeightWidth(grRenderer *r, bool enabled);

/** Returns whether weight-based edge thickness is currently enabled. */
bool grRendererEdgeWeightWidth(const grRenderer *r);

/**
 * Uploads per-node fill colors (GR_RGBA8 packed), indexed by local vertex
 * index (see grRendererSetGraph's INDEXING CONVENTION); @p count must equal
 * EmbeddedGraph::PositionCount(). Pass NULL
 * to revert to the global style. The data is copied; the caller keeps
 * ownership.
 *
 * This is a persistent base layer: it survives until replaced by another
 * call (or cleared by passing NULL) and is NOT affected by
 * grRendererSetHighlight / grRendererClearHighlight or
 * grRendererSetAccentVertex. A highlight, while active, is painted over
 * these colors; an accent (if set) is painted over both. Clearing the
 * highlight/accent simply removes that overlay and these colors reappear
 * unchanged.
 *
 * @return 0 on success, -1 on failure.
 */
int grRendererSetNodeColors(grRenderer *r, const uint32_t *rgba8, size_t count);
int grRendererParseVertexColors(grRenderer *r);

/** Per-node radii, same indexing rules as grRendererSetNodeColors. */
int grRendererSetNodeSizes(grRenderer *r, const float *radii, size_t count);

/**
 * Per-vertex subgraph degrees (uint32), indexed by local vertex index with
 * the same count rules as grRendererSetNodeColors. Used by the edge
 * shader when edge-degree-alpha is enabled, and by the node shader when
 * degree-based node sizing (grRendererSetNodeDegreeScale) is enabled. Pass
 * NULL to clear. The caller keeps ownership; values are copied to the GPU.
 *
 * @return 0 on success, -1 on failure.
 */
int grRendererSetNodeDegrees(grRenderer *r, const uint32_t *degrees,
                             size_t count);

/**
 * Enables or disables shader-side degree-based node sizing. When enabled
 * (and degrees are present via grRendererSetNodeDegrees), each node's world
 * radius is computed as nodeStyle.radius * (1 + @p factor * sqrt(degree))
 * instead of the plain global default -- same formula shape as
 * gviz::layout::ForceAtlas::VertexRadius, so an application driving both
 * ForceAtlas's overlap-prevention physics and this rendering feature from
 * the same (base, perDegree) pair gets a simulated layout and a drawing
 * that agree on what "radius" means, without grender depending on
 * ForceAtlas or gviz depending on grender -- the caller is the only layer
 * that legitimately knows about both, so it's the one responsible for
 * passing matching numbers to both APIs.
 *
 * A per-vertex override from grRendererSetNodeSizes, where set for a given
 * vertex, replaces this computed radius entirely -- same highest-precedence
 * behavior it already has over the plain global default. Off by default
 * (equivalent to factor 0, i.e. every node at nodeStyle.radius).
 */
void grRendererSetNodeDegreeScale(grRenderer *r, bool enabled, float factor);

/** Returns whether degree-based node sizing is currently enabled. */
bool grRendererNodeDegreeScaleEnabled(const grRenderer *r);

/** Returns the degree-scale factor last set via grRendererSetNodeDegreeScale
 *  (0 if never set). */
float grRendererNodeDegreeScaleFactor(const grRenderer *r);

/**
 * Optional per-vertex string labels, indexed by local vertex index, same
 * count rules as grRendererSetNodeColors. When set, clicking a vertex (the
 * default GR_ACTION_PICK_VERTEX binding) shows that vertex's label in the
 * vertex-info overlay panel (see VERTEX INFO OVERLAY below); a vertex with
 * a NULL label, or a click that hits nothing, clears the panel (draws
 * nothing -- there is no empty chrome). The panel can also be suppressed
 * unconditionally with grRendererShowVertexInfo(false), even while labels
 * are set and a vertex is picked. The renderer does not take ownership or
 * copy the strings -- entries must stay valid for as long as they might be
 * displayed, i.e. until this is called again or the renderer is destroyed.
 * Pass NULL to clear.
 *
 * @return 0 on success, -1 on failure.
 */
int grRendererSetVertexLabels(grRenderer *r, const char *const *labels,
                              size_t count);

/**
 * Per-edge colors, indexed in edge-buffer order: the order edges are produced
 * by iterating subgraph vertices in increasing id and their neighbor
 * iterators (undirected edges appear once, with u < v). Use
 * grRendererEdgeCount / grRendererGetEdges to inspect that order.
 *
 * Like grRendererSetNodeColors, this is a persistent base layer that a
 * highlight paints over rather than replaces (see grRendererSetHighlight);
 * clearing the highlight restores it. Because the indexing is
 * edge-buffer-order rather than a stable id, any structural change to the
 * graph invalidates it -- the renderer drops it automatically (as if NULL
 * had been passed) whenever the edge order changes, and it must be
 * re-uploaded afterward.
 */
int grRendererSetEdgeColors(grRenderer *r, const uint32_t *rgba8, size_t count);

/**
 * Per-edge weights, indexed the same way as grRendererSetEdgeColors (edge-
 * buffer order; see grRendererGetEdges). Used by the edge shader when
 * edge-weight-width is enabled; values should be positive. Pass NULL to
 * clear.
 *
 * @return 0 on success, -1 on failure.
 */
int grRendererSetEdgeWeights(grRenderer *r, const float *weights, size_t count);

/**
 * Marks individual edges as dashed, indexed the same way as
 * grRendererSetEdgeColors (edge-buffer order; see grRendererGetEdges): a
 * nonzero entry draws that edge with a fixed dash pattern instead of a solid
 * line, for visually marking connectors that aren't "real" structural edges
 * (e.g. a teaching visualization's contour threads) as distinct from
 * ordinary graph edges. Not a general per-edge line-style knob -- the dash
 * period/duty cycle are fixed, not configurable. Pass NULL to clear (every
 * edge solid). Like grRendererSetEdgeWeights, this is dropped automatically
 * whenever the edge order changes and must be re-uploaded afterward.
 *
 * @return 0 on success, -1 on failure.
 */
int grRendererSetEdgeDashed(grRenderer *r, const uint32_t *dashed,
                            size_t count);

/** Number of edges in the current topology buffer. */
size_t grRendererEdgeCount(const grRenderer *r);

/**
 * Copies the current edge list ((u,v) pairs, edge-buffer order) into @p out,
 * which must hold 2 * grRendererEdgeCount() uint32 values. Returns the number
 * of edges copied.
 */
size_t grRendererGetEdges(const grRenderer *r, uint32_t *out);

// INPUT AND ACTIONS: ----------------------------------------------------------

/** Key codes accepted by grRendererBindKey. Printable ASCII (uppercase
 *  letters, digits, punctuation) can be passed directly, e.g. 'R'. */
enum {
  GR_KEY_SPACE = 32,
  GR_KEY_ESCAPE = 256,
  GR_KEY_ENTER = 257,
  GR_KEY_TAB = 258,
  GR_KEY_BACKSPACE = 259,
  GR_KEY_RIGHT = 262,
  GR_KEY_LEFT = 263,
  GR_KEY_DOWN = 264,
  GR_KEY_UP = 265,
  GR_KEY_F1 = 290, /* F2..F12 follow consecutively */
};

/** Modifier bits reported in gviz::layout::ActionPayload.iarg (bitwise OR). */
enum {
  GR_MOD_SHIFT = 1,
  GR_MOD_CTRL = 2,
  GR_MOD_ALT = 4,
  GR_MOD_SUPER = 8,
};

/** Mouse buttons accepted by grRendererBindMouse (GLFW button ids). */
enum {
  GR_MOUSE_BUTTON_LEFT = 0,
  GR_MOUSE_BUTTON_RIGHT = 1,
  GR_MOUSE_BUTTON_MIDDLE = 2,
};

/**
 * Action name registered by grRendererSetGraph for 2D face picking and
 * highlighting. The payload worldX/worldY are filled from the click position.
 * Requires a planar combinatorial embedding (gviz::layout::ApplyPlanarRotation)
 * and a non-NULL backingGraph (see grRendererSetGraph) -- a no-op otherwise.
 */
#define GR_ACTION_PICK_FACE "grender.pickFace"

/**
 * Action name registered by grRendererSetGraph for vertex picking. Selects
 * the vertex nearest the click (in the worldX/worldY plane) and, only when
 * that click actually lands within the vertex's current on-screen radius
 * (its drawn size, converted from pixels to world units at the vertex's
 * depth -- i.e. the same tolerance a click needs to land inside the visible
 * circle), highlights it together with its neighbors and incident edges via
 * grRendererSetHighlight. On a directed graph, a plain click highlights only
 * the picked vertex's out-neighbors and out-edges; holding Shift flips this
 * to in-neighbors and in-edges instead (Shift is a no-op on undirected
 * graphs, where every edge already highlights both ways). A click that lands
 * on no vertex clears any existing highlight (grRendererClearHighlight), the
 * same as a face-pick miss. Replacing a highlight discards the previous one
 * automatically -- no need to clear first. Only fires on an actual click
 * (press and release without dragging); a click-and-drag pans/orbits instead
 * and never reaches this action, the same drag-vs-click distinction
 * grRendererBindMouse already documents. Bound to the left mouse button by
 * default; rebind with grRendererBindMouse or replace with a no-op action
 * name to disable.
 */
#define GR_ACTION_PICK_VERTEX "grender.pickVertex"

/**
 * Binds @p key so that pressing it invokes the action named @p actionName on
 * the attached embedded graph (see gviz::layout::EmbeddedGraph::AddAction).
 * The payload is filled by the renderer:
 *   worldX/worldY - cursor position unprojected into embedding coordinates
 *                   (in 3D, on the plane through the camera target),
 *   deltaTime     - seconds since the previous frame,
 *   iarg          - modifier bits (GR_MOD_*),
 *   darg          - 0.
 * Unknown action names are ignored at dispatch time, so bindings may be set
 * before the creator registers its actions. Rebinding a key replaces the
 * previous binding. @p actionName is not copied and must outlive the binding.
 *
 * @return 0 on success, -1 on allocation failure.
 */
int grRendererBindKey(grRenderer *r, int key, const char *actionName);

/** Removes the binding for @p key, if any. */
void grRendererUnbindKey(grRenderer *r, int key);

/**
 * Binds @p button so that a click (press and release without a drag) invokes
 * the action named @p actionName on the attached embedded graph. The payload
 * is filled like grRendererBindKey. Dragging with the same button still pans
 * or orbits as usual.
 *
 * @return 0 on success, -1 on allocation failure.
 */
int grRendererBindMouse(grRenderer *r, int button, const char *actionName);

/** Removes the binding for @p button, if any. */
void grRendererUnbindMouse(grRenderer *r, int button);

/**
 * Binds @p key so that holding it down continuously orbits the camera
 * around its current target at @p radPerSec radians/second of yaw, for as
 * long as the key stays down -- unlike grRendererBindKey (which fires a
 * graph action once per press), this drives grCameraOrbit directly every
 * frame, the same continuous-poll style already used for the mouse-drag
 * orbit and the Shift-pan modifier. Positive @p radPerSec matches
 * grCameraOrbit's dYaw sign (increasing yaw). Only one key can be bound at
 * a time; a later call replaces the previous binding. Pass @p key < 0 to
 * disable. No-op while the console is open or the vertex-list search box
 * has focus, same as the other single-letter shortcuts (F/Q/E/S/I/L/C).
 * 3D (perspective) cameras only -- grCameraOrbit itself no-ops on 2D.
 */
void grRendererSetOrbitKey(grRenderer *r, int key, double radPerSec);

/**
 * Action name invoked whenever a click (press and release without a drag,
 * on any mouse button) lands on a vertex's drawn circle -- the same hit test
 * GR_ACTION_PICK_VERTEX uses, so a click only counts as landing "on" a
 * vertex when it falls within the circle the user actually sees. Unlike
 * GR_ACTION_PICK_VERTEX (the renderer's own default for an *unbound* left
 * button), this fires unconditionally whenever a handler is registered for
 * it, independent of and in addition to whatever action is separately bound
 * to that button with grRendererBindMouse.
 *
 * There is nothing to bind: register a handler the same way as any other
 * action (gviz::layout::EmbeddedGraph::AddAction) and it starts firing on the
 * next hit. The payload is filled like grRendererBindKey, except @p iarg
 * carries the clicked vertex's local index (not modifier bits --
 * gviz::layout::ActionPayload's fields are contextual per trigger, see its
 * doc comment):
 *   worldX/worldY - cursor position unprojected into embedding coordinates,
 *   deltaTime     - seconds since the previous frame,
 *   iarg          - the clicked vertex's local index,
 *   darg          - 0.
 * A click that misses every vertex does not invoke it.
 */
#define GR_ACTION_VERTEX_CLICKED "grender.vertexClicked"

// HIGHLIGHTS: ---------------------------------------------------------------

/**
 * Stores @p highlight (a gviz::Subgraph on the same parent graph as the
 * attached embedded graph) and colors its vertices/edges differently from the
 * global node/edge styles every frame. Pass 0 for @p nodeRgba or @p edgeRgba
 * to keep the global style for that element class. The subgraph is copied
 * (gviz::Subgraph's copy constructor); @p highlight may be destroyed by the
 * caller immediately after this call returns.
 *
 * This paints over, rather than replaces, any base colors set via
 * grRendererSetNodeColors / grRendererSetEdgeColors: elements in @p
 * highlight show the highlight color, and everything else keeps showing its
 * base color (or the global style, if none was set). Those base colors are
 * unaffected by this call and reappear as-is once the highlight is cleared
 * or replaced. Any prior accent vertex is cleared (see
 * grRendererSetAccentVertex); re-set it after the panel updates if needed.
 *
 * @return 0 on success, -1 on failure (no graph attached, or the renderer's
 *         attached graph has no backingGraph -- see grRendererSetGraph).
 */
int grRendererSetHighlight(grRenderer *r, const gviz::Subgraph &highlight,
                           uint32_t nodeRgba, uint32_t edgeRgba);

/**
 * Convenience for highlighting a planar face boundary: @p vertices lists the
 * dart-head cycle from the rotation system (as returned by planar face
 * enumeration). Consecutive entries are endpoints of one boundary dart.
 * Requires a non-NULL backingGraph (see grRendererSetGraph) -- fails
 * otherwise.
 */
int grRendererSetHighlightCycle(grRenderer *r, const size_t *vertices,
                                size_t count, uint32_t nodeRgba,
                                uint32_t edgeRgba);

/**
 * Clears the stored highlight. Elements revert to whatever base colors were
 * set via grRendererSetNodeColors / grRendererSetEdgeColors (unaffected by
 * this call), or the global node/edge styles for elements with no base
 * color set. Also clears any accent vertex (see grRendererSetAccentVertex).
 */
void grRendererClearHighlight(grRenderer *r);

/**
 * Accents a single vertex so it renders in @p rgba on top of both the client
 * base colors (grRendererSetNodeColors) and the active highlight. Compositing
 * order is: global style / base colors, then highlight, then accent. Use this
 * when an external UI (e.g. an HTML side panel listing highlight members)
 * wants one vertex visually distinguished without disturbing the highlight or
 * the base color layer.
 *
 * @p vertexId is a local vertex index (same space as
 * grRendererSetNodeColors / GR_ACTION_VERTEX_CLICKED's iarg -- see
 * grRendererSetGraph's INDEXING CONVENTION). @p rgba is GR_RGBA8 packed; pass
 * 0 to leave the accent slot set but skip painting (same convention as
 * highlight nodeRgba). Safe and cheap to call at interactive rates between
 * frames (O(1); marks colorsDirty so applyColorLayers repaints on the next
 * grRendererFrame). Does not require a frame to have run first, though an
 * out-of-range id is a no-op at paint time until posCapacity covers it.
 *
 * Lifetime:
 * - Cleared by grRendererClearAccentVertex, grRendererClearHighlight,
 *   grRendererSetHighlight / SetHighlightCycle (replacing the highlight set
 *   makes a prior accent stale), and grRendererSetGraph.
 * - Survives pure position updates and topology changes; at paint time an
 *   id >= current posCapacity is simply skipped.
 * - Does not modify the highlight subgraph or the client base color arrays.
 */
void grRendererSetAccentVertex(grRenderer *r, size_t vertexId, uint32_t rgba);

/** Clears any accent set by grRendererSetAccentVertex. No-op if none is set. */
void grRendererClearAccentVertex(grRenderer *r);

/** Parent-graph id of the accented vertex, or -1 if none. */
int64_t grRendererAccentVertex(const grRenderer *r);

// FRAME LOOP: -------------------------------------------------------------

/**
 * Runs one frame: polls window events, applies built-in navigation, dispatches
 * key-bound actions, re-uploads vertex positions, and draws.
 *
 * Typical usage:
 *
 *   while (grRendererFrame(r)) {
 *     // advance the embedding here if desired (positions are re-read
 *     // automatically on the next frame)
 *   }
 *
 * @return false when the window was closed, true otherwise.
 */
bool grRendererFrame(grRenderer *r);

/** Seconds since the previous grRendererFrame call (0 on the first frame). */
double grRendererDeltaTime(const grRenderer *r);

/** Reframes the camera to fit the bounding box of the live vertex positions.
 *  Also bound to the F key by default. */
void grRendererFitView(grRenderer *r);

/**
 * Sets the camera's orbit/pan pivot ("target") directly to (@p x, @p y,
 * @p z) in the graph's live embedding coordinates (already PCA-projected to
 * 3D for a 4D embedding, same space grRendererFocusVertex reads), without
 * touching distance, yaw, pitch, or roll. Unlike grRendererFitView/
 * grRendererFocusVertex, this never changes zoom -- useful for apps that
 * want to keep a custom point (e.g. a computed center of mass) as the pivot
 * for orbiting/panning while leaving the user's current framing alone.
 */
void grRendererSetCameraTarget(grRenderer *r, double x, double y, double z);

/**
 * Centers the camera on vertex @p vertexId and zooms in to frame a
 * neighborhood around it, sized as a fraction of the graph's current
 * visible bounding-box extent (same behavior as the C key and the console
 * "find <id>" command). Uses the renderer's last staged positions
 * (already PCA-projected to 3D for a 4D embedding), so it stays correct
 * across 2D/3D/4D without the caller redoing that projection.
 *
 * Safe to call from host code between frames and from a gviz Action
 * handler invoked via EmbeddedGraph::InvokeAction (including during
 * grRendererFrame's input processing). Applies immediately to the camera;
 * the next draw (or the remainder of the current frame, if called from an
 * action during that frame) reflects the new framing.
 *
 * No-op if @p r is NULL, @p vertexId is out of range of the currently
 * staged position buffer, or no position buffer has been allocated yet
 * (attach a graph with grRendererSetGraph and let at least one
 * grRendererFrame complete so positions are staged). Does not require the
 * vertex to be currently visible in the draw mask.
 */
void grRendererFocusVertex(grRenderer *r, size_t vertexId);

// STATS OVERLAY: ----------------------------------------------------------
//
// If the creator of the attached embedded graph records stat series on it
// (EmbeddedGraph::StatAppend, e.g. GRIP's "grip.heat"), the renderer charts
// them live in the top-right corner: one mini line chart per series, restyled
// per the series' gviz::layout::StatChartKind and autoscaled every frame. The
// renderer needs no knowledge of what the numbers mean.

/** Shows or hides the stats overlay. Visible by default (nothing is drawn
 *  when the graph has no non-empty stat series). Also toggled by the S key
 *  unless the app bound S itself. On macOS, also available under View. */
void grRendererShowStats(grRenderer *r, bool show);

/** Returns whether the stats overlay is currently enabled. */
bool grRendererStatsShown(const grRenderer *r);

/**
 * Returns the number of stat series registered on the attached embedded graph
 * (0 when no graph is attached).
 */
size_t grRendererStatSeriesCount(const grRenderer *r);

/** Returns the name of the @p idx-th stat series, or NULL if out of bounds. */
const char *grRendererStatSeriesName(const grRenderer *r, size_t idx);

/** Returns whether the chart for series @p idx is shown (rendering only). */
bool grRendererStatSeriesShown(const grRenderer *r, size_t idx);

/**
 * Shows or hides the chart for series @p idx. Does not affect data collection
 * on the embedded graph. No-op when @p idx is out of bounds.
 */
void grRendererShowStatSeries(grRenderer *r, size_t idx, bool show);

// COMMAND CONSOLE: ----------------------------------------------------------
//
// A stateless, in-window command line for driving renderer-level actions by
// typing them instead of clicking: press the backtick key (GR_CONSOLE_TOGGLE_
// KEY) to open a single input bar at the bottom of the window, type a command,
// press Enter to run it immediately (there is no session state beyond the
// current line -- each command is a one-shot dispatch), or Escape to close
// without running anything. Built-in commands:
//
//   find <id>   Clears the current highlight, highlights vertex <id> alone,
//               and focuses the camera on it -- centers it and zooms in to
//               comfortably frame it, the same as the C key does for the
//               vertex-list overlay's current selection (see VERTEX LIST
//               OVERLAY below).
//
// New commands are added in grConsole.cpp's command table, independent of the
// rest of the renderer.

/** Key that opens/closes the console (GLFW's grave-accent/backtick key,
 *  ASCII '`'). Not user-rebindable, like the stats (S) and texture-image (I)
 *  toggle keys -- pass it to grRendererBindKey/grRendererBindMouse and it
 *  will simply never fire, since the console intercepts it first. */
#define GR_CONSOLE_TOGGLE_KEY 96

/** Shows or hides the command console. Hidden by default; also toggled by
 *  GR_CONSOLE_TOGGLE_KEY. Hiding discards the current (unsubmitted) input
 *  line but leaves the last command's effects (highlight, camera, ...) in
 *  place. */
void grRendererShowConsole(grRenderer *r, bool show);

/** Returns whether the console is currently open. */
bool grRendererConsoleShown(const grRenderer *r);

// VERTEX INFO OVERLAY: -------------------------------------------------------
//
// A scrollable panel in the bottom-left corner showing the
// grRendererSetVertexLabels DATA string of the last vertex picked via
// GR_ACTION_PICK_VERTEX (or selected from the vertex-list overlay below).
// Draws nothing when no vertex is picked, the picked vertex has a NULL /
// empty label, no labels are set, or the panel is hidden via
// grRendererShowVertexInfo(false). Visible by default; there is no built-in
// key toggle (unlike the stats / vertex-list / console overlays).

/** Shows or hides the vertex-info overlay panel. Visible by default.
 *  Hiding suppresses the panel unconditionally -- even when labels are set
 *  and a vertex is currently picked. Does not clear the picked-vertex
 *  state or the label array; showing again restores the panel for whatever
 *  is currently picked. Survives grRendererSetGraph (like
 *  grRendererShowVertexList / grRendererShowCaption). */
void grRendererShowVertexInfo(grRenderer *r, bool show);

/** Returns whether the vertex-info overlay panel is currently enabled
 *  (independent of whether a vertex is picked or any label text is set). */
bool grRendererVertexInfoShown(const grRenderer *r);

// VERTEX LIST OVERLAY: -------------------------------------------------------
//
// A scrollable panel listing vertices as "Vertex N" (N is the local vertex
// index -- see grRendererSetGraph's INDEXING CONVENTION), with a search bar
// across the top. With no highlight active
// (see HIGHLIGHTS above), the list holds every currently-visible vertex;
// while a highlight is active, it holds only the highlighted vertices --
// e.g. clicking a vertex via GR_ACTION_PICK_VERTEX narrows the list to that
// vertex and its neighbors. Typing in the search bar further narrows
// whatever is currently listed to vertices whose grRendererSetVertexLabels
// DATA string fuzzy-matches the query (a vertex with no label, or no labels
// set at all, never matches a non-empty query); matches are ranked
// best-match-first. Visible by default; toggled by the L key unless the app
// bound L itself, same as the S (stats) and I (texture image) toggles.
//
// Click the search bar to give it keyboard focus (typed keys edit the query
// instead of driving camera navigation or bound actions); Enter or clicking
// elsewhere blurs it keeping the query, Escape while focused clears the
// query and blurs. Clicks anywhere else in the panel are consumed by it and
// never reach the graph, so interacting with the list never also picks a
// vertex or clears the current highlight.
//
// The Up/Down arrow keys move a selection cursor through the list (works
// even while the search box has focus, unless the app has bound those keys
// itself); the selected row is highlighted and its vertex becomes the
// picked vertex -- the same grRendererSetVertexLabels DATA string shown by
// clicking a vertex (see GR_ACTION_PICK_VERTEX) appears in the vertex-info
// panel, without changing the current highlight or the list's own contents.
// The C key centers the camera on the selected row's vertex and zooms in to
// comfortably frame it, unless the app has bound C itself -- the same
// center-and-zoom behavior as the console's "find <id>" command (see
// COMMAND CONSOLE above); a no-op if nothing is currently selected.

/** Shows or hides the vertex-list overlay. Visible by default. */
void grRendererShowVertexList(grRenderer *r, bool show);

/** Returns whether the vertex-list overlay is currently shown. */
bool grRendererVertexListShown(const grRenderer *r);

// CAPTION OVERLAY: -----------------------------------------------------------
//
// A single small text banner centered near the bottom of the window, for an
// application to narrate what's currently happening -- e.g. a teaching
// visualization's "comparing right contour of A against left contour of B"
// step description -- without building its own screen-space text drawing.
// Deliberately minimal: one string, no history, no scrolling, no markup.

/**
 * Sets the caption text, replacing any previous caption. Pass "" or NULL to
 * clear it (an empty caption draws nothing, even when shown). The string is
 * copied; the caller may reuse or free @p text immediately after this call
 * returns. A caption too wide to fit in one line is shrunk to fit rather
 * than wrapped -- keep captions to one short sentence.
 */
void grRendererSetCaption(grRenderer *r, const char *text);

/** Shows or hides the caption banner. Visible by default (nothing is drawn
 *  while the caption text is empty, regardless of this setting). */
void grRendererShowCaption(grRenderer *r, bool show);

/** Returns whether the caption banner is currently enabled (independent of
 *  whether any text is actually set). */
bool grRendererCaptionShown(const grRenderer *r);

// OBJECT OVERLAY: -----------------------------------------------------------
//
// Loads a Wavefront .obj mesh (only 'v' and 'f' lines are read; per-vertex
// normals are computed from face windings) and renders it in a small panel
// in the bottom-left corner, with its own camera that continuously orbits
// the object. The overlay is fully decoupled from the main scene: it never
// receives keyboard or mouse input, and its camera is independent of
// grRenderer's own camera, so the usual pan/zoom/orbit controls keep
// affecting only the main view.

/** Loads @p path and shows it in the rotating object overlay panel,
 *  replacing any previously loaded mesh. Positions the overlay's camera to
 *  fit the mesh; call between grRendererCreate and the frame loop, or at any
 *  point during it.
 *
 * @return 0 on success, -1 on file/parse error or GPU allocation failure. */
int grRendererLoadObjOverlay(grRenderer *r, const char *path);

/** Shows or hides the object overlay panel. No-op when no mesh is loaded.
 *  Visible by default once a mesh is loaded. */
void grRendererShowObjOverlay(grRenderer *r, bool show);

/** Returns whether the object overlay is currently visible and has a mesh
 *  loaded. */
bool grRendererObjOverlayShown(const grRenderer *r);

/** Releases the loaded mesh, if any, and hides the overlay. */
void grRendererClearObjOverlay(grRenderer *r);

// TEXTURE MAPPING: ------------------------------------------------------------
//
// Derives per-vertex (u, v) texture coordinates for an .obj mesh loaded into
// the object overlay from where a live 2D embedded graph's vertices land
// relative to a movable/resizable image rectangle in embedding space. This
// works because gviz::io::LoadFromObjFile (gviz) and the object overlay's own
// .obj parser both assign vertex id i to the i-th 'v' line of the file, in
// file order, so an embedding built on that same graph and a mesh loaded from
// the same file always agree on vertex correspondence. Mesh regions that
// don't overlap the image rectangle at all render white; regions that
// partially overlap it are textured normally, with (u, v) interpolated
// per-fragment across each triangle so the white/textured transition lands
// exactly at the image's edge. Texture mapping replaces the object overlay's
// flat-shaded preview while active.

/** Opaque; owned by the renderer's object overlay. */
typedef struct grTextureMap grTextureMap;

/**
 * Loads @p objPath into the object overlay (replacing any previously loaded
 * mesh, same as grRendererLoadObjOverlay) and ties it to @p graph, a 2D
 * embedded graph whose vertex count must match the mesh's. The initial image
 * rectangle is fit to the graph's current live bounding box, preserving
 * @p imagePath's aspect ratio.
 *
 * @return the texture map on success (borrowed; owned by @p r), or NULL on
 *         a non-2D embedding, vertex-count mismatch, file/parse error, or
 *         GPU allocation failure.
 */
grTextureMap *grRendererLoadTextureMap(grRenderer *r,
                                       gviz::layout::EmbeddedGraph &graph,
                                       const char *objPath,
                                       const char *imagePath);

/** Sets the image rectangle's center and half-extents, in embedding
 *  coordinates. NULL-safe (no-op). */
void grTextureMapSetImageRect(grTextureMap *tm, double cx, double cy,
                              double halfW, double halfH);

/** Translates the image rectangle's center by (@p dx, @p dy). NULL-safe. */
void grTextureMapMoveImage(grTextureMap *tm, double dx, double dy);

/** Scales the image rectangle's half-extents by @p factor about its current
 *  center. NULL-safe. */
void grTextureMapScaleImage(grTextureMap *tm, double factor);

/** Restores the image rectangle to the fit computed at load time. NULL-safe. */
void grTextureMapResetImage(grTextureMap *tm);

/** Reads back the current image rectangle. Any output pointer may be NULL.
 *  Writes zeros when @p tm is NULL. */
void grTextureMapGetImageRect(const grTextureMap *tm, double *cx, double *cy,
                              double *halfW, double *halfH);

/**
 * Shows or hides the movable image rectangle drawn directly in the main
 * scene, in the same embedding-space coordinates as the attached graph, so a
 * user can see exactly how the image and the live graph overlap. This is
 * independent of the object-overlay panel's always-on textured mesh preview.
 * Visible by default as soon as a texture map loads. No-op when @p r has no
 * active texture map.
 */
void grRendererShowTextureMapImage(grRenderer *r, bool show);

/** Returns whether the main-scene image rectangle is currently shown (false
 *  when no texture map is active). */
bool grRendererTextureMapImageShown(const grRenderer *r);

/** Requests that the frame loop end; the next grRendererFrame returns false. */
void grRendererRequestClose(grRenderer *r);

/**
 * Renders the current scene into an offscreen buffer and writes it to @p path
 * as a binary PPM image. Uses the current camera, styles, and positions; call
 * between frames. Blocks until the GPU readback completes.
 *
 * @return 0 on success, -1 on failure.
 */
int grRendererSaveScreenshot(grRenderer *r, const char *path);

#ifdef __cplusplus
}
#endif

#endif
