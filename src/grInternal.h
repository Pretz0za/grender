#ifndef _GRENDER_INTERNAL_H_
#define _GRENDER_INTERNAL_H_

#include "grender/grender.h"
#include "grProfiling.h"

#include "ds/gvizArray.h"

#include <webgpu/webgpu.h>

// grender never hand-rolls a growable array: gviz already provides gvizArray
// (see ds/gvizArray.h) and every dynamically-sized list in this codebase
// (pending input events, key/mouse bindings, stats primitives, ...) is one.
// Reach for gvizArray before writing another malloc/realloc-doubling loop.

typedef struct GLFWwindow GLFWwindow;

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
 * a structural change. Arrays are owned by the topology.
 */
typedef struct grTopology {
  uint32_t *nodeIds; /**< Instance -> parent-graph vertex id. */
  size_t nodeCount;
  uint32_t *edges;   /**< Flat (u, v) pairs of parent-graph vertex ids. */
  size_t edgeCount;
} grTopology;

/**
 * Extracts visible vertices and edges from @p graph through the gviz public
 * API. Undirected edges are emitted once (u < v); directed edges as stored.
 *
 * @return 0 on success, -1 on allocation failure.
 */
int grTopologyExtract(grTopology *topo, gvizEmbeddedGraph *graph);
void grTopologyRelease(grTopology *topo);

// ------------------------------------------------------------------------------
// Stats overlay (charts for gvizStatSeries recorded by the embedder)
// ------------------------------------------------------------------------------

/** One screen-space overlay primitive. Must match struct StatsPrim in
 *  grShaders.h (32 bytes, vec4f-aligned). */
typedef struct grStatsPrim {
  /** Rect: min corner (xy) and max corner (zw). Line: endpoints a (xy) and
   *  b (zw). Framebuffer pixels, origin top-left. */
  float ab[4];
  uint32_t color; /**< GR_RGBA8 packed. */
  uint32_t kind;  /**< 0 = rect, 1 = anti-aliased line segment. */
  float halfWidth;
  float pad;
} grStatsPrim;

struct grRenderer;

/**
 * Rebuilds the overlay primitive list (r->statsPrims) from the stat series of
 * the attached graph: one mini line chart per non-empty series, stacked in the
 * top-right corner. Only reads the graph through gvizEmbeddedGraphStatSeries*.
 */
void grStatsOverlayBuild(struct grRenderer *r, double fbw, double fbh);

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
 * Live UV mapping tying a 2D gvizEmbeddedGraph's vertex positions to a
 * movable/resizable image rectangle in embedding space, reprojected as
 * texture coordinates for the object overlay's mesh. Owned by grObjOverlay;
 * `graph` itself is a borrowed pointer.
 */
typedef struct grTextureMap {
  bool active;
  bool visible; /**< Whether the image rect also draws in the main scene. */
  gvizEmbeddedGraph *graph; /**< Not owned; must be a 2D embedding. */
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
                       bit2: per-edge color. */
  float nodeFill[4];
  float nodeStroke[4];
  /** x: radius, y: strokeWidth, z: sizeMode (0 px / 1 world), w: proj11. */
  float nodeParams[4];
  /** x: minPixelRadius, y: maxPixelRadius (0 disables each), z/w: unused. */
  float nodeSizeLimits[4];
  float edgeColor[4];
  /** x: width, y: sizeMode, z/w: unused. */
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
  WGPURenderPipeline statsPipeline;
  WGPUTexture depthTexture;
  WGPUTextureView depthView;

  // graph data
  gvizEmbeddedGraph *graph;
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
  size_t edgesBufCapacity; /**< In edges. */
  WGPUBindGroup bindGroup;
  bool bindGroupDirty;
  bool hasNodeColors, hasNodeSizes, hasEdgeColors;
  /** CPU mirror of the last grRendererSetNodeSizes upload, indexed by
   *  parent-graph vertex id (only what the GPU buffer holds; not otherwise
   *  readable back from the GPU). Used by vertex-pick hit-testing
   *  (grenderActionPickVertex) so a per-vertex-sized node's actual on-screen
   *  radius is honored instead of falling back to the global node style.
   *  NULL/0 whenever hasNodeSizes is false. */
  float *nodeSizesStaging;
  size_t nodeSizesStagingCount;

  // highlight styling (subgraph lives on the attached gvizEmbeddedGraph)
  bool highlightActive;
  uint32_t highlightNodeRgba;
  uint32_t highlightEdgeRgba;
  bool highlightDirty;

  // style
  grColor clearColor;
  grNodeStyle nodeStyle;
  grEdgeStyle edgeStyle;

  // stats overlay
  bool statsVisible;
  gvizArray statsPrims; /**< of grStatsPrim; CPU staging list, rebuilt when
                             series revision or layout changes. */
  WGPUBuffer statsBuf;
  size_t statsBufCapacity; /**< In primitives. */
  gvizArray statsSeriesRevisions; /**< of uint64_t; cached
                                       gvizStatSeries.revision per index. */
  bool *statsSeriesVisible; /**< Per-series chart visibility (render only). */
  size_t statsSeriesVisibleCount;
  size_t statsMenuSeriesCount; /**< Last series count synced to the macOS menu. */
  double statsLayoutFbw, statsLayoutFbh, statsLayoutScale;
  bool statsOverlayDirty;

  // object overlay (rotating .obj mesh preview, independent of the graph)
  grObjOverlay objOverlay;

  // camera + input
  grCamera camera;
  grCameraFrame cameraFrame;
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
  gvizArray bindings;      /**< of grKeyBinding. */
  gvizArray mouseBindings; /**< of grMouseBinding. */
  gvizArray pendingKeys;   /**< of grPendingKey. */
  gvizArray pendingMouse;  /**< of grPendingMouse. */
  bool mouseDown[3];
  bool mouseDragged[3];
  double mousePressX, mousePressY;

  // timing
  double lastFrameTime;
  double deltaTime;
  bool closeRequested;
};

#endif
