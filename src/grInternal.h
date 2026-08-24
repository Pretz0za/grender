#ifndef _GRENDER_INTERNAL_H_
#define _GRENDER_INTERNAL_H_

#include "grender/grender.h"
#include "grProfiling.h"

#include "gviz.hpp"

#include <webgpu/webgpu.h>

// Every overlay (stats charts, vertex list, vertex-info panel, caption
// banner, command console) is an ImGui/ImPlot window -- see grStats.cpp,
// grListOverlay.cpp, grVertexOverlay.cpp, grCaption.cpp, grConsole.cpp.
// Included here (rather than separately in each of those .cpp files) since
// every one of them already pulls in grInternal.h. The imgui_impl_glfw.h/
// imgui_impl_wgpu.h backend headers are NOT included here -- only
// grRenderer.cpp needs them, for the Init/NewFrame/Shutdown lifecycle calls
// next to the GLFW/WebGPU device they wrap.
#include "imgui.h"
#include "implot.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// grender never hand-rolls a growable array: every dynamically-sized list in
// this codebase (pending input events, key/mouse bindings, stats primitives,
// ...) is a std::vector. Reach for std::vector before writing another
// malloc/realloc-doubling loop.

/**
 * Binds a compound-literal temporary (e.g. `grPtr(WGPUBufferDescriptor{...})`)
 * to a const reference parameter -- which, per C++'s ordinary temporary
 * lifetime rules, keeps it alive through the end of the full expression that
 * calls grPtr() -- and returns its address. wgpu-native's C API takes
 * `const T*` descriptor arguments almost everywhere; the old C code built
 * those inline with `&(const T){...}`, which relied on C's rule that a
 * compound literal is an lvalue. C++ compound literals (a Clang extension,
 * used throughout this codebase instead of hand-declaring a named temporary
 * for every single descriptor struct) are prvalues, so `&` directly on one is
 * ill-formed; routing through this template is the minimal-diff fix that
 * keeps the inline-descriptor style intact.
 */
template <typename T> const T *grPtr(const T &v) noexcept { return &v; }

typedef struct GLFWwindow GLFWwindow;

// ------------------------------------------------------------------------------
// Structural type erasure over gviz::GraphLike
// ------------------------------------------------------------------------------
//
// gviz::layout::EmbeddedGraph -- the one type every embedder publicly
// inherits, and the only type grender's rendering path is meant to be
// agnostic to which embedder produced -- carries no structural information
// of its own anymore (see EmbeddedGraph.hpp): no Structure(), no
// OutNeighbors/InNeighbors, no highlight. Every concrete embedder is now a
// class template over `gviz::GraphLike G` (typically gviz::Graph or
// gviz::Subgraph) and owns its own `G structure_`, reachable only through a
// *typed* Structure() accessor (or, for the non-templated Graph-only
// embedders -- Planar, ReingoldTilford, SchnyderWood -- not reachable from
// the embedder at all; the caller's own retained Graph& is the only route).
//
// grender's whole design assumes one non-template entry point
// (grRendererSetGraph) can attach *any* embedded graph. Since G is now a
// compile-time parameter, that entry point has to become a template itself
// (see grender.h) -- but everything downstream of it (grTopology.cpp,
// picking, highlight, the vertex-list overlay, ...) should stay ordinary,
// non-template code, exactly as before. grGraphStructure is the local type
// erasure that makes that possible: a small abstract interface over exactly
// what those consumers need (vertex count, directedness, raw<->local
// translation, adjacency), implemented once per G by the template below and
// stored as a single owned pointer on grRenderer.
//
// "Local index" here always means the SAME index space as
// gviz::layout::EmbeddedGraph::Positions() -- [0, structure->VertexCount())
// -- since that's what grender uploads to the GPU every frame. "Raw" means
// G's own native vertex handle (Graph::Size()'s dense range for Graph;
// the parent graph's own, generally non-contiguous, ids for Subgraph). This
// is also grender's whole public API's indexing convention now (see
// grRendererSetGraph's doc comment): every by-index part of that API
// (grRendererSetNodeColors, picking results, grRendererSetAccentVertex,
// grRendererGetEdges, ...) is LOCAL-indexed, matching Positions() exactly,
// which happens to be a strictly better fit than the old "parent-graph id"
// convention for a Subgraph-backed embedding (no more sizing per-vertex GPU
// arrays to the parent graph's id range just to address a handful of
// sparsely-scattered view vertices).
class grGraphStructure {
public:
  virtual ~grGraphStructure() = default;

  /** Number of vertices in the view -- == the attached embedding's
   *  PositionCount(). Every local index below is < this. */
  virtual size_t VertexCount() const = 0;

  virtual bool IsDirected() const = 0;

  /** Native handle -> local index. Unchecked: @p raw must be a handle the
   *  structure actually has. */
  virtual size_t RawToLocal(size_t raw) const = 0;

  /** Local index -> native handle. Unchecked: @p local must be <
   *  VertexCount(). */
  virtual size_t LocalToRaw(size_t local) const = 0;

  /** Appends the LOCAL indices of @p local's neighbors to @p out (does not
   *  clear it first). Unchecked: @p local must be < VertexCount(). */
  virtual void NeighborsLocal(size_t local, std::vector<uint32_t> &out) const = 0;
};

