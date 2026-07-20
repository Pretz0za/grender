#include "grInternal.h"
#include "grShaders.h"

#include "ds/gvizArray.h"
#include "ds/gvizGraph.h"
#include "ds/gvizSubgraph.h"
#include "embedders/gvizPlanarEmbedder.h"

#include <webgpu/wgpu.h> // wgpu-native extensions (wgpuDevicePoll)

#include <GLFW/glfw3.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GR_LOG(...) fprintf(stderr, "[grender] " __VA_ARGS__)

static void grenderActionPickFace(gvizEmbeddedGraph *eg, void *userData,
                                  const gvizActionPayload *payload);
static void grenderActionPickVertex(gvizEmbeddedGraph *eg, void *userData,
                                    const gvizActionPayload *payload);

// ------------------------------------------------------------------------------
// Defaults
// ------------------------------------------------------------------------------

void grRendererDescInit(grRendererDesc *desc) {
  memset(desc, 0, sizeof(*desc));
  desc->title = "grender";
  desc->width = 1280;
  desc->height = 800;
  desc->clearColor = GR_COLOR(0.07f, 0.07f, 0.09f, 1.0f);
  desc->nodeStyle = (grNodeStyle){
      .fillColor = GR_COLOR(0.95f, 0.95f, 0.98f, 1.0f),
      .strokeColor = GR_COLOR(0.10f, 0.10f, 0.12f, 1.0f),
      .radius = 3.0f,
      .strokeWidth = 0.0f,
      .sizeMode = GR_SIZE_PIXELS,
      .minPixelRadius = 0.0f,
      .maxPixelRadius = 0.0f,
  };
  desc->edgeStyle = (grEdgeStyle){
      .color = GR_COLOR(0.45f, 0.55f, 0.75f, 0.55f),
      .width = 1.0f,
      .sizeMode = GR_SIZE_PIXELS,
  };
  desc->vsync = true;
}

// ------------------------------------------------------------------------------
// WebGPU setup helpers
// ------------------------------------------------------------------------------

static void onAdapterRequest(WGPURequestAdapterStatus status,
                             WGPUAdapter adapter, WGPUStringView message,
                             void *userdata1, void *userdata2) {
  (void)userdata2;
  if (status == WGPURequestAdapterStatus_Success)
    *(WGPUAdapter *)userdata1 = adapter;
  else
    GR_LOG("adapter request failed: %.*s\n", (int)message.length, message.data);
}

static void onDeviceRequest(WGPURequestDeviceStatus status, WGPUDevice device,
                            WGPUStringView message, void *userdata1,
                            void *userdata2) {
  (void)userdata2;
  if (status == WGPURequestDeviceStatus_Success)
    *(WGPUDevice *)userdata1 = device;
  else
    GR_LOG("device request failed: %.*s\n", (int)message.length, message.data);
}

static void onUncapturedError(WGPUDevice const *device, WGPUErrorType type,
                              WGPUStringView message, void *userdata1,
                              void *userdata2) {
  (void)device, (void)userdata1, (void)userdata2;
  GR_LOG("GPU error (type %d): %.*s\n", (int)type, (int)message.length,
         message.data);
}

static WGPUBuffer createBuffer(grRenderer *r, size_t size, WGPUBufferUsage usage,
                               const char *label) {
  if (size < 4)
    size = 4;
  size = (size + 3) & ~(size_t)3;
  return wgpuDeviceCreateBuffer(r->device,
                                &(const WGPUBufferDescriptor){
                                    .label = {label, WGPU_STRLEN},
                                    .size = size,
                                    .usage = usage,
                                });
}

static int checkStorageBinding(grRenderer *r, const char *what, size_t bytes) {
  if (bytes > r->maxBufferSize) {
    GR_LOG("%s needs %zu bytes but this GPU max buffer size is %llu bytes\n",
           what, bytes, (unsigned long long)r->maxBufferSize);
    return -1;
  }
  if (bytes <= r->maxStorageBufferBindingSize)
    return 0;
  GR_LOG(
      "%s needs %zu bytes but this GPU allows %llu bytes per storage binding "
      "(WebGPU default is 128 MiB). Use a smaller graph or fewer visible "
      "edges.\n",
      what, bytes, (unsigned long long)r->maxStorageBufferBindingSize);
  return -1;
}

static uint64_t storageBindBytes(grRenderer *r, WGPUBuffer buf) {
  if (!buf)
    return 4;
  uint64_t sz = wgpuBufferGetSize(buf);
  if (sz > r->maxStorageBufferBindingSize) {
    GR_LOG("internal: buffer size %llu exceeds binding limit %llu\n",
           (unsigned long long)sz,
           (unsigned long long)r->maxStorageBufferBindingSize);
  }
  return sz;
}

// ------------------------------------------------------------------------------
// Pipelines
// ------------------------------------------------------------------------------

