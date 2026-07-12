#ifndef _GRENDER_INTERNAL_H_
#define _GRENDER_INTERNAL_H_

#include "grender/grender.h"

#include <webgpu/webgpu.h>

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
// Platform
// ------------------------------------------------------------------------------

/** Creates a WGPUSurface for @p window (per-OS implementation). */
WGPUSurface grPlatformCreateSurface(WGPUInstance instance, GLFWwindow *window);

/** macOS: registers the app and installs the menu bar; no-op elsewhere. */
void grPlatformInitApplication(void);

/** Refreshes the Charts submenu from the attached graph's stat series. */
void grPlatformStatsMenuRefresh(struct grRenderer *r);

// ------------------------------------------------------------------------------
// Renderer
// ------------------------------------------------------------------------------

typedef struct grKeyBinding {
  int key;
  const char *actionName;
} grKeyBinding;

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

  // style
  grColor clearColor;
  grNodeStyle nodeStyle;
  grEdgeStyle edgeStyle;

  // stats overlay
  bool statsVisible;
  grStatsPrim *statsPrims; /**< CPU staging list; rebuilt when series revision
                                or layout changes. */
  size_t statsPrimCount, statsPrimCapacity;
  WGPUBuffer statsBuf;
  size_t statsBufCapacity; /**< In primitives. */
  uint64_t *statsSeriesRevisions; /**< Cached gvizStatSeries.revision per index. */
  size_t statsSeriesCacheCount, statsSeriesCacheCapacity;
  bool *statsSeriesVisible; /**< Per-series chart visibility (render only). */
  size_t statsSeriesVisibleCount;
  size_t statsMenuSeriesCount; /**< Last series count synced to the macOS menu. */
  double statsLayoutFbw, statsLayoutFbh, statsLayoutScale;
  bool statsOverlayDirty;

  // camera + input
  grCamera camera;
  grCameraFrame cameraFrame;
  double contentScale;       /**< Framebuffer px per window point. */
  bool draggingPan, draggingOrbit;
  double dragLastX, dragLastY;
  double scrollAccum;
  bool fitRequested;

  // actions
  grKeyBinding *bindings;
  size_t bindingCount, bindingCapacity;
  struct {
    int key;
    int mods;
  } *pendingKeys;
  size_t pendingKeyCount, pendingKeyCapacity;

  // timing
  double lastFrameTime;
  double deltaTime;
  bool closeRequested;
};

#endif