/** Concrete grGraphStructure over a specific GraphLike @p G, built once from
 *  a non-owning reference to the caller's own structure (see
 *  grRendererSetGraph<G>'s doc comment for why grender never copies or owns
 *  it) plus a gviz::DenseIndex<G> grender builds itself. That index is
 *  guaranteed to match the attached embedder's own internal DenseIndex<G>
 *  exactly, since gviz::DenseIndex's bijection is a pure, deterministic
 *  function of @p G's vertex range (see DenseIndex.hpp) and grRendererSetGraph
 *  is always called with a reference to the very same structure object the
 *  embedder itself was built over (either literally the embedder's own
 *  Structure(), or the caller's own Graph& that a non-templated embedder
 *  like Planar holds a reference to rather than a move-in copy of). */
template <gviz::GraphLike G>
class grGraphStructureImpl : public grGraphStructure {
public:
  explicit grGraphStructureImpl(G &structure)
      : structure_(structure), index_(structure) {}

  size_t VertexCount() const override { return index_.Size(); }
  bool IsDirected() const override {
    return gviz::GraphLikeIsDirected(structure_);
  }
  size_t RawToLocal(size_t raw) const override { return index_.ToLocal(raw); }
  size_t LocalToRaw(size_t local) const override { return index_.ToRaw(local); }
  void NeighborsLocal(size_t local, std::vector<uint32_t> &out) const override {
    size_t raw = index_.ToRaw(local);
    for (size_t nb : structure_.Neighbors(raw))
      out.push_back((uint32_t)index_.ToLocal(nb));
  }

private:
  G &structure_;
  gviz::DenseIndex<G> index_;
};

// ------------------------------------------------------------------------------
// Camera
// ------------------------------------------------------------------------------

/**
 * One camera drives both projections. 2D embeddings use an orthographic
 * top-down view (yaw/pitch locked); 3D embeddings use a perspective orbit
 * camera. `target` is the world-space point the camera looks at / pans with.
 */
typedef struct grCamera {
  bool perspective;
  double target[3];
  double yaw;      /**< Radians around +Z of the orbit direction (3D only). */
  double pitch;    /**< Radians above the XY plane (3D only). */
  double distance; /**< Eye distance from target (3D); also drives 2D zoom:
                        pixelsPerWorld = viewportHeight / distance. */
  double roll;     /**< Radians of rotation about the view/forward axis:
                        rotates the right/up basis so panning, zooming, and
                        unprojection all stay correct relative to the rolled
                        view. In 2D this is the only way the view rotates; in
                        3D it rolls the perspective camera about its look
                        direction independently of yaw/pitch. */
} grCamera;

/** Per-frame camera-derived values consumed by the shaders and by picking. */
typedef struct grCameraFrame {
  float viewProj[16]; /**< Column-major, world -> clip. */
  float camRight[3];  /**< World-space billboard axes. */
  float camUp[3];
  float proj11;       /**< Projection [1][1]; converts clip w to px/world. */
  double eye[3];
  double forward[3];
} grCameraFrame;

void grCameraInit2D(grCamera *cam);
void grCameraInit3D(grCamera *cam);
void grCameraOrbit(grCamera *cam, double dYaw, double dPitch);
/** Rolls the view by @p dAngle radians about its forward axis (in 2D this is
 *  the view's only rotation; in 3D it rolls the perspective camera about its
 *  look direction, independent of grCameraOrbit's yaw/pitch). Positive is
 *  counter-clockwise on screen. */
void grCameraRoll(grCamera *cam, double dAngle);
void grCameraZoom(grCamera *cam, double factor);
/** Pans by a screen-space delta in pixels, keeping content under the cursor. */
void grCameraPanPixels(grCamera *cam, double dxPx, double dyPx,
                       double viewportHPx);
void grCameraFrameCompute(const grCamera *cam, double viewportWPx,
                          double viewportHPx, grCameraFrame *out);
/** Unprojects a window-space pixel onto the world plane through the camera
 *  target (the embedding plane itself in 2D). */
void grCameraUnproject(const grCamera *cam, const grCameraFrame *frame,
                       double xPx, double yPx, double viewportWPx,
                       double viewportHPx, double *worldX, double *worldY);
/** Frames an axis-aligned bounding box. */
void grCameraFitBox(grCamera *cam, const double bmin[3], const double bmax[3],
                    double viewportWPx, double viewportHPx);

// ------------------------------------------------------------------------------
// Topology extraction (the only code that walks gviz structures)
// ------------------------------------------------------------------------------

/**
 * CPU-side mirror of the graph structure, rebuilt only when the caller reports
 * a structural change. Arrays are owned by the topology. Doubles as grender's
 * own local substitute for the synced out/in adjacency snapshot
 * gviz::layout::EmbeddedGraph used to maintain (OutNeighbors/InNeighbors,
 * removed along with Sync() -- see EmbeddedGraph.hpp): outOffsets/outNbrs
 * (and inOffsets/inNbrs, directed graphs only) cover EVERY local vertex,
 * not just the currently visible ones, so vertex-pick highlighting
 * (grenderActionPickVertex) can walk a vertex's neighbors without going back
 * to gviz. Every id in this struct is a LOCAL index (see grGraphStructure's
 * doc comment above), not a raw/native handle.
 */