static int createPipelines(grRenderer *r) {
  r->shaderModule = wgpuDeviceCreateShaderModule(
      r->device,
      &(const WGPUShaderModuleDescriptor){
          .label = {"grender shaders", WGPU_STRLEN},
          .nextInChain =
              (WGPUChainedStruct *)&(WGPUShaderSourceWGSL){
                  .chain = {.sType = WGPUSType_ShaderSourceWGSL},
                  .code = {GR_WGSL_SOURCE, WGPU_STRLEN},
              },
      });
  if (!r->shaderModule)
    return -1;

  WGPUBindGroupLayoutEntry entries[8] = {0};
  entries[0] = (WGPUBindGroupLayoutEntry){
      .binding = 0,
      .visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment,
      .buffer = {.type = WGPUBufferBindingType_Uniform},
  };
  for (int i = 1; i < 8; i++) {
    entries[i] = (WGPUBindGroupLayoutEntry){
        .binding = (uint32_t)i,
        .visibility = WGPUShaderStage_Vertex,
        .buffer = {.type = WGPUBufferBindingType_ReadOnlyStorage},
    };
  }

  r->bindGroupLayout = wgpuDeviceCreateBindGroupLayout(
      r->device, &(const WGPUBindGroupLayoutDescriptor){
                     .label = {"grender bgl", WGPU_STRLEN},
                     .entryCount = 8,
                     .entries = entries,
                 });
  r->pipelineLayout = wgpuDeviceCreatePipelineLayout(
      r->device, &(const WGPUPipelineLayoutDescriptor){
                     .label = {"grender layout", WGPU_STRLEN},
                     .bindGroupLayoutCount = 1,
                     .bindGroupLayouts =
                         (const WGPUBindGroupLayout[]){r->bindGroupLayout},
                 });
  if (!r->bindGroupLayout || !r->pipelineLayout)
    return -1;

  const WGPUBlendState blend = {
      .color = {.operation = WGPUBlendOperation_Add,
                .srcFactor = WGPUBlendFactor_SrcAlpha,
                .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
      .alpha = {.operation = WGPUBlendOperation_Add,
                .srcFactor = WGPUBlendFactor_One,
                .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha},
  };
  const WGPUColorTargetState colorTarget = {
      .format = r->surfaceFormat,
      .blend = &blend,
      .writeMask = WGPUColorWriteMask_All,
  };

  const char *labels[3] = {"grender nodes", "grender edges", "grender stats"};
  const char *vsEntries[3] = {"vsNode", "vsEdge", "vsStats"};
  const char *fsEntries[3] = {"fsNode", "fsEdge", "fsStats"};
  WGPURenderPipeline pipelines[3] = {0};

  for (int i = 0; i < 3; i++) {
    const WGPUDepthStencilState depthState = {
        .format = WGPUTextureFormat_Depth24Plus,
        // Nodes write depth so edges/nodes behind them are occluded in 3D;
        // edges only test. The stats overlay ignores scene depth entirely.
        .depthWriteEnabled =
            (i == 0) ? WGPUOptionalBool_True : WGPUOptionalBool_False,
        .depthCompare =
            (i == 2) ? WGPUCompareFunction_Always : WGPUCompareFunction_LessEqual,
        .stencilFront = {.compare = WGPUCompareFunction_Always},
        .stencilBack = {.compare = WGPUCompareFunction_Always},
        .stencilReadMask = 0xFFFFFFFF,
        .stencilWriteMask = 0xFFFFFFFF,
    };

    pipelines[i] = wgpuDeviceCreateRenderPipeline(
        r->device,
        &(const WGPURenderPipelineDescriptor){
            .label = {labels[i], WGPU_STRLEN},
            .layout = r->pipelineLayout,
            .vertex = {.module = r->shaderModule,
                       .entryPoint = {vsEntries[i], WGPU_STRLEN}},
            .fragment =
                &(const WGPUFragmentState){
                    .module = r->shaderModule,
                    .entryPoint = {fsEntries[i], WGPU_STRLEN},
                    .targetCount = 1,
                    .targets = &colorTarget,
                },
            .primitive = {.topology = WGPUPrimitiveTopology_TriangleList,
                          .cullMode = WGPUCullMode_None},
            .depthStencil = &depthState,
            .multisample = {.count = 1, .mask = 0xFFFFFFFF},
        });
    if (!pipelines[i])
      return -1;
  }

  r->nodePipeline = pipelines[0];
  r->edgePipeline = pipelines[1];
  r->statsPipeline = pipelines[2];
  return 0;
}

static void recreateDepthTexture(grRenderer *r) {
  if (r->depthView)
    wgpuTextureViewRelease(r->depthView);
  if (r->depthTexture) {
    wgpuTextureDestroy(r->depthTexture);
    wgpuTextureRelease(r->depthTexture);
  }
  r->depthTexture = wgpuDeviceCreateTexture(
      r->device, &(const WGPUTextureDescriptor){
                     .label = {"grender depth", WGPU_STRLEN},
                     .usage = WGPUTextureUsage_RenderAttachment,
                     .dimension = WGPUTextureDimension_2D,
                     .size = {r->surfaceConfig.width, r->surfaceConfig.height, 1},
                     .format = WGPUTextureFormat_Depth24Plus,
                     .mipLevelCount = 1,
                     .sampleCount = 1,
                 });
  r->depthView = wgpuTextureCreateView(r->depthTexture, NULL);
}

// ------------------------------------------------------------------------------
// GLFW callbacks
// ------------------------------------------------------------------------------

static void onFramebufferSize(GLFWwindow *window, int width, int height) {
  (void)width, (void)height;
  grRenderer *r = glfwGetWindowUserPointer(window);
  if (r)
    r->surfaceDirty = true;
}

static void onScroll(GLFWwindow *window, double dx, double dy) {
  (void)dx;
  grRenderer *r = glfwGetWindowUserPointer(window);
  if (r)
    r->scrollAccum += dy;
}

static void onKey(GLFWwindow *window, int key, int scancode, int action,
                  int mods) {
  (void)scancode;
  grRenderer *r = glfwGetWindowUserPointer(window);
  if (!r || (action != GLFW_PRESS && action != GLFW_REPEAT))
    return;

  grPendingKey pk = {key, mods};
  gvizArrayPush(&r->pendingKeys, &pk);
}

static void onMouseButton(GLFWwindow *window, int button, int action, int mods) {
  grRenderer *r = glfwGetWindowUserPointer(window);
  if (!r || button < 0 || button > 2)
    return;

  if (action == GLFW_PRESS) {
    r->mouseDown[button] = true;
    r->mouseDragged[button] = false;
    glfwGetCursorPos(window, &r->mousePressX, &r->mousePressY);
    (void)mods;
    return;
  }

  if (action != GLFW_RELEASE)
    return;

  bool wasDown = r->mouseDown[button];
  r->mouseDown[button] = false;
  if (!wasDown || r->mouseDragged[button])
    return;

  double cx, cy;
  glfwGetCursorPos(window, &cx, &cy);

  grPendingMouse pm = {button, mods, cx * r->contentScale, cy * r->contentScale};
  gvizArrayPush(&r->pendingMouse, &pm);
}

// ------------------------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------------------------

grRenderer *grRendererCreate(const grRendererDesc *descIn) {
  grRendererDesc defaults;
  if (!descIn) {
    grRendererDescInit(&defaults);
    descIn = &defaults;
  }

  grRenderer *r = calloc(1, sizeof(grRenderer));
  if (!r)
    return NULL;
  r->clearColor = descIn->clearColor;
  r->nodeStyle = descIn->nodeStyle;
  r->edgeStyle = descIn->edgeStyle;
  r->statsVisible = true;
  gvizArrayInit(&r->statsPrims, sizeof(grStatsPrim));
  gvizArrayInit(&r->statsSeriesRevisions, sizeof(uint64_t));
  gvizArrayInit(&r->bindings, sizeof(grKeyBinding));
  gvizArrayInit(&r->mouseBindings, sizeof(grMouseBinding));
  gvizArrayInit(&r->pendingKeys, sizeof(grPendingKey));
  gvizArrayInit(&r->pendingMouse, sizeof(grPendingMouse));
  grCameraInit2D(&r->camera);

#ifdef __APPLE__
  glfwInitHint(GLFW_COCOA_MENUBAR, GLFW_FALSE);
#endif
  if (!glfwInit()) {
    GR_LOG("glfwInit failed\n");
    free(r);
    return NULL;
  }

  grPlatformInitApplication();

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  r->window = glfwCreateWindow((int)descIn->width, (int)descIn->height,
                               descIn->title ? descIn->title : "grender", NULL,
                               NULL);
  if (!r->window)
    goto fail;

  glfwSetWindowUserPointer(r->window, r);
  glfwSetFramebufferSizeCallback(r->window, onFramebufferSize);
  glfwSetScrollCallback(r->window, onScroll);
  glfwSetKeyCallback(r->window, onKey);
  glfwSetMouseButtonCallback(r->window, onMouseButton);

  r->instance = wgpuCreateInstance(NULL);
  if (!r->instance)
    goto fail;

  r->surface = grPlatformCreateSurface(r->instance, r->window);
  if (!r->surface)
    goto fail;

  // wgpu-native services these callbacks synchronously.
  wgpuInstanceRequestAdapter(
      r->instance,
      &(const WGPURequestAdapterOptions){.compatibleSurface = r->surface},
      (const WGPURequestAdapterCallbackInfo){.callback = onAdapterRequest,
                                             .userdata1 = &r->adapter});
  if (!r->adapter)
    goto fail;

  WGPULimits adapterLimits = WGPU_LIMITS_INIT;
  if (wgpuAdapterGetLimits(r->adapter, &adapterLimits) != WGPUStatus_Success) {
    GR_LOG("failed to query adapter limits\n");
    goto fail;
  }

  WGPULimits requiredLimits = WGPU_LIMITS_INIT;
  requiredLimits.maxStorageBufferBindingSize =
      adapterLimits.maxStorageBufferBindingSize;
  requiredLimits.maxBufferSize = adapterLimits.maxBufferSize;

  wgpuAdapterRequestDevice(
      r->adapter,
      &(const WGPUDeviceDescriptor){
          .label = {"grender device", WGPU_STRLEN},
          .requiredLimits = &requiredLimits,
          .uncapturedErrorCallbackInfo = {.callback = onUncapturedError},
      },
      (const WGPURequestDeviceCallbackInfo){.callback = onDeviceRequest,
                                            .userdata1 = &r->device});
  if (!r->device)
    goto fail;

  {
    WGPULimits deviceLimits = WGPU_LIMITS_INIT;
    if (wgpuDeviceGetLimits(r->device, &deviceLimits) == WGPUStatus_Success) {
      r->maxStorageBufferBindingSize = deviceLimits.maxStorageBufferBindingSize;
      r->maxBufferSize = deviceLimits.maxBufferSize;
      GR_LOG("GPU storage binding limit: %.0f MiB, max buffer: %.0f MiB\n",
             deviceLimits.maxStorageBufferBindingSize / (1024.0 * 1024.0),
             deviceLimits.maxBufferSize / (1024.0 * 1024.0));
    } else {
      r->maxStorageBufferBindingSize = 128u * 1024u * 1024u;
      r->maxBufferSize = 256u * 1024u * 1024u;
    }
  }

  r->queue = wgpuDeviceGetQueue(r->device);

  WGPUSurfaceCapabilities caps = {0};
  wgpuSurfaceGetCapabilities(r->surface, r->adapter, &caps);
  r->surfaceFormat = caps.formats[0];

  WGPUPresentMode presentMode = WGPUPresentMode_Fifo;
  if (!descIn->vsync) {
    for (size_t i = 0; i < caps.presentModeCount; i++)
      if (caps.presentModes[i] == WGPUPresentMode_Immediate)
        presentMode = WGPUPresentMode_Immediate;
  }

  int fbw, fbh;
  glfwGetFramebufferSize(r->window, &fbw, &fbh);
  r->surfaceConfig = (WGPUSurfaceConfiguration){
      .device = r->device,
      .usage = WGPUTextureUsage_RenderAttachment,
      .format = r->surfaceFormat,
      .width = (uint32_t)fbw,
      .height = (uint32_t)fbh,
      .presentMode = presentMode,
      .alphaMode = caps.alphaModes[0],
  };
  wgpuSurfaceCapabilitiesFreeMembers(caps);
  wgpuSurfaceConfigure(r->surface, &r->surfaceConfig);
  recreateDepthTexture(r);

  if (createPipelines(r) < 0) {
    GR_LOG("pipeline creation failed\n");
    goto fail;
  }

  r->globalsBuf = createBuffer(r, sizeof(grGlobalsUBO),
                               WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                               "grender globals");

  glfwGetCursorPos(r->window, &r->dragLastX, &r->dragLastY);
  r->lastFrameTime = glfwGetTime();
  return r;

fail:
  grRendererDestroy(r);
  return NULL;
}

#define GR_RELEASE(fn, x)                                                      \
  do {                                                                         \
    if (x) {                                                                   \
      fn(x);                                                                   \
      (x) = NULL;                                                              \
    }                                                                          \
  } while (0)

void grRendererDestroy(grRenderer *r) {
  if (!r)
    return;

  GR_RELEASE(wgpuBufferRelease, r->globalsBuf);
  GR_RELEASE(wgpuBufferRelease, r->positionsBuf);
  GR_RELEASE(wgpuBufferRelease, r->nodeIdsBuf);
  GR_RELEASE(wgpuBufferRelease, r->nodeColorsBuf);
  GR_RELEASE(wgpuBufferRelease, r->nodeSizesBuf);
  GR_RELEASE(wgpuBufferRelease, r->edgesBuf);
  GR_RELEASE(wgpuBufferRelease, r->edgeColorsBuf);
  GR_RELEASE(wgpuBufferRelease, r->statsBuf);
  GR_RELEASE(wgpuBindGroupRelease, r->bindGroup);
  grObjOverlayRelease(r);
  GR_RELEASE(wgpuRenderPipelineRelease, r->nodePipeline);
  GR_RELEASE(wgpuRenderPipelineRelease, r->edgePipeline);
  GR_RELEASE(wgpuRenderPipelineRelease, r->statsPipeline);
  GR_RELEASE(wgpuPipelineLayoutRelease, r->pipelineLayout);
  GR_RELEASE(wgpuBindGroupLayoutRelease, r->bindGroupLayout);
  GR_RELEASE(wgpuShaderModuleRelease, r->shaderModule);
  GR_RELEASE(wgpuTextureViewRelease, r->depthView);
  GR_RELEASE(wgpuTextureRelease, r->depthTexture);
  GR_RELEASE(wgpuQueueRelease, r->queue);
  GR_RELEASE(wgpuDeviceRelease, r->device);
  GR_RELEASE(wgpuAdapterRelease, r->adapter);
  GR_RELEASE(wgpuSurfaceRelease, r->surface);
  GR_RELEASE(wgpuInstanceRelease, r->instance);
  GR_RELEASE(glfwDestroyWindow, r->window);
  glfwTerminate();

  grTopologyRelease(&r->topo);
  free(r->posStaging);
  free(r->nodeSizesStaging);
  gvizArrayRelease(&r->statsPrims);
  gvizArrayRelease(&r->statsSeriesRevisions);
  free(r->statsSeriesVisible);
  gvizArrayRelease(&r->bindings);
  gvizArrayRelease(&r->mouseBindings);
  gvizArrayRelease(&r->pendingKeys);
  gvizArrayRelease(&r->pendingMouse);
  free(r);
}

// ------------------------------------------------------------------------------
// Stats overlay visibility
// ------------------------------------------------------------------------------

static void statsVisibilitySync(grRenderer *r) {
  free(r->statsSeriesVisible);
  r->statsSeriesVisible = NULL;
  r->statsSeriesVisibleCount = 0;

  if (!r->graph)
    return;

  size_t n = gvizEmbeddedGraphStatSeriesCount(r->graph);
  if (n == 0)
    return;

  r->statsSeriesVisible = calloc(n, sizeof(bool));
  if (!r->statsSeriesVisible)
    return;
  for (size_t i = 0; i < n; i++)
    r->statsSeriesVisible[i] = true;
  r->statsSeriesVisibleCount = n;
}

static void statsMenuSyncIfNeeded(grRenderer *r) {
  if (!r->graph)
    return;
  size_t n = gvizEmbeddedGraphStatSeriesCount(r->graph);
  if (n == r->statsMenuSeriesCount)
    return;
  if (n != r->statsSeriesVisibleCount)
    statsVisibilitySync(r);
  r->statsMenuSeriesCount = n;
  grPlatformStatsMenuRefresh(r);
}

size_t grRendererStatSeriesCount(const grRenderer *r) {
  if (!r || !r->graph)
    return 0;
  return gvizEmbeddedGraphStatSeriesCount(r->graph);
}

const char *grRendererStatSeriesName(const grRenderer *r, size_t idx) {
  if (!r || !r->graph)
    return NULL;
  const gvizStatSeries *series = gvizEmbeddedGraphStatSeriesAt(r->graph, idx);
  return series ? series->name : NULL;
}

bool grRendererStatSeriesShown(const grRenderer *r, size_t idx) {
  if (!r || idx >= r->statsSeriesVisibleCount)
    return false;
  return r->statsSeriesVisible[idx];
}

void grRendererShowStatSeries(grRenderer *r, size_t idx, bool show) {
  if (!r || idx >= r->statsSeriesVisibleCount ||
      r->statsSeriesVisible[idx] == show)
    return;
  r->statsSeriesVisible[idx] = show;
  r->statsOverlayDirty = true;
  grPlatformStatsMenuRefresh(r);
}

void grRendererShowStats(grRenderer *r, bool show) {
  if (r->statsVisible == show)
    return;
  r->statsVisible = show;
  if (show)
    r->statsOverlayDirty = true;
  else
    r->statsPrims.count = 0;
  grPlatformStatsMenuRefresh(r);
}

bool grRendererStatsShown(const grRenderer *r) { return r->statsVisible; }

// ------------------------------------------------------------------------------
// Graph attachment and GPU buffer management
// ------------------------------------------------------------------------------

/** (Re)creates position-indexed buffers when the vertex capacity changes. */
static int ensurePositionBuffers(grRenderer *r) {
  size_t count = gvizEmbeddedGraphPositionCount(r->graph);
  size_t srcDim = gvizEmbeddedGraphDim(r->graph);
  size_t renderDim = srcDim == 4 ? 3 : srcDim;
  if (count == r->posCapacity && srcDim == r->srcDim && renderDim == r->posDim &&
      r->positionsBuf)
    return 0;

  free(r->posStaging);
  r->posStaging = malloc(sizeof(float) * count * renderDim);
  if (!r->posStaging)
    return -1;

  GR_RELEASE(wgpuBufferRelease, r->positionsBuf);
  size_t posBytes = sizeof(float) * count * renderDim;
  if (checkStorageBinding(r, "positions", posBytes) < 0)
    return -1;
  r->positionsBuf = createBuffer(
      r, posBytes,
      WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst, "grender positions");
  if (!r->positionsBuf)
    return -1;

  r->posCapacity = count;
  r->srcDim = srcDim;
  r->posDim = renderDim;
  r->bindGroupDirty = true;

  // Per-vertex attributes are indexed the same way; stale ones are dropped.
  if (r->hasNodeColors || r->hasNodeSizes) {
    r->hasNodeColors = false;
    r->hasNodeSizes = false;
  }
  return 0;
}

/** Uploads topology-derived buffers (node id remap + edge endpoint pairs). */
static int uploadTopology(grRenderer *r) {
  if (grTopologyExtract(&r->topo, r->graph) < 0)
    return -1;

  size_t nodeBytes = sizeof(uint32_t) * (r->topo.nodeCount ? r->topo.nodeCount : 1);
  size_t edgeBytes =
      sizeof(uint32_t) * 2 * (r->topo.edgeCount ? r->topo.edgeCount : 1);

  if (checkStorageBinding(r, "node ids", nodeBytes) < 0 ||
      checkStorageBinding(r, "edges", edgeBytes) < 0)
    return -1;

  // Node-id and edge buffers are recreated on structural change only.
  GR_RELEASE(wgpuBufferRelease, r->nodeIdsBuf);
  r->nodeIdsBuf =
      createBuffer(r, nodeBytes,
                   WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                   "grender node ids");
  GR_RELEASE(wgpuBufferRelease, r->edgesBuf);
  r->edgesBuf = createBuffer(r, edgeBytes,
                             WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                             "grender edges");
  if (!r->nodeIdsBuf || !r->edgesBuf)
    return -1;

  if (r->topo.nodeCount)
    wgpuQueueWriteBuffer(r->queue, r->nodeIdsBuf, 0, r->topo.nodeIds,
                         sizeof(uint32_t) * r->topo.nodeCount);
  if (r->topo.edgeCount)
    wgpuQueueWriteBuffer(r->queue, r->edgesBuf, 0, r->topo.edges,
                         sizeof(uint32_t) * 2 * r->topo.edgeCount);

  // Stale per-edge colors no longer match the edge ordering.
  r->hasEdgeColors = false;
  r->highlightDirty = true;
  r->bindGroupDirty = true;
  return 0;
}

static int rebuildBindGroup(grRenderer *r) {
  GR_RELEASE(wgpuBindGroupRelease, r->bindGroup);

  // Optional attribute buffers get 4-byte placeholders so the bind group is
  // always complete; shaders never read them unless the matching flag is set.
  if (!r->nodeColorsBuf)
    r->nodeColorsBuf = createBuffer(r, 4, WGPUBufferUsage_Storage |
                                              WGPUBufferUsage_CopyDst,
                                    "grender node colors");
  if (!r->nodeSizesBuf)
    r->nodeSizesBuf = createBuffer(r, 4, WGPUBufferUsage_Storage |
                                             WGPUBufferUsage_CopyDst,
                                   "grender node sizes");
  if (!r->edgeColorsBuf)
    r->edgeColorsBuf = createBuffer(r, 4, WGPUBufferUsage_Storage |
                                              WGPUBufferUsage_CopyDst,
                                    "grender edge colors");
  if (!r->statsBuf)
    r->statsBuf = createBuffer(r, sizeof(grStatsPrim),
                               WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                               "grender stats prims");

  const WGPUBindGroupEntry entries[8] = {
      {.binding = 0, .buffer = r->globalsBuf,
       .size = storageBindBytes(r, r->globalsBuf)},
      {.binding = 1, .buffer = r->positionsBuf,
       .size = storageBindBytes(r, r->positionsBuf)},
      {.binding = 2, .buffer = r->nodeIdsBuf,
       .size = storageBindBytes(r, r->nodeIdsBuf)},
      {.binding = 3, .buffer = r->nodeColorsBuf,
       .size = storageBindBytes(r, r->nodeColorsBuf)},
      {.binding = 4, .buffer = r->nodeSizesBuf,
       .size = storageBindBytes(r, r->nodeSizesBuf)},
      {.binding = 5, .buffer = r->edgesBuf,
       .size = storageBindBytes(r, r->edgesBuf)},
      {.binding = 6, .buffer = r->edgeColorsBuf,
       .size = storageBindBytes(r, r->edgeColorsBuf)},
      {.binding = 7, .buffer = r->statsBuf,
       .size = storageBindBytes(r, r->statsBuf)},
  };
  for (size_t i = 0; i < 8; i++) {
    if (entries[i].size > r->maxStorageBufferBindingSize) {
      GR_LOG("bind group entry %zu size %llu exceeds storage binding limit\n",
             i, (unsigned long long)entries[i].size);
      return -1;
    }
  }
  r->bindGroup = wgpuDeviceCreateBindGroup(
      r->device, &(const WGPUBindGroupDescriptor){
                     .label = {"grender bind group", WGPU_STRLEN},
                     .layout = r->bindGroupLayout,
                     .entryCount = 8,
                     .entries = entries,
                 });
  r->bindGroupDirty = false;
  return r->bindGroup ? 0 : -1;
}

int grRendererSetGraph(grRenderer *r, gvizEmbeddedGraph *graph) {
  size_t dim = gvizEmbeddedGraphDim(graph);
  if (dim != 2 && dim != 3 && dim != 4) {
    GR_LOG("unsupported embedding dimension %zu (only 2, 3, and 4)\n", dim);
    return -1;
  }

  r->graph = graph;
  gvizEmbeddedGraphAddAction(graph, GR_ACTION_PICK_FACE, grenderActionPickFace,
                             r);
  gvizEmbeddedGraphAddAction(graph, GR_ACTION_PICK_VERTEX,
                             grenderActionPickVertex, r);
  r->highlightActive = false;
  r->highlightDirty = false;
  if (dim == 3 || dim == 4)
    grCameraInit3D(&r->camera);
  else
    grCameraInit2D(&r->camera);

  if (ensurePositionBuffers(r) < 0 || uploadTopology(r) < 0)
    return -1;
  r->topoDirty = false;
  r->drawMaskRevision = gvizEmbeddedGraphDrawMaskRevision(graph);
  r->statsSeriesRevisions.count = 0;
  r->statsOverlayDirty = true;
  r->statsPrims.count = 0;
  r->pcaBasisValid = false;
  statsVisibilitySync(r);
  r->statsMenuSeriesCount = gvizEmbeddedGraphStatSeriesCount(graph);
  grPlatformStatsMenuRefresh(r);
  r->fitRequested = true;
  return 0;
}

void grRendererGraphStructureChanged(grRenderer *r) { r->topoDirty = true; }

// ------------------------------------------------------------------------------
// Styling
// ------------------------------------------------------------------------------

void grRendererSetNodeStyle(grRenderer *r, const grNodeStyle *style) {
  r->nodeStyle = *style;
}

void grRendererSetEdgeStyle(grRenderer *r, const grEdgeStyle *style) {
  r->edgeStyle = *style;
}

static int uploadAttribute(grRenderer *r, WGPUBuffer *buf, const void *data,
                           size_t bytes, bool *flag, const char *label) {
  if (!data) {
    *flag = false;
    return 0;
  }

  // Grow-only: recreate when the existing buffer is too small.
  if (*buf && wgpuBufferGetSize(*buf) < bytes) {
    GR_RELEASE(wgpuBufferRelease, *buf);
    r->bindGroupDirty = true;
  }
  if (!*buf) {
    *buf = createBuffer(r, bytes,
                        WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                        label);
    r->bindGroupDirty = true;
    if (!*buf)
      return -1;
  }
  wgpuQueueWriteBuffer(r->queue, *buf, 0, data, bytes);
  *flag = true;
  return 0;
}

int grRendererSetNodeColors(grRenderer *r, const uint32_t *rgba8,
                            size_t count) {
  if (rgba8 && (!r->graph || count != r->posCapacity))
    return -1;
  return uploadAttribute(r, &r->nodeColorsBuf, rgba8, count * sizeof(uint32_t),
                         &r->hasNodeColors, "grender node colors");
}

int grRendererSetNodeSizes(grRenderer *r, const float *radii, size_t count) {
  if (radii && (!r->graph || count != r->posCapacity))
    return -1;

  int res = uploadAttribute(r, &r->nodeSizesBuf, radii, count * sizeof(float),
                            &r->hasNodeSizes, "grender node sizes");
  if (res < 0)
    return res;

  if (!r->hasNodeSizes) {
    free(r->nodeSizesStaging);
    r->nodeSizesStaging = NULL;
    r->nodeSizesStagingCount = 0;
    return 0;
  }

  float *staging = realloc(r->nodeSizesStaging, count * sizeof(float));
  if (!staging) {
    // GPU upload already succeeded; hit-testing just falls back to the
    // global node style radius until the next successful call.
    free(r->nodeSizesStaging);
    r->nodeSizesStaging = NULL;
    r->nodeSizesStagingCount = 0;
    return 0;
  }
  memcpy(staging, radii, count * sizeof(float));
  r->nodeSizesStaging = staging;
  r->nodeSizesStagingCount = count;
  return 0;
}

int grRendererSetEdgeColors(grRenderer *r, const uint32_t *rgba8,
                            size_t count) {
  if (rgba8 && (!r->graph || count != r->topo.edgeCount))
    return -1;
  return uploadAttribute(r, &r->edgeColorsBuf, rgba8, count * sizeof(uint32_t),
                         &r->hasEdgeColors, "grender edge colors");
}

size_t grRendererEdgeCount(const grRenderer *r) { return r->topo.edgeCount; }

size_t grRendererGetEdges(const grRenderer *r, uint32_t *out) {
  memcpy(out, r->topo.edges, sizeof(uint32_t) * 2 * r->topo.edgeCount);
  return r->topo.edgeCount;
}

// ------------------------------------------------------------------------------
// Input and actions
// ------------------------------------------------------------------------------

int grRendererBindKey(grRenderer *r, int key, const char *actionName) {
  grKeyBinding *bindings = r->bindings.arr;
  for (size_t i = 0; i < r->bindings.count; i++) {
    if (bindings[i].key == key) {
      bindings[i].actionName = actionName;
      return 0;
    }
  }
  grKeyBinding kb = {key, actionName};
  return gvizArrayPush(&r->bindings, &kb);
}

void grRendererUnbindKey(grRenderer *r, int key) {
  grKeyBinding *bindings = r->bindings.arr;
  for (size_t i = 0; i < r->bindings.count; i++) {
    if (bindings[i].key == key) {
      gvizArraySwapDelete(&r->bindings, i);
      return;
    }
  }
}

int grRendererBindMouse(grRenderer *r, int button, const char *actionName) {
  grMouseBinding *mouseBindings = r->mouseBindings.arr;
  for (size_t i = 0; i < r->mouseBindings.count; i++) {
    if (mouseBindings[i].button == button) {
      mouseBindings[i].actionName = actionName;
      return 0;
    }
  }
  grMouseBinding mb = {button, actionName};
  return gvizArrayPush(&r->mouseBindings, &mb);
}

void grRendererUnbindMouse(grRenderer *r, int button) {
  grMouseBinding *mouseBindings = r->mouseBindings.arr;
  for (size_t i = 0; i < r->mouseBindings.count; i++) {
    if (mouseBindings[i].button == button) {
      gvizArraySwapDelete(&r->mouseBindings, i);
      return;
    }
  }
}

static uint32_t colorToRgba8(const grColor *c) {
  return GR_RGBA8((uint32_t)(c->r * 255.0f + 0.5f),
                (uint32_t)(c->g * 255.0f + 0.5f),
                (uint32_t)(c->b * 255.0f + 0.5f),
                (uint32_t)(c->a * 255.0f + 0.5f));
}

static void grHighlightReset(grRenderer *r) {
  r->highlightActive = false;
  r->highlightDirty = false;
}

static gvizSubgraph grHighlightCopySubgraph(const gvizSubgraph *src) {
  gvizSubgraph dst = {0};
  if (!src || !src->g)
    return dst;

  dst = gvizSubgraphCreateEmpty(src->g);
  if (!dst.g)
    return dst;

  gvizSubgraphVertexIterator vit = gvizSubgraphVertexIteratorCreate(src);
  size_t u;
  while (gvizSubgraphVertexIterate(&vit, &u))
    gvizSubgraphShowVertex(&dst, u);

  vit = gvizSubgraphVertexIteratorCreate(src);
  while (gvizSubgraphVertexIterate(&vit, &u)) {
    gvizSubgraphNeighborIterator nit =
        gvizSubgraphNeighborIteratorCreate(src, u);
    size_t v;
    while (gvizSubgraphNeighborIterate(&nit, &v)) {
      if (gvizSubgraphHasEdge(src, u, v))
        gvizSubgraphShowEdge(&dst, u, v);
    }
  }
  gvizSubgraphRebuild(&dst);
  return dst;
}

static int highlightHasEdge(const gvizSubgraph *sg, size_t u, size_t v) {
  if (gvizSubgraphHasEdge(sg, u, v))
    return 1;
  if (u == v)
    return 0;
  return gvizSubgraphHasEdge(sg, v, u);
}

static void applyHighlightColors(grRenderer *r) {
  if (!r->graph)
    return;

  if (!r->highlightActive || !gvizEmbeddedGraphHasHighlight(r->graph)) {
    if (r->hasNodeColors || r->hasEdgeColors) {
      grRendererSetNodeColors(r, NULL, 0);
      grRendererSetEdgeColors(r, NULL, 0);
    }
    r->highlightDirty = false;
    return;
  }

  if (!r->highlightDirty)
    return;

  const gvizSubgraph *highlight = gvizEmbeddedGraphGetHighlight(r->graph);
  size_t nodeCount = r->posCapacity;
  uint32_t *nodeColors = calloc(nodeCount, sizeof(uint32_t));
  if (!nodeColors)
    return;

  uint32_t baseNode = colorToRgba8(&r->nodeStyle.fillColor);
  for (size_t i = 0; i < nodeCount; i++)
    nodeColors[i] = baseNode;

  size_t u;
  gvizSubgraphVertexIterator vit =
      gvizSubgraphVertexIteratorCreate(highlight);
  while (gvizSubgraphVertexIterate(&vit, &u)) {
    if (r->highlightNodeRgba)
      nodeColors[u] = r->highlightNodeRgba;
  }

  if (grRendererSetNodeColors(r, nodeColors, nodeCount) < 0) {
    free(nodeColors);
    return;
  }
  free(nodeColors);

  size_t edgeCount = r->topo.edgeCount;
  if (edgeCount == 0) {
    r->highlightDirty = false;
    return;
  }

  uint32_t *edgeColors = calloc(edgeCount, sizeof(uint32_t));
  if (!edgeColors)
    return;

  uint32_t baseEdge = colorToRgba8(&r->edgeStyle.color);
  for (size_t i = 0; i < edgeCount; i++)
    edgeColors[i] = baseEdge;

  if (r->highlightEdgeRgba) {
    for (size_t i = 0; i < edgeCount; i++) {
      uint32_t eu = r->topo.edges[i * 2];
      uint32_t ev = r->topo.edges[i * 2 + 1];
      if (highlightHasEdge(highlight, eu, ev))
        edgeColors[i] = r->highlightEdgeRgba;
    }
  }

  grRendererSetEdgeColors(r, edgeColors, edgeCount);
  free(edgeColors);
  r->highlightDirty = false;
}

static void highlightShowBoundaryEdge(gvizSubgraph *sg, size_t u, size_t v) {
  if (u > v) {
    size_t t = u;
    u = v;
    v = t;
  }
  gvizSubgraphShowEdge(sg, u, v);
}

int grRendererSetHighlight(grRenderer *r, const gvizSubgraph *highlight,
                           uint32_t nodeRgba, uint32_t edgeRgba) {
  if (!r || !r->graph || !highlight)
    return -1;

  gvizSubgraph copy = grHighlightCopySubgraph(highlight);
  if (!copy.g)
    return -1;

  gvizEmbeddedGraphSetHighlight(r->graph, copy);
  r->highlightActive = true;
  r->highlightNodeRgba = nodeRgba;
  r->highlightEdgeRgba = edgeRgba;
  r->highlightDirty = true;
  return 0;
}

int grRendererSetHighlightCycle(grRenderer *r, const size_t *vertices,
                                size_t count, uint32_t nodeRgba,
                                uint32_t edgeRgba) {
  if (!r || !r->graph || !vertices || count < 3)
    return -1;

  const gvizGraph *g = gvizEmbeddedGraphStructure(r->graph)->g;
  gvizSubgraph cycle = gvizSubgraphCreateEmpty(g);
  if (!cycle.g)
    return -1;

  for (size_t i = 0; i < count; i++)
    gvizSubgraphShowVertex(&cycle, vertices[i]);
  for (size_t i = 0; i < count; i++)
    highlightShowBoundaryEdge(&cycle, vertices[i], vertices[(i + 1) % count]);

  int res = grRendererSetHighlight(r, &cycle, nodeRgba, edgeRgba);
  gvizSubgraphRelease(&cycle);
  return res;
}

void grRendererClearHighlight(grRenderer *r) {
  if (!r)
    return;
  if (r->graph)
    gvizEmbeddedGraphClearHighlight(r->graph);
  grHighlightReset(r);
  grRendererSetNodeColors(r, NULL, 0);
  grRendererSetEdgeColors(r, NULL, 0);
}

static void grenderActionPickFace(gvizEmbeddedGraph *eg, void *userData,
                                  const gvizActionPayload *payload) {
  grRenderer *r = userData;
  if (!r || !payload)
    return;

  gvizSubgraph face = {0};
  if (gvizPlanarFaceSubgraphAt(eg, payload->worldX, payload->worldY,
                               &face) != 0) {
    grRendererClearHighlight(r);
    return;
  }

  grRendererSetHighlight(r, &face, GR_RGBA8(255, 210, 80, 255),
                         GR_RGBA8(255, 180, 40, 255));
  gvizSubgraphRelease(&face);
}

/**
 * World-space click tolerance for grenderActionPickVertex: the radius (in
 * world units, at depth (x, y, z)) that vertex @p v is actually drawn at on
 * screen right now -- honoring a per-vertex size from grRendererSetNodeSizes
 * when one is active, exactly like the vertex shader does. Ports the shader's
 * own pxPerWorld/radiusPx math (grShaders.h) to the CPU so a click only
 * counts as landing "on" a vertex when it falls within the same circle the
 * user sees, then converts that pixel radius back to world units for
 * comparison against worldX/worldY (already unprojected onto the
 * camera-target plane, same as pick-face).
 */
static double grHitTestVertexEpsilon(grRenderer *r, uint32_t v, double x,
                                     double y, double z) {
  const float *viewProj = r->cameraFrame.viewProj;
  double clipW = (double)viewProj[3] * x + (double)viewProj[7] * y +
                (double)viewProj[11] * z + (double)viewProj[15];
  if (clipW < 1e-6)
    clipW = 1e-6;
  double pxPerWorld =
      (double)r->cameraFrame.proj11 * r->viewportHeightPx / (2.0 * clipW);
  if (pxPerWorld <= 0.0)
    return 0.0;

  double radius = (r->hasNodeSizes && r->nodeSizesStaging &&
                   v < r->nodeSizesStagingCount)
                      ? r->nodeSizesStaging[v]
                      : r->nodeStyle.radius;
  double radiusPx = radius;
  if (r->nodeStyle.sizeMode == GR_SIZE_WORLD) {
    radiusPx = radius * pxPerWorld;
    if (r->nodeStyle.maxPixelRadius > 0.0f)
      radiusPx = fmin(radiusPx, r->nodeStyle.maxPixelRadius);
    radiusPx = fmax(radiusPx, r->nodeStyle.minPixelRadius);
  }
  return radiusPx / pxPerWorld;
}

static void grenderActionPickVertex(gvizEmbeddedGraph *eg, void *userData,
                                    const gvizActionPayload *payload) {
  grRenderer *r = userData;
  if (!r || !payload || r->topo.nodeCount == 0)
    return;

  size_t dim = gvizEmbeddedGraphDim(eg);
  const double *pos = gvizEmbeddedGraphPositions(eg);

  // Picks the vertex whose drawn circle contains the click, preferring the
  // most centrally-contained one when circles overlap (smaller vertices
  // sitting on/near a larger one must still be selectable, so this can't
  // just take the nearest center and test that one vertex's radius alone --
  // per-vertex sizes vary, so the nearest center isn't necessarily the
  // vertex whose circle actually reaches the click).
  size_t best = SIZE_MAX;
  double bestRatio2 = 0.0;
  for (size_t i = 0; i < r->topo.nodeCount; i++) {
    uint32_t v = r->topo.nodeIds[i];
    const double *p = pos + (size_t)v * dim;
    double dx = p[0] - payload->worldX;
    double dy = p[1] - payload->worldY;
    double d2 = dx * dx + dy * dy;

    double epsilon = grHitTestVertexEpsilon(r, v, p[0], p[1],
                                            dim >= 3 ? p[2] : 0.0);
    if (epsilon <= 0.0)
      continue;
    double ratio2 = d2 / (epsilon * epsilon);
    if (ratio2 > 1.0)
      continue; // click falls outside this vertex's on-screen circle
    if (best == SIZE_MAX || ratio2 < bestRatio2) {
      best = v;
      bestRatio2 = ratio2;
    }
  }
  if (best == SIZE_MAX) {
    grRendererClearHighlight(r);
    return;
  }
  size_t nearest = best;

  const gvizSubgraph *structure = gvizEmbeddedGraphStructure(eg);
  gvizSubgraph pick = gvizSubgraphCreateEmpty(structure->g);
  if (!pick.g)
    return;

  gvizSubgraphShowVertex(&pick, nearest);
  gvizSubgraphNeighborIterator nit =
      gvizSubgraphNeighborIteratorCreate(structure, nearest);
  size_t v;
  while (gvizSubgraphNeighborIterate(&nit, &v)) {
    gvizSubgraphShowVertex(&pick, v);
    highlightShowBoundaryEdge(&pick, nearest, v);
  }
  gvizSubgraphRebuild(&pick);

  grRendererSetHighlight(r, &pick, GR_RGBA8(255, 210, 80, 255),
                         GR_RGBA8(255, 180, 40, 255));
  gvizSubgraphRelease(&pick);
}

void grRendererFitView(grRenderer *r) { r->fitRequested = true; }

void grRendererRequestClose(grRenderer *r) { r->closeRequested = true; }

double grRendererDeltaTime(const grRenderer *r) { return r->deltaTime; }

static void fitViewNow(grRenderer *r, double fbw, double fbh) {
  if (!r->graph || r->topo.nodeCount == 0)
    return;

  const double *pos = gvizEmbeddedGraphPositions(r->graph);
  size_t srcDim = r->srcDim;
  double bmin[3] = {INFINITY, INFINITY, 0.0};
  double bmax[3] = {-INFINITY, -INFINITY, 0.0};
  if (r->posDim == 3)
    bmin[2] = INFINITY, bmax[2] = -INFINITY;

  if (srcDim == 4) {
    float *proj = malloc(sizeof(float) * r->posCapacity * 3);
    if (!proj)
      return;
    if (grPCAProjectTo3(pos, r->posCapacity, srcDim, proj, r->pcaBasis,
                        r->pcaBasisValid ? r->pcaBasis : NULL) < 0) {
      free(proj);
      return;
    }
    r->pcaBasisValid = true;
    for (size_t i = 0; i < r->topo.nodeCount; i++) {
      const float *p = proj + (size_t)r->topo.nodeIds[i] * 3;
      for (size_t d = 0; d < 3; d++) {
        if (p[d] < bmin[d])
          bmin[d] = p[d];
        if (p[d] > bmax[d])
          bmax[d] = p[d];
      }
    }
    free(proj);
  } else {
    for (size_t i = 0; i < r->topo.nodeCount; i++) {
      const double *p = pos + (size_t)r->topo.nodeIds[i] * srcDim;
      for (size_t d = 0; d < srcDim; d++) {
        if (p[d] < bmin[d])
          bmin[d] = p[d];
        if (p[d] > bmax[d])
          bmax[d] = p[d];
      }
    }
  }
  grCameraFitBox(&r->camera, bmin, bmax, fbw, fbh);
}

/** Applies built-in navigation and queues action dispatches. */
static void processInput(grRenderer *r, double fbw, double fbh) {
  r->viewportHeightPx = fbh;

  double cx, cy;
  glfwGetCursorPos(r->window, &cx, &cy);
  double cxPx = cx * r->contentScale, cyPx = cy * r->contentScale;

  bool leftDown =
      glfwGetMouseButton(r->window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
  bool rightDown =
      glfwGetMouseButton(r->window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
  bool shiftDown = glfwGetKey(r->window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                   glfwGetKey(r->window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;

  double dx = (cx - r->dragLastX) * r->contentScale;
  double dy = (cy - r->dragLastY) * r->contentScale;

  for (int b = 0; b < 3; b++) {
    if (!r->mouseDown[b])
      continue;
    double pdx = (cx - r->mousePressX) * r->contentScale;
    double pdy = (cy - r->mousePressY) * r->contentScale;
    if (pdx * pdx + pdy * pdy > 25.0)
      r->mouseDragged[b] = true;
  }

  bool pan, orbit;
  if (r->camera.perspective) {
    pan = rightDown || (leftDown && shiftDown);
    orbit = leftDown && !shiftDown;
  } else {
    pan = leftDown || rightDown;
    orbit = false;
  }

  if (pan && r->draggingPan)
    grCameraPanPixels(&r->camera, dx, dy, fbh);
  else if (orbit && r->draggingOrbit)
    grCameraOrbit(&r->camera, dx * 0.008, dy * 0.008);

  r->draggingPan = pan;
  r->draggingOrbit = orbit;
  r->dragLastX = cx;
  r->dragLastY = cy;

  if (r->scrollAccum != 0.0) {
    double factor = pow(0.90, r->scrollAccum);
    if (!r->camera.perspective) {
      // Zoom about the cursor: keep the world point under it fixed.
      double wx0, wy0, wx1, wy1;
      grCameraUnproject(&r->camera, &r->cameraFrame, cxPx, cyPx, fbw, fbh,
                        &wx0, &wy0);
      grCameraZoom(&r->camera, factor);
      grCameraUnproject(&r->camera, &r->cameraFrame, cxPx, cyPx, fbw, fbh,
                        &wx1, &wy1);
      r->camera.target[0] += wx0 - wx1;
      r->camera.target[1] += wy0 - wy1;
    } else {
      grCameraZoom(&r->camera, factor);
    }
    r->scrollAccum = 0.0;
  }

  grCameraFrameCompute(&r->camera, fbw, fbh, &r->cameraFrame);

  // Key dispatch: built-in fit on F unless the app bound F itself.
  const grPendingKey *pendingKeys = r->pendingKeys.arr;
  const grKeyBinding *bindings = r->bindings.arr;
  for (size_t k = 0; k < r->pendingKeys.count; k++) {
    int key = pendingKeys[k].key;
    int mods = pendingKeys[k].mods;

    const char *actionName = NULL;
    for (size_t i = 0; i < r->bindings.count; i++) {
      if (bindings[i].key == key) {
        actionName = bindings[i].actionName;
        break;
      }
    }

    if (!actionName) {
      if (key == 'F')
        r->fitRequested = true;
      else if (key == 'S')
        grRendererShowStats(r, !r->statsVisible);
      else if (key == 'I')
        grRendererShowTextureMapImage(r, !grRendererTextureMapImageShown(r));
      continue;
    }
    if (!r->graph)
      continue;

    gvizActionPayload payload = {0};
    grCameraUnproject(&r->camera, &r->cameraFrame, cxPx, cyPx, fbw, fbh,
                      &payload.worldX, &payload.worldY);
    payload.deltaTime = r->deltaTime;
    payload.iarg = mods; // GLFW mod bits match GR_MOD_*
    gvizEmbeddedGraphInvokeAction(r->graph, actionName, &payload);
  }
  r->pendingKeys.count = 0;

  const grPendingMouse *pendingMouse = r->pendingMouse.arr;
  const grMouseBinding *mouseBindings = r->mouseBindings.arr;
  for (size_t m = 0; m < r->pendingMouse.count; m++) {
    int button = pendingMouse[m].button;
    int mods = pendingMouse[m].mods;

    const char *actionName = NULL;
    for (size_t i = 0; i < r->mouseBindings.count; i++) {
      if (mouseBindings[i].button == button) {
        actionName = mouseBindings[i].actionName;
        break;
      }
    }
    if (!actionName && button == GR_MOUSE_BUTTON_LEFT)
      actionName = GR_ACTION_PICK_VERTEX;
    if (!actionName || !r->graph)
      continue;

    gvizActionPayload payload = {0};
    grCameraUnproject(&r->camera, &r->cameraFrame, pendingMouse[m].xPx,
                      pendingMouse[m].yPx, fbw, fbh, &payload.worldX,
                      &payload.worldY);
    payload.deltaTime = r->deltaTime;
    payload.iarg = mods;
    gvizEmbeddedGraphInvokeAction(r->graph, actionName, &payload);
  }
  r->pendingMouse.count = 0;
}

// ------------------------------------------------------------------------------
// Frame
// ------------------------------------------------------------------------------

static void writeGlobals(grRenderer *r, double fbw, double fbh) {
  grGlobalsUBO g = {0};
  memcpy(g.viewProj, r->cameraFrame.viewProj, sizeof(g.viewProj));
  memcpy(g.camRight, r->cameraFrame.camRight, sizeof(float) * 3);
  memcpy(g.camUp, r->cameraFrame.camUp, sizeof(float) * 3);
  g.viewport[0] = (float)fbw;
  g.viewport[1] = (float)fbh;
  g.posDim = (uint32_t)r->posDim;
  g.flags = (r->hasNodeColors ? 1u : 0u) | (r->hasNodeSizes ? 2u : 0u) |
            (r->hasEdgeColors ? 4u : 0u);

  memcpy(g.nodeFill, &r->nodeStyle.fillColor, sizeof(float) * 4);
  memcpy(g.nodeStroke, &r->nodeStyle.strokeColor, sizeof(float) * 4);
  g.nodeParams[0] = r->nodeStyle.radius;
  g.nodeParams[1] = r->nodeStyle.strokeWidth;
  g.nodeParams[2] = r->nodeStyle.sizeMode == GR_SIZE_WORLD ? 1.0f : 0.0f;
  g.nodeParams[3] = r->cameraFrame.proj11;
  g.nodeSizeLimits[0] = r->nodeStyle.minPixelRadius;
  g.nodeSizeLimits[1] = r->nodeStyle.maxPixelRadius;
  g.nodeSizeLimits[2] = 0.0f;
  g.nodeSizeLimits[3] = 0.0f;

  memcpy(g.edgeColor, &r->edgeStyle.color, sizeof(float) * 4);
  g.edgeParams[0] = r->edgeStyle.width;
  g.edgeParams[1] = r->edgeStyle.sizeMode == GR_SIZE_WORLD ? 1.0f : 0.0f;

  wgpuQueueWriteBuffer(r->queue, r->globalsBuf, 0, &g, sizeof(g));
}

static void uploadPositions(grRenderer *r) {
  const double *src = gvizEmbeddedGraphPositions(r->graph);
  size_t n = r->posCapacity;
  float *dst = r->posStaging;
  if (r->srcDim == 4) {
    if (grPCAProjectTo3(src, n, r->srcDim, dst, r->pcaBasis,
                        r->pcaBasisValid ? r->pcaBasis : NULL) < 0) {
      for (size_t i = 0; i < n * 3; i++)
        dst[i] = 0.0f;
    } else {
      r->pcaBasisValid = true;
    }
    wgpuQueueWriteBuffer(r->queue, r->positionsBuf, 0, dst, sizeof(float) * n * 3);
    return;
  }
  for (size_t i = 0; i < n * r->posDim; i++)
    dst[i] = (float)src[i];
  wgpuQueueWriteBuffer(r->queue, r->positionsBuf, 0, dst,
                       sizeof(float) * n * r->posDim);
}

static void statsRevisionCacheSync(grRenderer *r, double fbw, double fbh) {
  size_t n = gvizEmbeddedGraphStatSeriesCount(r->graph);
  r->statsSeriesRevisions.count = 0;
  for (size_t i = 0; i < n; i++) {
    const gvizStatSeries *series = gvizEmbeddedGraphStatSeriesAt(r->graph, i);
    uint64_t rev = series ? series->revision : 0;
    if (gvizArrayPush(&r->statsSeriesRevisions, &rev) < 0)
      return;
  }
  r->statsLayoutFbw = fbw;
  r->statsLayoutFbh = fbh;
  r->statsLayoutScale = r->contentScale > 0.0 ? r->contentScale : 1.0;
  r->statsOverlayDirty = false;
}

static bool statsOverlayNeedsRebuild(grRenderer *r, double fbw, double fbh) {
  if (r->statsOverlayDirty)
    return true;
  double scale = r->contentScale > 0.0 ? r->contentScale : 1.0;
  if (fbw != r->statsLayoutFbw || fbh != r->statsLayoutFbh ||
      scale != r->statsLayoutScale)
    return true;
  size_t n = gvizEmbeddedGraphStatSeriesCount(r->graph);
  if (n != r->statsSeriesRevisions.count)
    return true;
  const uint64_t *cached = r->statsSeriesRevisions.arr;
  for (size_t i = 0; i < n; i++) {
    const gvizStatSeries *series = gvizEmbeddedGraphStatSeriesAt(r->graph, i);
    if (!series)
      continue;
    if (series->revision != cached[i])
      return true;
  }
  return false;
}

/** Rebuilds overlay primitives when stat data or layout changed; uploads
 *  (grow-only buffer). */
static void uploadStats(grRenderer *r, double fbw, double fbh) {
  if (!r->statsVisible) {
    r->statsPrims.count = 0;
    return;
  }
  if (!statsOverlayNeedsRebuild(r, fbw, fbh))
    return;

  r->statsPrims.count = 0;
  grStatsOverlayBuild(r, fbw, fbh);
  statsRevisionCacheSync(r, fbw, fbh);
  if (r->statsPrims.count == 0)
    return;

  if (r->statsPrims.count > r->statsBufCapacity) {
    GR_RELEASE(wgpuBufferRelease, r->statsBuf);
    r->statsBufCapacity = r->statsPrims.count * 2;
    r->statsBuf = createBuffer(
        r, sizeof(grStatsPrim) * r->statsBufCapacity,
        WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
        "grender stats prims");
    r->bindGroupDirty = true;
    if (!r->statsBuf) {
      r->statsPrims.count = 0;
      return;
    }
  }
  wgpuQueueWriteBuffer(r->queue, r->statsBuf, 0, r->statsPrims.arr,
                       sizeof(grStatsPrim) * r->statsPrims.count);
}

/** Encodes the scene render pass (clear + edges + nodes) into @p target. */
static void encodeScenePass(grRenderer *r, WGPUCommandEncoder encoder,
                            WGPUTextureView target, WGPUTextureView depth) {
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(
      encoder,
      &(const WGPURenderPassDescriptor){
          .colorAttachmentCount = 1,
          .colorAttachments =
              &(const WGPURenderPassColorAttachment){
                  .view = target,
                  .loadOp = WGPULoadOp_Clear,
                  .storeOp = WGPUStoreOp_Store,
                  .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
                  .clearValue = {r->clearColor.r, r->clearColor.g,
                                 r->clearColor.b, r->clearColor.a},
              },
          .depthStencilAttachment =
              &(const WGPURenderPassDepthStencilAttachment){
                  .view = depth,
                  .depthLoadOp = WGPULoadOp_Clear,
                  .depthStoreOp = WGPUStoreOp_Store,
                  .depthClearValue = 1.0f,
              },
      });

  // Texture map's movable image rect, if shown: drawn first (and never
  // depth-writing) so nodes/edges always remain visible on top of it,
  // letting a user see exactly how the image and the live graph overlap.
  grTextureMapEncodeImageQuad(r, pass);

  if (r->graph && r->bindGroup) {
    wgpuRenderPassEncoderSetBindGroup(pass, 0, r->bindGroup, 0, NULL);

    if (r->topo.nodeCount) {
      // 2D: edges below nodes (equal depth, later draw wins).
      // 3D: nodes first so their depth occludes edges behind them.
      bool nodesFirst = r->posDim == 3;
      for (int step = 0; step < 2; step++) {
        bool drawNodes = (step == 0) == nodesFirst;
        if (drawNodes) {
          wgpuRenderPassEncoderSetPipeline(pass, r->nodePipeline);
          wgpuRenderPassEncoderDraw(pass, 6, (uint32_t)r->topo.nodeCount, 0, 0);
        } else if (r->topo.edgeCount) {
          wgpuRenderPassEncoderSetPipeline(pass, r->edgePipeline);
          wgpuRenderPassEncoderDraw(pass, 6, (uint32_t)r->topo.edgeCount, 0, 0);
        }
      }
    }

    // Stats overlay always draws on top of the scene.
    if (r->statsPrims.count) {
      wgpuRenderPassEncoderSetPipeline(pass, r->statsPipeline);
      wgpuRenderPassEncoderDraw(pass, 6, (uint32_t)r->statsPrims.count, 0, 0);
    }
  }

  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);
}

static void onScreenshotMap(WGPUMapAsyncStatus status, WGPUStringView message,
                            void *userdata1, void *userdata2) {
  (void)userdata2;
  *(WGPUMapAsyncStatus *)userdata1 = status;
  if (status != WGPUMapAsyncStatus_Success)
    GR_LOG("screenshot map failed: %.*s\n", (int)message.length, message.data);
}

int grRendererSaveScreenshot(grRenderer *r, const char *path) {
  uint32_t w = r->surfaceConfig.width, h = r->surfaceConfig.height;
  if (w == 0 || h == 0)
    return -1;

  grCameraFrameCompute(&r->camera, w, h, &r->cameraFrame);
  writeGlobals(r, w, h);
  if (r->graph) {
    uploadPositions(r);
    uploadStats(r, w, h);
    if (r->bindGroupDirty && rebuildBindGroup(r) < 0)
      return -1;
  }

  WGPUTexture target = wgpuDeviceCreateTexture(
      r->device, &(const WGPUTextureDescriptor){
                     .label = {"grender screenshot", WGPU_STRLEN},
                     .usage = WGPUTextureUsage_RenderAttachment |
                              WGPUTextureUsage_CopySrc,
                     .dimension = WGPUTextureDimension_2D,
                     .size = {w, h, 1},
                     .format = r->surfaceFormat,
                     .mipLevelCount = 1,
                     .sampleCount = 1,
                 });
  if (!target)
    return -1;
  WGPUTextureView targetView = wgpuTextureCreateView(target, NULL);

  const uint32_t bytesPerRow = (w * 4 + 255) & ~255u; // 256-byte alignment
  WGPUBuffer readback = wgpuDeviceCreateBuffer(
      r->device, &(const WGPUBufferDescriptor){
                     .label = {"grender readback", WGPU_STRLEN},
                     .size = (uint64_t)bytesPerRow * h,
                     .usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead,
                 });

  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(r->device, NULL);
  encodeScenePass(r, encoder, targetView, r->depthView);
  grObjOverlayEncode(r, encoder, targetView, r->depthView, w, h);
  wgpuCommandEncoderCopyTextureToBuffer(
      encoder,
      &(const WGPUTexelCopyTextureInfo){.texture = target},
      &(const WGPUTexelCopyBufferInfo){
          .layout = {.bytesPerRow = bytesPerRow, .rowsPerImage = h},
          .buffer = readback,
      },
      &(const WGPUExtent3D){w, h, 1});
  WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, NULL);
  wgpuQueueSubmit(r->queue, 1, &commands);
  wgpuCommandBufferRelease(commands);
  wgpuCommandEncoderRelease(encoder);

  WGPUMapAsyncStatus mapStatus = 0;
  wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0,
                     (size_t)bytesPerRow * h,
                     (const WGPUBufferMapCallbackInfo){
                         .callback = onScreenshotMap,
                         .userdata1 = &mapStatus,
                     });
  wgpuDevicePoll(r->device, true, NULL);

  int result = -1;
  if (mapStatus == WGPUMapAsyncStatus_Success) {
    const uint8_t *data =
        wgpuBufferGetConstMappedRange(readback, 0, (size_t)bytesPerRow * h);
    FILE *f = data ? fopen(path, "wb") : NULL;
    if (f) {
      // Surface formats are 8-bit RGBA or BGRA; swizzle BGRA on write.
      bool bgra = r->surfaceFormat == WGPUTextureFormat_BGRA8Unorm ||
                  r->surfaceFormat == WGPUTextureFormat_BGRA8UnormSrgb;
      fprintf(f, "P6\n%u %u\n255\n", w, h);
      uint8_t *row = malloc((size_t)w * 3);
      if (row) {
        for (uint32_t y = 0; y < h; y++) {
          const uint8_t *src = data + (size_t)y * bytesPerRow;
          for (uint32_t x = 0; x < w; x++) {
            row[x * 3 + 0] = src[x * 4 + (bgra ? 2 : 0)];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + (bgra ? 0 : 2)];
          }
          fwrite(row, 3, w, f);
        }
        free(row);
        result = 0;
      }
      fclose(f);
    }
    wgpuBufferUnmap(readback);
  }

  wgpuBufferRelease(readback);
  wgpuTextureViewRelease(targetView);
  wgpuTextureRelease(target);
  return result;
}

bool grRendererFrame(grRenderer *r) {
  if (r->closeRequested || glfwWindowShouldClose(r->window))
    return false;

  GR_PROF_FRAME_BEGIN();

  glfwPollEvents();

  double now = glfwGetTime();
  r->deltaTime = now - r->lastFrameTime;
  r->lastFrameTime = now;

  int fbwI, fbhI, winW, winH;
  glfwGetFramebufferSize(r->window, &fbwI, &fbhI);
  glfwGetWindowSize(r->window, &winW, &winH);
  if (fbwI == 0 || fbhI == 0) // minimized
    return true;
  double fbw = fbwI, fbh = fbhI;
  r->contentScale = winW > 0 ? fbw / winW : 1.0;

  if (r->surfaceDirty || (uint32_t)fbwI != r->surfaceConfig.width ||
      (uint32_t)fbhI != r->surfaceConfig.height) {
    r->surfaceConfig.width = (uint32_t)fbwI;
    r->surfaceConfig.height = (uint32_t)fbhI;
    wgpuSurfaceConfigure(r->surface, &r->surfaceConfig);
    recreateDepthTexture(r);
    r->surfaceDirty = false;
  }

  processInput(r, fbw, fbh);
  grObjOverlayUpdate(r, r->deltaTime);

  if (r->graph) {
    statsMenuSyncIfNeeded(r);
    uint64_t rev = gvizEmbeddedGraphDrawMaskRevision(r->graph);
    if (rev != r->drawMaskRevision) {
      r->drawMaskRevision = rev;
      r->topoDirty = true;
    }
    if (r->topoDirty) {
      if (ensurePositionBuffers(r) < 0 || uploadTopology(r) < 0)
        return false;
      r->topoDirty = false;
    }
    applyHighlightColors(r);
    if (r->fitRequested) {
      fitViewNow(r, fbw, fbh);
      r->fitRequested = false;
    }
  }

  writeGlobals(r, fbw, fbh);

  if (r->graph) {
    uploadPositions(r);
    uploadStats(r, fbw, fbh);
    if (r->bindGroupDirty && rebuildBindGroup(r) < 0)
      return false;
  }

  // acquire frame
  WGPUSurfaceTexture surfaceTexture;
  wgpuSurfaceGetCurrentTexture(r->surface, &surfaceTexture);
  switch (surfaceTexture.status) {
  case WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal:
  case WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal:
    break;
  default:
    if (surfaceTexture.texture)
      wgpuTextureRelease(surfaceTexture.texture);
    r->surfaceDirty = true;
    return true; // skip the frame; surface reconfigured next time
  }

  WGPUTextureView frame = wgpuTextureCreateView(surfaceTexture.texture, NULL);
  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(
      r->device,
      &(const WGPUCommandEncoderDescriptor){.label = {"grender", WGPU_STRLEN}});

  encodeScenePass(r, encoder, frame, r->depthView);
  grObjOverlayEncode(r, encoder, frame, r->depthView, fbw, fbh);

  WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, NULL);
  wgpuQueueSubmit(r->queue, 1, &commands);
  wgpuSurfacePresent(r->surface);

  wgpuCommandBufferRelease(commands);
  wgpuCommandEncoderRelease(encoder);
  wgpuTextureViewRelease(frame);
  wgpuTextureRelease(surfaceTexture.texture);
  GR_PROF_FRAME_END();
  return true;
}
