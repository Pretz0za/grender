#ifndef _GRENDER_H_
#define _GRENDER_H_

/**
 * grender - a GPU renderer for gviz embedded graphs.
 *
 * grender sits strictly on the consumer side of the gviz abstraction barrier:
 * it only reads embedded graphs through the public gvizEmbeddedGraph /
 * gvizSubgraph API and is agnostic to which embedding algorithm produced the
 * positions. It supports 2D and 3D embeddings directly, and 4D embeddings
 * projected to 3D with PCA for display. It renders millions of vertices
 * and edges in two instanced draw calls, and re-reads vertex positions every
 * frame so a live embedder (force-directed, GRIP rounds, ...) is rendered
 * online.
 *
 * Interaction is split in two layers:
 *  - Built-in navigation owned by the renderer: mouse pan/zoom (2D) or
 *    orbit/pan/dolly (3D), and 'grRendererFitView' framing.
 *  - Named actions owned by the embedded graph creator (see
 *    gvizEmbeddedGraphAddAction). The application binds input keys to action
 *    names with grRendererBindKey; the renderer dispatches payloads without
 *    knowing anything about the embedder.
 */

#include "embedders/gvizEmbeddedGraph.h"

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
} grNodeStyle;

typedef struct grEdgeStyle {
  grColor color; /**< Global color; overridden by per-edge colors. */
  float width;   /**< Edge width, interpreted per sizeMode. */
  grSizeMode sizeMode;
} grEdgeStyle;

typedef struct grRendererDesc {
  const char *title;   /**< Window title. NULL for a default. */
  uint32_t width;      /**< Initial window width in screen points. */
  uint32_t height;     /**< Initial window height in screen points. */
  grColor clearColor;  /**< Background color. */
  grNodeStyle nodeStyle;
  grEdgeStyle edgeStyle;
  bool vsync;          /**< true: FIFO present (default), false: immediate. */
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

/**
 * Attaches the embedded graph to render. The renderer reads structure through
 * the gviz subgraph API and positions through gvizEmbeddedGraphPositions every
 * frame, so position changes made by the caller (or by actions) between frames
 * are always visible. 2D and 3D embeddings are rendered directly; 4D
 * embeddings are PCA-projected to 3D each frame.
 *
 * @return 0 on success, -1 on unsupported dimension or GPU allocation failure.
 */
int grRendererSetGraph(grRenderer *r, gvizEmbeddedGraph *graph);

/**
 * Notifies the renderer that the *structure* of the attached graph changed
   * (vertices/edges added, removed, hidden or shown). Topology buffers are
   * rebuilt lazily before the next frame. Pure position changes and draw-mask
   * updates (via gvizEmbeddedGraphSetDrawMask) never require this call.
   */
  void grRendererGraphStructureChanged(grRenderer *r);

// STYLING: --------------------------------------------------------------------

/** Replaces the global node/edge styles. Cheap; takes effect next frame. */
void grRendererSetNodeStyle(grRenderer *r, const grNodeStyle *style);
void grRendererSetEdgeStyle(grRenderer *r, const grEdgeStyle *style);

/**
 * Uploads per-node fill colors (GR_RGBA8 packed), indexed by parent-graph
 * vertex id; @p count must equal gvizEmbeddedGraphPositionCount(). Pass NULL
 * to revert to the global style. The data is copied to the GPU; the caller
 * keeps ownership.
 *
 * @return 0 on success, -1 on failure.
 */
int grRendererSetNodeColors(grRenderer *r, const uint32_t *rgba8, size_t count);

/** Per-node radii, same indexing rules as grRendererSetNodeColors. */
int grRendererSetNodeSizes(grRenderer *r, const float *radii, size_t count);

/**
 * Per-edge colors, indexed in edge-buffer order: the order edges are produced
 * by iterating subgraph vertices in increasing id and their neighbor
 * iterators (undirected edges appear once, with u < v). Use
 * grRendererEdgeCount / grRendererGetEdges to inspect that order.
 */
int grRendererSetEdgeColors(grRenderer *r, const uint32_t *rgba8, size_t count);

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
  GR_KEY_RIGHT = 262,
  GR_KEY_LEFT = 263,
  GR_KEY_DOWN = 264,
  GR_KEY_UP = 265,
  GR_KEY_F1 = 290, /* F2..F12 follow consecutively */
};

/** Modifier bits reported in gvizActionPayload.iarg (bitwise OR). */
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
 * Requires a planar combinatorial embedding (gvizEmbeddedGraphApplyPlanarEmbedding).
 */
#define GR_ACTION_PICK_FACE "grender.pickFace"

/**
 * Binds @p key so that pressing it invokes the action named @p actionName on
 * the attached embedded graph (see gvizEmbeddedGraphAddAction). The payload is
 * filled by the renderer:
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

// HIGHLIGHTS: ---------------------------------------------------------------

/**
 * Stores @p highlight (a gvizSubgraph on the same parent graph as the
 * attached embedded graph) and colors its vertices/edges differently from the
 * global node/edge styles every frame. Pass 0 for @p nodeRgba or @p edgeRgba
 * to keep the global style for that element class. The subgraph is copied;
 * @p highlight may be released by the caller afterward.
 *
 * @return 0 on success, -1 on failure.
 */
int grRendererSetHighlight(grRenderer *r, const gvizSubgraph *highlight,
                           uint32_t nodeRgba, uint32_t edgeRgba);

/**
 * Convenience for highlighting a planar face boundary: @p vertices lists the
 * dart-head cycle from the rotation system (as returned by planar face
 * enumeration). Consecutive entries are endpoints of one boundary dart.
 */
int grRendererSetHighlightCycle(grRenderer *r, const size_t *vertices,
                                size_t count, uint32_t nodeRgba,
                                uint32_t edgeRgba);

/** Clears the stored highlight and reverts to global node/edge styles. */
void grRendererClearHighlight(grRenderer *r);

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

// STATS OVERLAY: ----------------------------------------------------------
//
// If the creator of the attached embedded graph records stat series on it
// (gvizEmbeddedGraphStatAppend, e.g. GRIP's "grip.heat"), the renderer charts
// them live in the top-right corner: one mini line chart per series, restyled
// per the series' gvizStatChartKind and autoscaled every frame. The renderer
// needs no knowledge of what the numbers mean.

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