typedef struct grTopology {
  uint32_t *nodeIds; /**< Draw instance -> local vertex index, visible only. */
  size_t nodeCount;
  uint32_t *edges;   /**< Flat (u, v) pairs of local vertex indices. */
  size_t edgeCount;
  /** Whether the attached structure is directed. When true, each (u, v) pair
   *  in edges is stored as (from, to) rather than the u < v-deduplicated
   *  pairs used for undirected graphs, and the edge pipeline draws an
   *  arrowhead at v. */
  bool directed;

  // Full adjacency CSR, LOCAL-indexed, sized over every vertex in the
  // structure (VertexCount() + 1 offsets) regardless of draw-mask
  // visibility -- see the class doc.
  std::vector<uint32_t> outOffsets, outNbrs;
  std::vector<uint32_t> inOffsets, inNbrs; // directed only; empty otherwise

  size_t vertexCount = 0; // == outOffsets.size() - 1 when built, else 0
} grTopology;

/**
 * Extracts the full adjacency CSR (outOffsets/outNbrs, inOffsets/inNbrs) from
 * @p structure, then visible vertices and edges (filtered through @p
 * embedding's draw mask) from it. Undirected edges are emitted once (u < v);
 * directed edges as stored.
 *
 * @return 0 on success, -1 on allocation failure.
 */
int grTopologyExtract(grTopology *topo, grGraphStructure &structure,
                      gviz::layout::EmbeddedGraph &embedding);
void grTopologyRelease(grTopology *topo);

/** Total degree (out + in, if directed) of local vertex @p v as the drawn
 *  structure defines it, read from @p topo's own CSR. */
uint32_t grTopologyVertexDegree(const grTopology *topo, size_t v);

// ------------------------------------------------------------------------------
// Stats overlay (charts for gviz::layout::StatSeries recorded by the embedder)
// ------------------------------------------------------------------------------

struct grRenderer;

/**
 * Draws an ImGui window ("Stats") holding one ImPlot line chart per
 * non-empty, currently-shown stat series of the attached graph, stacked
 * top-to-bottom. Must run between ImGui::NewFrame() and ImGui::Render() (see
 * grRendererFrame). Only reads the graph through EmbeddedGraph::StatSeries*;
 * no-op when no graph is attached. Autoscales every frame -- ImPlot recomputes
 * axis limits from the live sample data itself, so there is no cached range
 * to invalidate the way the old bitmap-font renderer needed.
 */
void grStatsOverlayBuild(struct grRenderer *r);

/**
 * Draws an ImGui window ("Vertex Info") showing the grRendererSetVertexLabels
 * label of the last vertex picked via GR_ACTION_PICK_VERTEX (or selected from
 * the vertex-list overlay), word-wrapped by ImGui::TextWrapped. Draws nothing
 * (not even an empty window) when the panel is hidden
 * (grRendererShowVertexInfo), no vertex is picked, or it has no label -- see
 * grRendererShowVertexInfo's doc comment for the exact contract. Labels come
 * entirely from grRendererSetVertexLabels; this never reads gviz's graph
 * loader directly.
 */
void grVertexOverlayBuild(struct grRenderer *r);

// ------------------------------------------------------------------------------
// Vertex list overlay (scrollable list of vertices, filtered by the active
// highlight and by a fuzzy search over vertex DATA labels; see
// grListOverlay.cpp)
// ------------------------------------------------------------------------------

/**
 * Draws an ImGui window ("Vertices") holding the search bar
 * (ImGui::InputText, writing into r->listSearchInput) and the scrollable,
 * filtered vertex list below it. When r->listFilterDirty, first rebuilds
 * r->listFilteredIds: the vertices of the active highlight (or, with no
 * highlight active, every visible vertex in the current topology), further
 * narrowed to those whose grRendererSetVertexLabels DATA string
 * fuzzy-matches r->listSearchInput (vertices with a NULL label never match a
 * non-empty query), ranked best-match-first when a query is active. Labels
 * are read exactly as supplied -- this never touches gviz's graph loader
 * directly, the same "supplied by the caller, indexed by parent-graph vertex
 * id" contract as grVertexOverlayBuild. Sets r->listSearchInputFocused to
 * whether the search box has ImGui keyboard focus as of this call, which
 * processInput (grRenderer.cpp) reads to decide whether Escape should clear
 * the query. No-op when the overlay is hidden or no graph is attached.
 */
void grListOverlayBuild(struct grRenderer *r);

/**
 * Draws an ImGui window (no title bar, semi-transparent, centered near the
 * bottom of the window) holding r->captionText, if r->captionVisible and
 * r->captionText is non-empty. @p fbw/@p fbh position it relative to the
 * current framebuffer size. See grCaption.cpp.
 */
void grCaptionBuild(struct grRenderer *r, double fbw, double fbh);

/**
 * Moves the list's selection cursor by @p delta (+1/-1) within
 * r->listFilteredIds, wiring the newly selected row up exactly like clicking
 * its vertex would for the vertex-info panel: sets r->pickedVertexId so that
 * vertex's grRendererSetVertexLabels DATA string shows there, without
 * touching the current highlight or re-running the list's own filter.
 * Scrolls the newly selected row into view via ImGui::SetScrollHereY the
 * next time grListOverlayBuild runs. No-op when the list is empty.
 */
void grListOverlaySelectDelta(struct grRenderer *r, int delta);

// ------------------------------------------------------------------------------
// Command console (stateless command line, e.g. "find <id>"; see grConsole.cpp)
// ------------------------------------------------------------------------------

/** Opens the console: shows the input bar and clears any leftover input/
 *  result from a previous session. No-op if already open. */
void grConsoleOpen(struct grRenderer *r);

/** Closes the console, discarding the current (unsubmitted) input line. */
void grConsoleClose(struct grRenderer *r);

/**
 * Parses @p line as "<command> [args...]" (whitespace-separated) and runs it
 * against the console's built-in command table, writing a result or error
 * message into r->consoleMessage for the next grConsoleBuild to display. An
 * empty or all-whitespace line is a silent no-op.
 */
void grConsoleRun(struct grRenderer *r, const char *line);

/**
 * Draws an ImGui window (no title bar, docked to the bottom of the window,
 * @p fbw/@p fbh wide) holding the input line (ImGui::InputText with
 * ImGuiInputTextFlags_EnterReturnsTrue -- Enter runs it via grConsoleRun and
 * clears the buffer) and the last command's result/error message below it.
 * No-op when the console is closed.
 */
void grConsoleBuild(struct grRenderer *r, double fbw, double fbh);

/**
 * PCA-project @p n vertex-major points from @p srcDim to 3D into @p dst
 * (n * 3 floats). Falls back to copying the leading components when
 * @p srcDim <= 3.
 *
 * @p basisOut/@p basisIn hold srcDim * 3 eigenvectors (row-major); pass NULL
 * to ignore. When @p basisIn is set, signs are aligned to reduce frame jumps.
 *
 * @return 0 on success, -1 on failure.
 */
int grPCAProjectTo3(const double *src, size_t n, size_t srcDim, float *dst,
                    double *basisOut, const double *basisIn);

// ------------------------------------------------------------------------------
// Object overlay (rotating picture-in-picture preview of a loaded Wavefront
// .obj mesh, fully independent of the attached embedded graph and its camera)
// ------------------------------------------------------------------------------

/** CPU-side triangle mesh parsed from a .obj file. Only 'v' and 'f' lines are
 *  read; per-vertex normals are the area-weighted average of adjacent face
 *  normals. Owns its arrays. */
typedef struct grObjMesh {
  float *positions;   /**< vertexCount * 3 (xyz). */
  float *normals;     /**< vertexCount * 3 (xyz), unit length. */
  uint32_t *indices;  /**< indexCount, 3 per triangle. */
  uint32_t *triangleFaceIds; /**< indexCount/3 entries; one per emitted
                                   triangle, = the 0-based index of the 'f'
                                   line it was fan-triangulated from. */
  size_t vertexCount;
  size_t indexCount;
  size_t faceCount; /**< Number of 'f' lines parsed. */
  double bmin[3], bmax[3];
} grObjMesh;

int grObjMeshLoad(const char *path, grObjMesh *out);
void grObjMeshRelease(grObjMesh *mesh);

/** Shared WGPU storage-buffer helper: rounds @p bytes up to a 4-byte,
 *  >=4-byte size and optionally uploads @p data immediately. Used by the
 *  object overlay and the texture map for their read-only storage buffers. */
WGPUBuffer grMakeStorageBuffer(grRenderer *r, const void *data, size_t bytes,
                               const char *label);

/** Must match struct ObjGlobals in grShaders.h. */
typedef struct grObjOverlayUBO {
  float viewProj[16];
  float lightDir[4];
  float baseColor[4];
  float panelSizePx[4]; /**< xy used, zw padding. */
  float texFlags[4]; /**< x: 1.0 when a texture map is active else 0.0. */
} grObjOverlayUBO;

/** Must match struct ImgGlobals in grShaders.h. */
typedef struct grTexMapImageUBO {
  float viewProj[16];
  float rectCenter[2];
  float rectHalfExtent[2];
  float opacity;
  float pad[3];
} grTexMapImageUBO;

/**
 * Live UV mapping tying a 2D gviz::layout::EmbeddedGraph's vertex positions to
 * a movable/resizable image rectangle in embedding space, reprojected as
 * texture coordinates for the object overlay's mesh. Owned by grObjOverlay;
 * `graph` itself is a borrowed pointer.
 */
typedef struct grTextureMap {
  bool active;
  bool visible; /**< Whether the image rect also draws in the main scene. */
  gviz::layout::EmbeddedGraph *graph = nullptr; /**< Not owned; must be a 2D
                                                      embedding. */
  double imgCenter[2], imgHalfExtent[2];
  double initCenter[2], initHalfExtent[2]; /**< For grTextureMapResetImage. */
  float *uvStaging;          /**< vertexCount * 2, rebuilt every frame. */
  uint32_t *insideStaging;   /**< vertexCount scratch (0/1). */
  uint32_t *faceValidStaging; /**< faceCount scratch (0/1). */
  uint32_t *triValidStaging; /**< indexCount/3, uploaded every frame. */
  WGPUBuffer uvBuf;
  WGPUBuffer triValidBuf;
  WGPUTexture imageTexture;
  WGPUTextureView imageView;
  WGPUSampler imageSampler;
  int imageW, imageH;

  /** Lazily-created pipeline drawing the image rect directly in the main
   *  scene (grRenderer's own camera/pass), reusing imageTexture/imageView/
   *  imageSampler above. Independent of the object-overlay's own pipeline. */
  WGPUShaderModule imgQuadShaderModule;
  WGPUBindGroupLayout imgQuadBindGroupLayout;
  WGPUPipelineLayout imgQuadPipelineLayout;
  WGPURenderPipeline imgQuadPipeline;
  WGPUBuffer imgQuadUniformBuf;
  WGPUBindGroup imgQuadBindGroup;
} grTextureMap;

typedef struct grObjOverlay {
  bool loaded;
  bool visible;
  grObjMesh mesh;
  grCamera camera; /**< Independent of grRenderer::camera; never reads input. */

  WGPUShaderModule shaderModule;
  WGPUBindGroupLayout bindGroupLayout;
  WGPUPipelineLayout pipelineLayout;
  WGPURenderPipeline bgPipeline;
  WGPURenderPipeline meshPipeline;

  WGPUBuffer uniformBuf;
  WGPUBuffer positionsBuf;
  WGPUBuffer normalsBuf;
  WGPUBuffer indicesBuf;
  WGPUBindGroup bindGroup;
  bool bindGroupDirty;

  /** Lazily-created placeholders bound at slots 4-6 whenever texMap.active is
   *  false (or no mesh has ever been loaded), so the bind group is always
   *  complete WGPU state. Overlay-lifetime, released only in
   *  grObjOverlayRelease. */
  WGPUBuffer dummyUvBuf;
  WGPUTexture dummyTexture;
  WGPUTextureView dummyView;
  WGPUSampler dummySampler;

  grTextureMap texMap;
} grObjOverlay;

/** Parses @p path and swaps it in as the overlay's mesh, replacing any
 *  previous one. Lazily creates the overlay's GPU pipelines on first use.
 *  Positions the overlay's own orbiting camera to fit the mesh.
 *
 * @return 0 on success, -1 on parse or GPU allocation failure. */
int grObjOverlayLoad(grRenderer *r, const char *path);

/** Frees the loaded mesh (CPU and GPU) and resets overlay state. Safe to call
 *  with no mesh loaded. */
void grObjOverlayClear(grRenderer *r);

/** Advances the overlay's orbit camera by @p dt seconds. No-op when no mesh
 *  is loaded. */
void grObjOverlayUpdate(grRenderer *r, double dt);

/** Encodes a render pass drawing the overlay panel into the bottom-left
 *  corner of @p colorTarget, reusing @p depthView (cleared fresh) as its
 *  depth attachment. No-op when no mesh is loaded, the overlay is hidden, or
 *  the framebuffer is too small to fit the panel. */
void grObjOverlayEncode(grRenderer *r, WGPUCommandEncoder encoder,
                        WGPUTextureView colorTarget, WGPUTextureView depthView,
                        double fbw, double fbh);

/** Releases all GPU resources owned by the overlay. Called from
 *  grRendererDestroy. */
void grObjOverlayRelease(grRenderer *r);

/** Recomputes uv/triValid staging from the live graph and uploads the GPU
 *  buffers. No-op when no texture map is active. */
void grTextureMapUpdate(grRenderer *r);

/** Frees the texture map's staging arrays and GPU resources (image
 *  texture/view/sampler, uv/triValid buffers) and resets it to a zeroed,
 *  inactive state. Safe to call repeatedly and when no texture map was ever
 *  loaded. Does not touch the mesh itself (owned by grObjOverlay). Called
 *  from grObjOverlayClear/grObjOverlayRelease. */
void grTextureMapRelease(grRenderer *r);

/** Encodes the image-rect quad directly into the main scene's render pass
 *  (@p pass already active, same pass as nodes/edges), using @p r's current
 *  camera frame so the quad lines up with the live graph. No-op when no
 *  texture map is active or grTextureMapShowImage(false) was called. Drawn
 *  before nodes/edges by the caller so the graph remains visible on top. */
void grTextureMapEncodeImageQuad(grRenderer *r, WGPURenderPassEncoder pass);

/** Pure CPU math (no GPU/gviz types) behind grTextureMapUpdate, split out so
 *  it is directly unit-testable. For each of @p vertexCount 2D positions,
 *  computes its (u, v) inside the rect defined by @p center/@p halfExtent
 *  (u,v in [0,1] when inside; v is flipped so row 0 is "top", matching
 *  stbi_load's row order) and whether it falls inside that rect. */
void grTextureMapComputeUV(const double *pos2D, size_t vertexCount,
                           const double center[2], const double halfExtent[2],
                           float *uvOut, uint32_t *insideOut);

/** Pure CPU math behind grTextureMapUpdate: derives per-face and per-triangle
 *  validity from per-vertex insideness. A face (possibly an n-gon,
 *  fan-triangulated into several triangles sharing @p triangleFaceIds) is
 *  valid only if every triangle emitted from it has all 3 vertices inside;
 *  that per-face result is then propagated back to every triangle belonging
 *  to it, so triangles are only ever marked valid together with the rest of
 *  their originating face. */
void grTextureMapComputeFaceValidity(const uint32_t *insideOut,
                                     size_t vertexCount,
                                     const uint32_t *indices,
                                     const uint32_t *triangleFaceIds,
                                     size_t triangleCount, size_t faceCount,
                                     uint32_t *faceValidOut,
                                     uint32_t *triValidOut);

// ------------------------------------------------------------------------------
// Platform
// ------------------------------------------------------------------------------

/** Creates a WGPUSurface for @p window (per-OS implementation). */
WGPUSurface grPlatformCreateSurface(WGPUInstance instance, GLFWwindow *window);

/** macOS: registers the app and installs the menu bar; no-op elsewhere. */
void grPlatformInitApplication(void);

/** Refreshes the Charts submenu from the attached graph's stat series. */
void grPlatformStatsMenuRefresh(struct grRenderer *r);

/** Refreshes the View menu's "Show Texture Image" checkbox from the current
 *  texture map state (enabled/checked only while a texture map is active). */
void grPlatformTextureMapMenuRefresh(struct grRenderer *r);

// ------------------------------------------------------------------------------
// Renderer
// ------------------------------------------------------------------------------

typedef struct grKeyBinding {
  int key;
  const char *actionName;
} grKeyBinding;

typedef struct grMouseBinding {
  int button;
  const char *actionName;
} grMouseBinding;

typedef struct grPendingKey {
  int key;
  int mods;
} grPendingKey;

typedef struct grPendingMouse {
  int button;
  int mods;
  double xPx;
  double yPx;
} grPendingMouse;

/** Must match struct Globals in grShaders.h (16-byte aligned rows). */
typedef struct grGlobalsUBO {
  float viewProj[16];
  float camRight[4];
  float camUp[4];
  float viewport[2];
  uint32_t posDim;
  uint32_t flags; /**< bit0: per-node color, bit1: per-node size,
                       bit2: per-edge color, bit3: edge degree-alpha,
                       bit4: edge weight-width, bit5: directed (draw
                       arrowheads), bit6: node shape is rounded-square
                       instead of circle (see grNodeStyle::roundedSquare),
                       bit7: per-edge dashed (see grRendererSetEdgeDashed),
                       bit8: node degree-scale (see
                       grRendererSetNodeDegreeScale). */
  float nodeFill[4];
  float nodeStroke[4];
  /** x: radius, y: strokeWidth, z: sizeMode (0 px / 1 world), w: proj11. */
  float nodeParams[4];
  /** x: minPixelRadius, y: maxPixelRadius (0 disables each), z: corner
   *  radius fraction (grNodeStyle::cornerRadiusFraction, read only when
   *  bit6 of flags is set), w: node degree-scale factor (read only when
   *  bit8 of flags is set; see grRendererSetNodeDegreeScale). */
  float nodeSizeLimits[4];
  float edgeColor[4];
  /** x: width, y: sizeMode, z: maxDegree (degree-alpha),
   *  w: mean edge weight (weight-width). */
  float edgeParams[4];
} grGlobalsUBO;

struct grRenderer {
  // window / device
  GLFWwindow *window;
  WGPUInstance instance;
  WGPUSurface surface;
  WGPUAdapter adapter;
  WGPUDevice device;
  WGPUQueue queue;
  /** From @ref wgpuDeviceGetLimits after creation; used to validate uploads. */
  uint64_t maxStorageBufferBindingSize;
  uint64_t maxBufferSize;
  WGPUSurfaceConfiguration surfaceConfig;
  WGPUTextureFormat surfaceFormat;
  bool surfaceDirty; /**< Reconfigure surface + depth before next frame. */

  // pipelines (created on first grRendererSetGraph)
  WGPUShaderModule shaderModule;
  WGPUBindGroupLayout bindGroupLayout;
  WGPUPipelineLayout pipelineLayout;
  WGPURenderPipeline nodePipeline;
  WGPURenderPipeline edgePipeline;
  WGPUTexture depthTexture;
  WGPUTextureView depthView;

  // graph data
  gviz::layout::EmbeddedGraph *graph = nullptr; /**< Not owned. */
  /** Type-erased view over whatever gviz::GraphLike structure @p graph's
   *  embedder was built over -- see grGraphStructure's doc comment above and
   *  grRendererSetGraph<G>'s. Owned; rebuilt (not merely cleared) by every
   *  grRendererSetGraph call. Null only before the first attach. */
  std::unique_ptr<grGraphStructure> structureView;
  /** Not owned. Backs the raw-graph-only operations (EnsureLayout,
   *  GetEdgeWeight, planar face queries) that gviz::Subgraph deliberately
   *  never exposes -- see grRendererSetGraph's doc comment. NULL is
   *  tolerated everywhere it's read: the features that need it just no-op
   *  (return/find nothing) instead of crashing. Automatically equal to the
   *  attached structure itself when that structure is a gviz::Graph (see
   *  grRendererSetGraph<G>). */
  gviz::Graph *backingGraph = nullptr;
  /** The active highlight, if any (grRendererSetHighlight/SetHighlightCycle),
   *  a full subgraph over @ref backingGraph. Lives here now, not on the
   *  attached embedding -- gviz::layout::EmbeddedGraph no longer carries a
   *  highlight at all (selection/highlight state is presentation state, and
   *  the base class now knows nothing about GraphLike structure to begin
   *  with) -- see grRendererSetHighlight's doc comment. Raw/native-id
   *  addressed, like any gviz::Subgraph; translate through structureView
   *  when painting local-indexed GPU colors from it (see applyColorLayers). */
  std::optional<gviz::Subgraph> highlight;
  grTopology topo;
  bool topoDirty;
  uint64_t drawMaskRevision;
  size_t posCapacity;   /**< Vertices the GPU buffers are sized for. */
  size_t srcDim;        /**< Embedding dimension from the attached graph. */
  size_t posDim;        /**< Dimension uploaded to the GPU (3 when srcDim==4). */
  float *posStaging;    /**< Persistent double->float conversion buffer. */
  double pcaBasis[48];  /**< Cached PCA eigenvectors (up to 4D * 3). */
  bool pcaBasisValid;
  WGPUBuffer globalsBuf;
  WGPUBuffer positionsBuf;
  WGPUBuffer nodeIdsBuf;
  WGPUBuffer nodeColorsBuf;
  WGPUBuffer nodeSizesBuf;
  WGPUBuffer edgesBuf;
  WGPUBuffer edgeColorsBuf;
  WGPUBuffer nodeDegreesBuf;
  WGPUBuffer edgeWeightsBuf;
  WGPUBuffer edgeDashedBuf; /**< 0/1 per edge, edge-buffer order; see
                                 grRendererSetEdgeDashed. */
  size_t edgesBufCapacity; /**< In edges. */
  WGPUBindGroup bindGroup;
  bool bindGroupDirty;
  /** hasNodeColors/hasEdgeColors track whether nodeColorsBuf/edgeColorsBuf
   *  (the buffers actually bound for drawing, i.e. the shader flags in
   *  writeGlobals) currently hold per-element colors -- this is the
   *  *composited* result (client base layer with any active highlight and
   *  accent painted over it), not necessarily a verbatim copy of what the
   *  client last uploaded. See hasClientNodeColors/hasClientEdgeColors below
   *  for the persistent base layer, and applyColorLayers for how the layers
   *  combine. */
  bool hasNodeColors, hasNodeSizes, hasEdgeColors, hasNodeDegrees;
  bool hasEdgeWeights;
  bool hasEdgeDashed; /**< Whether edgeDashedBuf currently holds real data
                           (edge-buffer-order dependent, like hasEdgeColors/
                           hasEdgeWeights -- invalidated on every structural
                           change, see uploadTopology). */
  uint32_t maxNodeDegree; /**< Max value last uploaded via SetNodeDegrees. */
  float meanEdgeWeight;   /**< Mean value last uploaded via SetEdgeWeights. */
  /** CPU mirror of the last grRendererSetNodeSizes upload, indexed by
   *  parent-graph vertex id (only what the GPU buffer holds; not otherwise
   *  readable back from the GPU). Used by vertex-pick hit-testing
   *  (grenderActionPickVertex) so a per-vertex-sized node's actual on-screen
   *  radius is honored instead of falling back to the global node style.
   *  NULL/0 whenever hasNodeSizes is false. */
  float *nodeSizesStaging;
  size_t nodeSizesStagingCount;

  /** CPU copy of the last grRendererSetNodeColors upload, indexed by
   *  parent-graph vertex id -- the persistent "base layer" of client colors
   *  that grRendererSetNodeColors's contract promises survive "until
   *  replaced". Deliberately kept separate from nodeColorsBuf (the GPU
   *  buffer actually bound for drawing): nodeColorsBuf holds this base layer
   *  composited with the active highlight, if any, so that clearing or
   *  changing the highlight can restore the base colors instead of falling
   *  back to the global style. Preserved across capacity growth the same
   *  way nodeSizesStaging is. NULL/0 whenever hasClientNodeColors is false.
   */
  uint32_t *nodeColorsStaging;
  size_t nodeColorsStagingCount;
  bool hasClientNodeColors;

  /** Same idea as nodeColorsStaging, for grRendererSetEdgeColors. Indexed in
   *  edge-buffer order like edgeColorsBuf; invalidated whenever the topology
   *  changes (see uploadTopology) since that reorders/renumbers edges out
   *  from under any previously-uploaded array, exactly like hasEdgeColors
   *  today. */
  uint32_t *edgeColorsStaging;
  size_t edgeColorsStagingCount;
  bool hasClientEdgeColors;

  // highlight styling (subgraph lives on the attached gviz::layout::EmbeddedGraph)
  bool highlightActive;
  uint32_t highlightNodeRgba;
  uint32_t highlightEdgeRgba;
  /** Single-vertex accent painted above base colors and highlight
   *  (grRendererSetAccentVertex). accentVertexId is a parent-graph id, or
   *  -1 when none. Cleared with the highlight and on SetGraph; see the
   *  public doc for full lifetime. */
  int64_t accentVertexId;
  uint32_t accentVertexRgba;
  /** True when nodeColorsBuf/edgeColorsBuf need to be recomputed from the
   *  client base layer + active highlight + accent by applyColorLayers: set
   *  on highlight/accent set/clear, client base-layer changes, and
   *  capacity/topology changes. Checked and cleared once per frame. */
  bool colorsDirty;

  // style
  grColor clearColor;
  grNodeStyle nodeStyle;
  grEdgeStyle edgeStyle;
  bool edgeDegreeAlpha;
  bool edgeWeightWidth;
  /** Shader-side degree-based node sizing (grRendererSetNodeDegreeScale):
   *  when nodeDegreeScale is true and degrees are present, each node's
   *  world radius is nodeStyle.radius * (1 + nodeDegreeScaleFactor *
   *  sqrt(degree)) instead of the plain global default -- still overridden
   *  per-vertex by grRendererSetNodeSizes where set, same precedence as
   *  the plain default. Off by default. */
  bool nodeDegreeScale;
  float nodeDegreeScaleFactor;

  // stats overlay (grStats.cpp): one ImPlot line chart per stat series,
  // drawn fresh from EmbeddedGraph::StatSeries* every frame it's visible --
  // no cached-revision dirty tracking needed anymore (see buildOverlayWindows
  // in grRenderer.cpp for why).
  bool statsVisible;
  bool *statsSeriesVisible; /**< Per-series chart visibility (render only). */
  size_t statsSeriesVisibleCount;
  size_t statsMenuSeriesCount; /**< Last series count synced to the macOS menu. */

  // caption overlay (grCaption.cpp): a single small ImGui window across the
  // bottom of the window, for apps that want to narrate what's currently
  // happening (e.g. a teaching visualization's "current step" text) without
  // building their own on-screen text drawing.
  std::string captionText; /**< Empty draws nothing. Owned/copied here --
                                unlike vertex labels, captions are short and
                                change often, so copying is not worth
                                avoiding. */
  bool captionVisible;

  // vertex-info overlay (click-to-inspect vertex string data; grVertexOverlay.cpp)
  const char *const *vertexLabels; /**< Optional, set via
                                        grRendererSetVertexLabels; not owned,
                                        indexed by parent-graph vertex id. */
  size_t vertexLabelsCount;
  int64_t pickedVertexId; /**< Parent-graph id of the last vertex picked via
                               GR_ACTION_PICK_VERTEX, or -1 if none. */
  bool vertexInfoVisible; /**< Master show/hide for the panel
                               (grRendererShowVertexInfo); true by default.
                               Independent of whether a vertex is picked or
                               labels are set -- when false, nothing is drawn
                               even if both are present. */
  int64_t vertexOverlayShownId; /**< pickedVertexId as of the last
                                      grVertexOverlayBuild call that actually
                                      drew a panel; used only to detect "a
                                      different vertex was just picked" so
                                      the panel's ImGui scroll position can be
                                      reset to the top, mirroring the old
                                      vertexOverlayScrollPx reset. */

  // vertex list overlay (searchable "Vertex N" list, filtered by the active
  // highlight and a fuzzy search over vertex labels; grListOverlay.cpp)
  bool listVisible;
  bool listSearchInputFocused; /**< Whether the search box (ImGui::InputText)
                                    had ImGui keyboard focus as of the most
                                    recent grListOverlayBuild call; read by
                                    processInput (grRenderer.cpp) so Escape
                                    can clear the query while it's focused --
                                    see grListOverlaySelectDelta's doc. */
  char listSearchInput[128];   /**< ImGui::InputText's buffer directly --
                                    ImGui null-terminates and manages the
                                    content in place; no separate length
                                    field needed. */
  std::vector<uint32_t> listFilteredIds;  /**< Parent-graph vertex ids
                                    currently shown, after the highlight
                                    filter and fuzzy search, best match first.
                                    Only rebuilt when listFilterDirty (see
                                    grListOverlayBuild) -- this can be
                                    O(vertex count), so it must not run every
                                    frame regardless of visibility. */
  size_t listSelectedIdx; /**< Index into listFilteredIds of the row selected
                                via Up/Down (see grListOverlaySelectDelta), or
                                SIZE_MAX for no selection. Reset to SIZE_MAX
                                whenever the filter is rebuilt, since a stale
                                index could point at an unrelated vertex once
                                the result set changes. */
  bool listFilterDirty; /**< Set on search text changes, highlight changes,
                             topology/label changes, and the panel's first
                             show; cleared once grListOverlayBuild has re-run
                             the filter. */
  bool listScrollToSelected; /**< Set by grListOverlaySelectDelta; consumed
                                  by the next grListOverlayBuild to call
                                  ImGui::SetScrollHereY on the newly selected
                                  row instead of grender computing the scroll
                                  offset itself. */

  // command console (stateless command line, e.g. "find <id>"; commands by
  // grConsoleRun, input + drawing both via ImGui::InputText in grConsoleBuild
  // -- see grConsole.cpp)
  bool consoleOpen;
  char consoleInput[256];      /**< ImGui::InputText's buffer directly, like
                                     listSearchInput above -- no separate
                                     length field. */
  char consoleMessage[128];    /**< Result/error from the last run command,
                                     empty if none yet. */
  bool consoleMessageIsError;

  // object overlay (rotating .obj mesh preview, independent of the graph)
  grObjOverlay objOverlay;

  // camera + input
  grCamera camera;
  grCameraFrame cameraFrame;
  int orbitKey = -1;        /**< GLFW key held to continuously orbit the
                                  camera, or -1 if unbound. See
                                  grRendererSetOrbitKey. */
  double orbitRadPerSec = 0.0;
  double contentScale;       /**< Framebuffer px per window point. */
  double viewportHeightPx;   /**< Cached each frame (processInput); lets
                                  action handlers convert a node's pixel
                                  radius to world units, mirroring the
                                  vertex shader's pxPerWorld (grShaders.h),
                                  for hit-testing (see grenderActionPickVertex). */
  bool draggingPan, draggingOrbit;
  double dragLastX, dragLastY;
  double scrollAccum;
  bool fitRequested;

  // actions
  std::vector<grKeyBinding> bindings;
  std::vector<grMouseBinding> mouseBindings;
  std::vector<grPendingKey> pendingKeys;
  std::vector<grPendingMouse> pendingMouse;
  bool mouseDown[3];
  bool mouseDragged[3];
  double mousePressX, mousePressY;

  // timing
  double lastFrameTime;
  double deltaTime;
  bool closeRequested;
};

#endif
