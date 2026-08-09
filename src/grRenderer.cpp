#include "grInternal.h"
#include "grShaders.h"

#ifndef __EMSCRIPTEN__
#include <webgpu/wgpu.h> // wgpu-native extensions (wgpuDevicePoll)
#endif

#include <GLFW/glfw3.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>
#include <optional>
#include <utility>

#define GR_LOG(...) fprintf(stderr, "[grender] " __VA_ARGS__)

static void grenderActionPickFace(gviz::layout::EmbeddedGraph &eg,
                                  void *userData,
                                  const gviz::layout::ActionPayload &payload);
static void grenderActionPickVertex(gviz::layout::EmbeddedGraph &eg,
                                    void *userData,
                                    const gviz::layout::ActionPayload &payload);

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
  desc->edgeDegreeAlpha = false;
  desc->edgeWeightWidth = false;
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
                                grPtr(WGPUBufferDescriptor{
                                    .label = {label, WGPU_STRLEN},
                                    .size = size,
                                    .usage = usage,
                                }));
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
      grPtr(WGPUShaderModuleDescriptor{
          .label = {"grender shaders", WGPU_STRLEN},
          .nextInChain =
              (WGPUChainedStruct *)grPtr(WGPUShaderSourceWGSL{
                  .chain = {.sType = WGPUSType_ShaderSourceWGSL},
                  .code = {GR_WGSL_SOURCE, WGPU_STRLEN},
              }),
      }));
  if (!r->shaderModule)
    return -1;

  WGPUBindGroupLayoutEntry entries[10] = {0};
  entries[0] = (WGPUBindGroupLayoutEntry){
      .binding = 0,
      .visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment,
      .buffer = {.type = WGPUBufferBindingType_Uniform},
  };
  for (int i = 1; i < 10; i++) {
    entries[i] = (WGPUBindGroupLayoutEntry){
        .binding = (uint32_t)i,
        .visibility = WGPUShaderStage_Vertex,
        .buffer = {.type = WGPUBufferBindingType_ReadOnlyStorage},
    };
  }

  r->bindGroupLayout = wgpuDeviceCreateBindGroupLayout(
      r->device, grPtr(WGPUBindGroupLayoutDescriptor{
                     .label = {"grender bgl", WGPU_STRLEN},
                     .entryCount = 10,
                     .entries = entries,
                 }));
  r->pipelineLayout = wgpuDeviceCreatePipelineLayout(
      r->device, grPtr(WGPUPipelineLayoutDescriptor{
                     .label = {"grender layout", WGPU_STRLEN},
                     .bindGroupLayoutCount = 1,
                     .bindGroupLayouts =
                         (const WGPUBindGroupLayout[]){r->bindGroupLayout},
                 }));
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
        grPtr(WGPURenderPipelineDescriptor{
            .label = {labels[i], WGPU_STRLEN},
            .layout = r->pipelineLayout,
            .vertex = {.module = r->shaderModule,
                       .entryPoint = {vsEntries[i], WGPU_STRLEN}},
            .fragment =
                grPtr(WGPUFragmentState{
                    .module = r->shaderModule,
                    .entryPoint = {fsEntries[i], WGPU_STRLEN},
                    .targetCount = 1,
                    .targets = &colorTarget,
                }),
            .primitive = {.topology = WGPUPrimitiveTopology_TriangleList,
                          .cullMode = WGPUCullMode_None},
            .depthStencil = &depthState,
            .multisample = {.count = 1, .mask = 0xFFFFFFFF},
        }));
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
      r->device, grPtr(WGPUTextureDescriptor{
                     .label = {"grender depth", WGPU_STRLEN},
                     .usage = WGPUTextureUsage_RenderAttachment,
                     .dimension = WGPUTextureDimension_2D,
                     .size = {r->surfaceConfig.width, r->surfaceConfig.height, 1},
                     .format = WGPUTextureFormat_Depth24Plus,
                     .mipLevelCount = 1,
                     .sampleCount = 1,
                 }));
  r->depthView = wgpuTextureCreateView(r->depthTexture, NULL);
}

// ------------------------------------------------------------------------------
// GLFW callbacks
// ------------------------------------------------------------------------------

static void onFramebufferSize(GLFWwindow *window, int width, int height) {
  (void)width, (void)height;
  grRenderer *r = (grRenderer *)glfwGetWindowUserPointer(window);
  if (r)
    r->surfaceDirty = true;
}

static void onScroll(GLFWwindow *window, double dx, double dy) {
  (void)dx;
  grRenderer *r = (grRenderer *)glfwGetWindowUserPointer(window);
  if (r)
    r->scrollAccum += dy;
}

static void onKey(GLFWwindow *window, int key, int scancode, int action,
                  int mods) {
  (void)scancode;
  grRenderer *r = (grRenderer *)glfwGetWindowUserPointer(window);
  if (!r || (action != GLFW_PRESS && action != GLFW_REPEAT))
    return;

  grPendingKey pk = {key, mods};
  r->pendingKeys.push_back(pk);

  // Also recorded in the console's own chronologically-ordered queue (see
  // grPendingConsoleEvent) so Enter/Escape/Backspace interleave correctly
  // with typed characters from onChar below; harmless/unused when the
  // console is closed (processInput discards it every such frame).
  grPendingConsoleEvent ce = {.isChar = false, .code = key};
  r->pendingConsoleEvents.push_back(ce);
}

static void onChar(GLFWwindow *window, unsigned int codepoint) {
  grRenderer *r = (grRenderer *)glfwGetWindowUserPointer(window);
  if (!r)
    return;

  grPendingConsoleEvent ce = {.isChar = true, .code = (int32_t)codepoint};
  r->pendingConsoleEvents.push_back(ce);
}

static void onMouseButton(GLFWwindow *window, int button, int action, int mods) {
  grRenderer *r = (grRenderer *)glfwGetWindowUserPointer(window);
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
  r->pendingMouse.push_back(pm);
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

  // Not calloc: grRenderer now owns real C++ members (std::vector fields,
  // gviz pointer fields with default member initializers) that need their
  // constructors to actually run, not a zero-fill over raw bytes. Value
  // -initializing via `new grRenderer()` gives every field its declared
  // default (0/false/nullptr for the plain-old-data fields, a properly
  // constructed empty vector for the std::vector ones) in one step, so the
  // explicit gvizArrayInit-equivalent calls below are gone entirely (RAII).
  grRenderer *r = new (std::nothrow) grRenderer();
  if (!r)
    return NULL;
  r->clearColor = descIn->clearColor;
  r->nodeStyle = descIn->nodeStyle;
  r->edgeStyle = descIn->edgeStyle;
  r->edgeDegreeAlpha = descIn->edgeDegreeAlpha;
  r->edgeWeightWidth = descIn->edgeWeightWidth;
  r->statsVisible = true;
  r->pickedVertexId = -1;
  r->listVisible = true;
  r->listSelectedIdx = SIZE_MAX;
  grCameraInit2D(&r->camera);

#ifdef __APPLE__
  glfwInitHint(GLFW_COCOA_MENUBAR, GLFW_FALSE);
#endif
  if (!glfwInit()) {
    GR_LOG("glfwInit failed\n");
    delete r;
    return NULL;
  }

  grPlatformInitApplication();

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  r->window = glfwCreateWindow((int)descIn->width, (int)descIn->height,
                               descIn->title ? descIn->title : "grender", NULL,
                               NULL);
  if (!r->window)
    { grRendererDestroy(r); return NULL; }

  glfwSetWindowUserPointer(r->window, r);
  glfwSetFramebufferSizeCallback(r->window, onFramebufferSize);
  glfwSetScrollCallback(r->window, onScroll);
  glfwSetKeyCallback(r->window, onKey);
  glfwSetCharCallback(r->window, onChar);
  glfwSetMouseButtonCallback(r->window, onMouseButton);

#ifdef __EMSCRIPTEN__
  // wgpuInstanceWaitAny (used below and in grRendererSaveScreenshot) errors
  // out unless the instance opted into the TimedWaitAny feature up front --
  // wgpu-native's synchronous callbacks need no such opt-in, hence native
  // still passes NULL here.
  WGPUInstanceFeatureName instanceFeatures[] = {WGPUInstanceFeatureName_TimedWaitAny};
  r->instance = wgpuCreateInstance(grPtr(WGPUInstanceDescriptor{
      .requiredFeatureCount = 1,
      .requiredFeatures = instanceFeatures,
  }));
#else
  r->instance = wgpuCreateInstance(NULL);
#endif
  if (!r->instance)
    { grRendererDestroy(r); return NULL; }

  r->surface = grPlatformCreateSurface(r->instance, r->window);
  if (!r->surface)
    { grRendererDestroy(r); return NULL; }

#ifdef __EMSCRIPTEN__
  // Unlike wgpu-native, the browser's WebGPU never resolves a callback
  // registered with mode 0 -- it must be WaitAnyOnly, and the future must
  // actually be waited on. -sASYNCIFY (see CMakeLists.txt) makes this wait
  // genuinely yield to the browser event loop instead of hanging the tab,
  // so from here down the call still reads as a blocking request.
  WGPUFuture adapterFuture = wgpuInstanceRequestAdapter(
      r->instance,
      grPtr(WGPURequestAdapterOptions{.compatibleSurface = r->surface}),
      (const WGPURequestAdapterCallbackInfo){.mode = WGPUCallbackMode_WaitAnyOnly,
                                             .callback = onAdapterRequest,
                                             .userdata1 = &r->adapter});
  WGPUFutureWaitInfo adapterWait = {.future = adapterFuture};
  wgpuInstanceWaitAny(r->instance, 1, &adapterWait, UINT64_MAX);
#else
  // wgpu-native services these callbacks synchronously.
  wgpuInstanceRequestAdapter(
      r->instance,
      grPtr(WGPURequestAdapterOptions{.compatibleSurface = r->surface}),
      (const WGPURequestAdapterCallbackInfo){.callback = onAdapterRequest,
                                             .userdata1 = &r->adapter});
#endif
  if (!r->adapter)
    { grRendererDestroy(r); return NULL; }

  WGPULimits adapterLimits = WGPU_LIMITS_INIT;
  if (wgpuAdapterGetLimits(r->adapter, &adapterLimits) != WGPUStatus_Success) {
    GR_LOG("failed to query adapter limits\n");
    { grRendererDestroy(r); return NULL; }
  }

  WGPULimits requiredLimits = WGPU_LIMITS_INIT;
  requiredLimits.maxStorageBufferBindingSize =
      adapterLimits.maxStorageBufferBindingSize;
  requiredLimits.maxBufferSize = adapterLimits.maxBufferSize;
  // Bind group has 9 storage buffers (bindings 1-9); the default WebGPU
  // baseline of 8 is one short, so ask the adapter for what it actually
  // supports.
  requiredLimits.maxStorageBuffersPerShaderStage =
      adapterLimits.maxStorageBuffersPerShaderStage;

#ifdef __EMSCRIPTEN__
  WGPUFuture deviceFuture = wgpuAdapterRequestDevice(
      r->adapter,
      grPtr(WGPUDeviceDescriptor{
          .label = {"grender device", WGPU_STRLEN},
          .requiredLimits = &requiredLimits,
          .uncapturedErrorCallbackInfo = {.callback = onUncapturedError},
      }),
      (const WGPURequestDeviceCallbackInfo){.mode = WGPUCallbackMode_WaitAnyOnly,
                                            .callback = onDeviceRequest,
                                            .userdata1 = &r->device});
  WGPUFutureWaitInfo deviceWait = {.future = deviceFuture};
  wgpuInstanceWaitAny(r->instance, 1, &deviceWait, UINT64_MAX);
#else
  wgpuAdapterRequestDevice(
      r->adapter,
      grPtr(WGPUDeviceDescriptor{
          .label = {"grender device", WGPU_STRLEN},
          .requiredLimits = &requiredLimits,
          .uncapturedErrorCallbackInfo = {.callback = onUncapturedError},
      }),
      (const WGPURequestDeviceCallbackInfo){.callback = onDeviceRequest,
                                            .userdata1 = &r->device});
#endif
  if (!r->device)
    { grRendererDestroy(r); return NULL; }

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
    { grRendererDestroy(r); return NULL; }
  }

  r->globalsBuf = createBuffer(r, sizeof(grGlobalsUBO),
                               WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                               "grender globals");

  glfwGetCursorPos(r->window, &r->dragLastX, &r->dragLastY);
  r->lastFrameTime = glfwGetTime();
  return r;

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
  GR_RELEASE(wgpuBufferRelease, r->nodeDegreesBuf);
  GR_RELEASE(wgpuBufferRelease, r->edgeWeightsBuf);
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
  free(r->nodeColorsStaging);
  free(r->edgeColorsStaging);
  free(r->statsSeriesVisible);
  delete r; // matches grRendererCreate's `new`; runs every member's destructor
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

  size_t n = r->graph->StatSeriesCount();
  if (n == 0)
    return;

  r->statsSeriesVisible = (bool *)calloc(n, sizeof(bool));
  if (!r->statsSeriesVisible)
    return;
  for (size_t i = 0; i < n; i++)
    r->statsSeriesVisible[i] = true;
  r->statsSeriesVisibleCount = n;
}

static void statsMenuSyncIfNeeded(grRenderer *r) {
  if (!r->graph)
    return;
  size_t n = r->graph->StatSeriesCount();
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
  return r->graph->StatSeriesCount();
}

const char *grRendererStatSeriesName(const grRenderer *r, size_t idx) {
  if (!r || !r->graph)
    return NULL;
  const gviz::layout::StatSeries *series = r->graph->StatSeriesAt(idx);
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
    r->statsPrims.clear();
  grPlatformStatsMenuRefresh(r);
}

bool grRendererStatsShown(const grRenderer *r) { return r->statsVisible; }

void grRendererShowConsole(grRenderer *r, bool show) {
  if (!r || r->consoleOpen == show)
    return;
  if (show)
    grConsoleOpen(r);
  else
    grConsoleClose(r);
}

bool grRendererConsoleShown(const grRenderer *r) {
  return r && r->consoleOpen;
}

void grRendererShowVertexList(grRenderer *r, bool show) {
  if (!r || r->listVisible == show)
    return;
  r->listVisible = show;
  r->listOverlayDirty = true;
  if (!show)
    r->listSearchFocused = false;
}

bool grRendererVertexListShown(const grRenderer *r) {
  return r && r->listVisible;
}

// ------------------------------------------------------------------------------
// Graph attachment and GPU buffer management
// ------------------------------------------------------------------------------

static int uploadAttribute(grRenderer *r, WGPUBuffer *buf, const void *data,
                           size_t bytes, bool *flag, const char *label);
static uint32_t colorToRgba8(const grColor *c);

/** (Re)creates position-indexed buffers when the vertex capacity changes. */
static int ensurePositionBuffers(grRenderer *r) {
  size_t count = r->graph->PositionCount();
  size_t srcDim = r->graph->Dim();
  size_t renderDim = srcDim == 4 ? 3 : srcDim;
  if (count == r->posCapacity && srcDim == r->srcDim && renderDim == r->posDim &&
      r->positionsBuf)
    return 0;

  free(r->posStaging);
  r->posStaging = (float *)malloc(sizeof(float) * count * renderDim);
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

  // Per-vertex attributes are indexed the same way. Node sizes are
  // preserved across capacity growth from their CPU staging copy -- new
  // slots default to the global style radius until the client re-uploads
  // real values -- so a growth commit never blanks every vertex's size for
  // a frame. Client base node colors get the same treatment below.
  if (r->hasNodeSizes && r->nodeSizesStaging) {
    float *grown = (float *)realloc(r->nodeSizesStaging, sizeof(float) * count);
    if (grown) {
      for (size_t i = r->nodeSizesStagingCount; i < count; i++)
        grown[i] = r->nodeStyle.radius;
      r->nodeSizesStaging = grown;
      r->nodeSizesStagingCount = count;
      if (uploadAttribute(r, &r->nodeSizesBuf, grown, sizeof(float) * count,
                          &r->hasNodeSizes, "grender node sizes") < 0)
        r->hasNodeSizes = false;
    } else {
      free(r->nodeSizesStaging);
      r->nodeSizesStaging = NULL;
      r->nodeSizesStagingCount = 0;
      r->hasNodeSizes = false;
    }
  } else {
    r->hasNodeSizes = false;
  }
  // Client base node colors are preserved across capacity growth from their
  // CPU staging copy, exactly like nodeSizesStaging above -- new slots
  // default to the global fill style until the client re-uploads real
  // values. The composited GPU buffer bound for drawing is rebuilt by
  // applyColorLayers (colorsDirty) regardless, since it must reflect the
  // new capacity either way.
  if (r->hasClientNodeColors && r->nodeColorsStaging) {
    uint32_t *grown =
        (uint32_t *)realloc(r->nodeColorsStaging, sizeof(uint32_t) * count);
    if (grown) {
      uint32_t baseNode = colorToRgba8(&r->nodeStyle.fillColor);
      for (size_t i = r->nodeColorsStagingCount; i < count; i++)
        grown[i] = baseNode;
      r->nodeColorsStaging = grown;
      r->nodeColorsStagingCount = count;
    } else {
      free(r->nodeColorsStaging);
      r->nodeColorsStaging = NULL;
      r->nodeColorsStagingCount = 0;
      r->hasClientNodeColors = false;
    }
  } else {
    r->hasClientNodeColors = false;
  }
  r->colorsDirty = true;
  r->vertexLabels = NULL;
  r->vertexLabelsCount = 0;
  if (r->pickedVertexId != -1) {
    r->pickedVertexId = -1;
    r->vertexOverlayDirty = true;
  }
  return 0;
}

/* Degree of raw vertex @p v as the drawn structure defines it: the
 * embedding's synced out+in rows when a snapshot exists (matching exactly
 * what grTopologyExtract drew), the live subgraph otherwise. */
static uint32_t topoVertexDegree(gviz::layout::EmbeddedGraph &graph, size_t v) {
  std::span<const size_t> outNbrs = graph.OutNeighbors(v);
  if (!outNbrs.empty())
    return (uint32_t)(outNbrs.size() + graph.InDegree(v));
  return (uint32_t)graph.Structure().Degree(v);
}

/* Recomputes and re-uploads the per-vertex degrees the edge shader's
 * degree-alpha mode reads. Called on every structural change, so degrees
 * stay current as the graph grows -- a new edge changes its endpoints'
 * degrees even when both vertices are old. */
static int refreshNodeDegrees(grRenderer *r) {
  if (r->posCapacity == 0)
    return 0;
  uint32_t *degrees = (uint32_t *)calloc(r->posCapacity, sizeof(uint32_t));
  if (!degrees)
    return -1;
  for (size_t i = 0; i < r->topo.nodeCount; i++) {
    uint32_t v = r->topo.nodeIds[i];
    degrees[v] = topoVertexDegree(*r->graph, v);
  }
  int res = grRendererSetNodeDegrees(r, degrees, r->posCapacity);
  free(degrees);
  return res;
}

/* Recomputes and re-uploads per-edge weights in the exact order of the edge
 * buffer just extracted -- the only ordering the weight-width shader can
 * index by, and one that changes wholesale on every structural change. */
static int refreshEdgeWeights(grRenderer *r) {
  size_t edgeCount = r->topo.edgeCount;
  if (edgeCount == 0)
    return grRendererSetEdgeWeights(r, NULL, 0);
  float *weights = (float *)malloc(sizeof(float) * edgeCount);
  if (!weights)
    return -1;
  // gviz::Subgraph deliberately never exposes its parent graph, so real
  // weights need r->backingGraph (see grRendererSetGraph); with none
  // attached every edge just reports the default weight of 1.0, same as an
  // edge whose weight was never explicitly set.
  gviz::Graph *g = r->backingGraph;
  for (size_t i = 0; i < edgeCount; i++) {
    double w = 1.0;
    if (g)
      g->GetEdgeWeight(r->topo.edges[2 * i], r->topo.edges[2 * i + 1], w);
    weights[i] = (float)w;
  }
  int res = grRendererSetEdgeWeights(r, weights, edgeCount);
  free(weights);
  return res;
}

/** Uploads topology-derived buffers (node id remap + edge endpoint pairs). */
static int uploadTopology(grRenderer *r) {
  if (grTopologyExtract(&r->topo, *r->graph) < 0)
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

  // Stale per-edge colors/weights no longer match the edge ordering -- this
  // includes the client base layer (edgeColorsStaging), which is indexed
  // the same edge-buffer-order way and is just as stale; the client must
  // re-upload after a structural change, matching grRendererSetEdgeColors's
  // documented indexing contract.
  r->hasEdgeColors = false;
  r->hasEdgeWeights = false;
  free(r->edgeColorsStaging);
  r->edgeColorsStaging = NULL;
  r->edgeColorsStagingCount = 0;
  r->hasClientEdgeColors = false;
  r->colorsDirty = true;
  r->bindGroupDirty = true;
  r->listFilterDirty = true; // visible vertex set changed
  r->listOverlayDirty = true;

  // Auto-maintained attributes: with degree-alpha or weight-width enabled,
  // the data those shader modes index is a pure function of the structure
  // just uploaded, so the renderer refreshes it here instead of every
  // client having to chase structural changes (which made both modes
  // unusable on growing graphs).
  if (r->edgeDegreeAlpha && refreshNodeDegrees(r) < 0)
    return -1;
  if (r->edgeWeightWidth && refreshEdgeWeights(r) < 0)
    return -1;
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
  if (!r->nodeDegreesBuf)
    r->nodeDegreesBuf = createBuffer(r, 4, WGPUBufferUsage_Storage |
                                               WGPUBufferUsage_CopyDst,
                                     "grender node degrees");
  if (!r->edgeWeightsBuf)
    r->edgeWeightsBuf = createBuffer(r, 4, WGPUBufferUsage_Storage |
                                               WGPUBufferUsage_CopyDst,
                                     "grender edge weights");
  if (!r->statsBuf)
    r->statsBuf = createBuffer(r, sizeof(grStatsPrim),
                               WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                               "grender stats prims");

  const WGPUBindGroupEntry entries[10] = {
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
      {.binding = 8, .buffer = r->nodeDegreesBuf,
       .size = storageBindBytes(r, r->nodeDegreesBuf)},
      {.binding = 9, .buffer = r->edgeWeightsBuf,
       .size = storageBindBytes(r, r->edgeWeightsBuf)},
  };
  for (size_t i = 0; i < 10; i++) {
    if (entries[i].size > r->maxStorageBufferBindingSize) {
      GR_LOG("bind group entry %zu size %llu exceeds storage binding limit\n",
             i, (unsigned long long)entries[i].size);
      return -1;
    }
  }
  r->bindGroup = wgpuDeviceCreateBindGroup(
      r->device, grPtr(WGPUBindGroupDescriptor{
                     .label = {"grender bind group", WGPU_STRLEN},
                     .layout = r->bindGroupLayout,
                     .entryCount = 10,
                     .entries = entries,
                 }));
  r->bindGroupDirty = false;
  return r->bindGroup ? 0 : -1;
}

int grRendererSetGraph(grRenderer *r, gviz::layout::EmbeddedGraph &graph,
                       gviz::Graph *backingGraph) {
  size_t dim = graph.Dim();
  if (dim != 2 && dim != 3 && dim != 4) {
    GR_LOG("unsupported embedding dimension %zu (only 2, 3, and 4)\n", dim);
    return -1;
  }

  r->graph = &graph;
  r->backingGraph = backingGraph;
  graph.AddAction(GR_ACTION_PICK_FACE, grenderActionPickFace, r);
  graph.AddAction(GR_ACTION_PICK_VERTEX, grenderActionPickVertex, r);
  r->highlightActive = false;
  r->colorsDirty = false;
  r->pickedVertexId = -1;
  r->vertexOverlayDirty = true;
  r->listSearchFocused = false;
  r->listSearchInput[0] = '\0';
  r->listSearchInputLen = 0;
  r->listScrollPx = 0.0;
  r->listSelectedIdx = SIZE_MAX;
  r->listFilterDirty = true;
  r->listOverlayDirty = true;
  if (dim == 3 || dim == 4)
    grCameraInit3D(&r->camera);
  else
    grCameraInit2D(&r->camera);

  if (ensurePositionBuffers(r) < 0 || uploadTopology(r) < 0)
    return -1;
  r->topoDirty = false;
  r->drawMaskRevision = graph.DrawMaskRevision();
  r->statsSeriesRevisions.clear();
  r->statsOverlayDirty = true;
  r->statsPrims.clear();
  r->pcaBasisValid = false;
  statsVisibilitySync(r);
  r->statsMenuSeriesCount = graph.StatSeriesCount();
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

void grRendererSetEdgeDegreeAlpha(grRenderer *r, bool enabled) {
  if (!r)
    return;
  r->edgeDegreeAlpha = enabled;
  /* Auto-derived data (see uploadTopology): refresh right away so enabling
   * mid-run takes effect this frame, not at the next structural change. */
  if (enabled && r->graph)
    refreshNodeDegrees(r);
}

bool grRendererEdgeDegreeAlpha(const grRenderer *r) {
  return r && r->edgeDegreeAlpha;
}

void grRendererSetEdgeWeightWidth(grRenderer *r, bool enabled) {
  if (!r)
    return;
  r->edgeWeightWidth = enabled;
  if (enabled && r->graph)
    refreshEdgeWeights(r);
}

bool grRendererEdgeWeightWidth(const grRenderer *r) {
  return r && r->edgeWeightWidth;
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

  // This is the persistent client base layer, not the GPU buffer bound for
  // drawing: keep our own copy and let applyColorLayers (colorsDirty)
  // recompute nodeColorsBuf from it (composited with any active highlight)
  // before the next frame, so a highlight set/clear afterward can't stomp
  // it the way overwriting nodeColorsBuf directly here used to.
  if (!rgba8) {
    free(r->nodeColorsStaging);
    r->nodeColorsStaging = NULL;
    r->nodeColorsStagingCount = 0;
    r->hasClientNodeColors = false;
    r->colorsDirty = true;
    return 0;
  }

  uint32_t *staging = (uint32_t *)malloc(count * sizeof(uint32_t));
  if (!staging)
    return -1;
  memcpy(staging, rgba8, count * sizeof(uint32_t));
  free(r->nodeColorsStaging);
  r->nodeColorsStaging = staging;
  r->nodeColorsStagingCount = count;
  r->hasClientNodeColors = true;
  r->colorsDirty = true;
  return 0;
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

  float *staging = (float *)realloc(r->nodeSizesStaging, count * sizeof(float));
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

int grRendererSetNodeDegrees(grRenderer *r, const uint32_t *degrees,
                             size_t count) {
  if (degrees && (!r->graph || count != r->posCapacity))
    return -1;

  if (!degrees) {
    r->maxNodeDegree = 0;
    return uploadAttribute(r, &r->nodeDegreesBuf, NULL, 0, &r->hasNodeDegrees,
                           "grender node degrees");
  }

  uint32_t maxDeg = 1;
  for (size_t i = 0; i < count; i++) {
    if (degrees[i] > maxDeg)
      maxDeg = degrees[i];
  }
  r->maxNodeDegree = maxDeg;
  return uploadAttribute(r, &r->nodeDegreesBuf, degrees,
                         count * sizeof(uint32_t), &r->hasNodeDegrees,
                         "grender node degrees");
}

int grRendererSetVertexLabels(grRenderer *r, const char *const *labels,
                              size_t count) {
  if (labels && (!r->graph || count != r->posCapacity))
    return -1;

  r->vertexLabels = labels;
  r->vertexLabelsCount = labels ? count : 0;
  r->vertexOverlayDirty = true;
  r->listFilterDirty = true; // search text now matches against new labels
  r->listOverlayDirty = true;
  return 0;
}

int grRendererSetEdgeColors(grRenderer *r, const uint32_t *rgba8,
                            size_t count) {
  if (rgba8 && (!r->graph || count != r->topo.edgeCount))
    return -1;

  // Same client-base-layer-vs-composited-buffer split as
  // grRendererSetNodeColors above.
  if (!rgba8) {
    free(r->edgeColorsStaging);
    r->edgeColorsStaging = NULL;
    r->edgeColorsStagingCount = 0;
    r->hasClientEdgeColors = false;
    r->colorsDirty = true;
    return 0;
  }

  uint32_t *staging = (uint32_t *)malloc(count * sizeof(uint32_t));
  if (!staging)
    return -1;
  memcpy(staging, rgba8, count * sizeof(uint32_t));
  free(r->edgeColorsStaging);
  r->edgeColorsStaging = staging;
  r->edgeColorsStagingCount = count;
  r->hasClientEdgeColors = true;
  r->colorsDirty = true;
  return 0;
}

int grRendererSetEdgeWeights(grRenderer *r, const float *weights,
                             size_t count) {
  if (weights && (!r->graph || count != r->topo.edgeCount))
    return -1;

  if (!weights) {
    r->meanEdgeWeight = 0.0f;
    return uploadAttribute(r, &r->edgeWeightsBuf, NULL, 0, &r->hasEdgeWeights,
                           "grender edge weights");
  }

  double sum = 0.0;
  for (size_t i = 0; i < count; i++)
    sum += weights[i];
  r->meanEdgeWeight = count ? (float)(sum / (double)count) : 1.0f;
  if (r->meanEdgeWeight <= 0.0f)
    r->meanEdgeWeight = 1.0f;

  return uploadAttribute(r, &r->edgeWeightsBuf, weights,
                         count * sizeof(float), &r->hasEdgeWeights,
                         "grender edge weights");
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
  for (grKeyBinding &b : r->bindings) {
    if (b.key == key) {
      b.actionName = actionName;
      return 0;
    }
  }
  try {
    r->bindings.push_back({key, actionName});
  } catch (const std::bad_alloc &) {
    return -1;
  }
  return 0;
}

void grRendererUnbindKey(grRenderer *r, int key) {
  for (size_t i = 0; i < r->bindings.size(); i++) {
    if (r->bindings[i].key == key) {
      r->bindings[i] = std::move(r->bindings.back());
      r->bindings.pop_back();
      return;
    }
  }
}

int grRendererBindMouse(grRenderer *r, int button, const char *actionName) {
  for (grMouseBinding &b : r->mouseBindings) {
    if (b.button == button) {
      b.actionName = actionName;
      return 0;
    }
  }
  try {
    r->mouseBindings.push_back({button, actionName});
  } catch (const std::bad_alloc &) {
    return -1;
  }
  return 0;
}

void grRendererUnbindMouse(grRenderer *r, int button) {
  for (size_t i = 0; i < r->mouseBindings.size(); i++) {
    if (r->mouseBindings[i].button == button) {
      r->mouseBindings[i] = std::move(r->mouseBindings.back());
      r->mouseBindings.pop_back();
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
  r->colorsDirty = true; // recompute nodeColorsBuf/edgeColorsBuf without it
}

/**
 * Deep-copies @p src into a fresh full subgraph over @p backingGraph.
 * gviz::Subgraph has no default/null state (it always references a real
 * parent Graph), so a copy that might not happen -- @p backingGraph is NULL,
 * or an internal gviz call throws -- is expressed as std::nullopt rather
 * than the old C code's zeroed `gvizSubgraph{0}` sentinel.
 */
static std::optional<gviz::Subgraph>
grHighlightCopySubgraph(const gviz::Subgraph &src, gviz::Graph *backingGraph) {
  if (!backingGraph)
    return std::nullopt;

  try {
    /* Pick/highlight subgraphs are full subgraphs whose edge bitsets are
     * addressed by the graph's shared layout; refresh it on demand (an O(1)
     * no-op unless the graph mutated) so highlighting keeps working on
     * graphs that grew between clicks. The persisted highlight this copy
     * replaces was written under the previous layout, but it's swapped out
     * before anything reads it under the rebuilt one. */
    backingGraph->EnsureLayout();

    gviz::Subgraph dst = gviz::Subgraph::CreateEmpty(*backingGraph);

    for (size_t u : src)
      dst.ShowVertex(u);

    for (size_t u : src) {
      for (size_t v : src.Neighbors(u)) {
        if (src.HasEdge(u, v))
          dst.ShowEdge(u, v);
      }
    }
    dst.Rebuild();
    return dst;
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

/**
 * Whether the highlight subgraph @p sg marks edge (u, v). For directed
 * topology edges, (u, v) means u -> v specifically, so only that exact
 * direction counts -- otherwise a mutual pair (u -> v and v -> u both
 * existing) would highlight both when only one was ever added to the
 * highlight (e.g. only the picked vertex's outgoing edges). Undirected
 * topology edges are deduplicated as (u, v) with u < v by grTopologyExtract,
 * while the highlight may have stored the same edge from either endpoint, so
 * both directions must be checked there.
 */
static int highlightHasEdge(const gviz::Subgraph *sg, size_t u, size_t v,
                            bool directed) {
  if (sg->HasEdge(u, v))
    return 1;
  if (directed || u == v)
    return 0;
  return sg->HasEdge(v, u);
}

/**
 * Recomputes nodeColorsBuf/edgeColorsBuf -- the GPU buffers actually bound
 * for drawing -- from two layers: the persistent client base layer
 * (nodeColorsStaging/edgeColorsStaging, set via grRendererSetNodeColors/
 * grRendererSetEdgeColors) and the transient active highlight, if any. The
 * highlight is painted *over* the base layer rather than replacing it, so
 * clearing or changing the highlight restores the client's colors instead
 * of falling back to the global style -- that fallback only happens for
 * elements the client never set a color for (or never called
 * grRendererSetNodeColors/SetEdgeColors at all), which is what keeps
 * existing callers' behavior unchanged.
 *
 * Runs once per frame, gated by colorsDirty (set on highlight set/clear,
 * client base-layer changes, and capacity/topology changes) so an unrelated
 * frame does no work.
 */
static void applyColorLayers(grRenderer *r) {
  if (!r->graph || !r->colorsDirty)
    return;

  bool highlightOn = r->highlightActive && r->graph->HasHighlight();
  const gviz::Subgraph *highlight =
      highlightOn ? r->graph->GetHighlight() : NULL;

  // ---- nodes ----
  size_t nodeCount = r->posCapacity;
  bool haveNodeBase = r->hasClientNodeColors && r->nodeColorsStaging &&
                      r->nodeColorsStagingCount == nodeCount;

  if (!highlightOn) {
    // No highlight: the bound buffer is exactly the client's base layer, or
    // cleared to the global style if the client never set one (or cleared
    // it) -- this is what restores base colors after a highlight ends.
    if (haveNodeBase)
      uploadAttribute(r, &r->nodeColorsBuf, r->nodeColorsStaging,
                      nodeCount * sizeof(uint32_t), &r->hasNodeColors,
                      "grender node colors");
    else
      uploadAttribute(r, &r->nodeColorsBuf, NULL, 0, &r->hasNodeColors,
                      "grender node colors");
  } else {
    uint32_t *nodeColors = (uint32_t *)calloc(nodeCount, sizeof(uint32_t));
    if (!nodeColors)
      return;

    uint32_t baseNode = colorToRgba8(&r->nodeStyle.fillColor);
    for (size_t i = 0; i < nodeCount; i++)
      nodeColors[i] = haveNodeBase ? r->nodeColorsStaging[i] : baseNode;

    for (size_t u : *highlight) {
      if (r->highlightNodeRgba)
        nodeColors[u] = r->highlightNodeRgba;
    }

    uploadAttribute(r, &r->nodeColorsBuf, nodeColors,
                    nodeCount * sizeof(uint32_t), &r->hasNodeColors,
                    "grender node colors");
    free(nodeColors);
  }

  // ---- edges ----
  size_t edgeCount = r->topo.edgeCount;
  bool haveEdgeBase = r->hasClientEdgeColors && r->edgeColorsStaging &&
                      r->edgeColorsStagingCount == edgeCount;

  if (!highlightOn) {
    if (haveEdgeBase)
      uploadAttribute(r, &r->edgeColorsBuf, r->edgeColorsStaging,
                      edgeCount * sizeof(uint32_t), &r->hasEdgeColors,
                      "grender edge colors");
    else
      uploadAttribute(r, &r->edgeColorsBuf, NULL, 0, &r->hasEdgeColors,
                      "grender edge colors");
    r->colorsDirty = false;
    return;
  }

  if (edgeCount == 0) {
    r->colorsDirty = false;
    return;
  }

  uint32_t *edgeColors = (uint32_t *)calloc(edgeCount, sizeof(uint32_t));
  if (!edgeColors)
    return;

  uint32_t baseEdge = colorToRgba8(&r->edgeStyle.color);
  for (size_t i = 0; i < edgeCount; i++)
    edgeColors[i] = haveEdgeBase ? r->edgeColorsStaging[i] : baseEdge;

  if (r->highlightEdgeRgba) {
    for (size_t i = 0; i < edgeCount; i++) {
      uint32_t eu = r->topo.edges[i * 2];
      uint32_t ev = r->topo.edges[i * 2 + 1];
      if (highlightHasEdge(highlight, eu, ev, r->topo.directed))
        edgeColors[i] = r->highlightEdgeRgba;
    }
  }

  uploadAttribute(r, &r->edgeColorsBuf, edgeColors,
                  edgeCount * sizeof(uint32_t), &r->hasEdgeColors,
                  "grender edge colors");
  free(edgeColors);
  r->colorsDirty = false;
}

/**
 * Marks edge (u, v) in @p sg exactly as given, without reordering. Undirected
 * edges are mirrored into both endpoints' adjacency lists by gviz, so u->v
 * always resolves regardless of numeric order; directed edges are stored
 * only under their true "from" vertex, so reordering here would silently
 * fail to mark the edge whenever the caller's u happened to be numerically
 * greater than v -- previously this function swapped to enforce u < v, which
 * broke exactly that case for directed graphs (see grenderActionPickVertex,
 * whose u is always the true source since it walks only u's own
 * out-neighbors).
 */
static void highlightShowBoundaryEdge(gviz::Subgraph &sg, size_t u, size_t v) {
  sg.ShowEdge(u, v);
}

int grRendererSetHighlight(grRenderer *r, const gviz::Subgraph &highlight,
                           uint32_t nodeRgba, uint32_t edgeRgba) {
  if (!r || !r->graph)
    return -1;

  std::optional<gviz::Subgraph> copy =
      grHighlightCopySubgraph(highlight, r->backingGraph);
  if (!copy)
    return -1;

  r->graph->SetHighlight(std::move(*copy));
  r->highlightActive = true;
  r->highlightNodeRgba = nodeRgba;
  r->highlightEdgeRgba = edgeRgba;
  r->colorsDirty = true;
  r->listFilterDirty = true; // the list's vertex set follows the highlight
  r->listOverlayDirty = true;
  return 0;
}

int grRendererSetHighlightCycle(grRenderer *r, const size_t *vertices,
                                size_t count, uint32_t nodeRgba,
                                uint32_t edgeRgba) {
  if (!r || !r->graph || !r->backingGraph || !vertices || count < 3)
    return -1;

  try {
    r->backingGraph->EnsureLayout();
    gviz::Subgraph cycle = gviz::Subgraph::CreateEmpty(*r->backingGraph);

    for (size_t i = 0; i < count; i++)
      cycle.ShowVertex(vertices[i]);
    for (size_t i = 0; i < count; i++)
      highlightShowBoundaryEdge(cycle, vertices[i], vertices[(i + 1) % count]);
    cycle.Rebuild();

    return grRendererSetHighlight(r, cycle, nodeRgba, edgeRgba);
  } catch (const std::exception &) {
    return -1;
  }
}

void grRendererClearHighlight(grRenderer *r) {
  if (!r)
    return;
  if (r->graph)
    r->graph->ClearHighlight();
  // grHighlightReset marks colorsDirty so applyColorLayers repaints
  // nodeColorsBuf/edgeColorsBuf from the client base layer alone (or the
  // global style, if none was set) -- it must NOT touch the client's base
  // layer itself (nodeColorsStaging/edgeColorsStaging), which persists
  // across highlight clears per grRendererSetNodeColors's contract.
  grHighlightReset(r);
  r->listFilterDirty = true; // the list's vertex set follows the highlight
  r->listOverlayDirty = true;
}

static void grenderActionPickFace(gviz::layout::EmbeddedGraph &eg,
                                  void *userData,
                                  const gviz::layout::ActionPayload &payload) {
  grRenderer *r = (grRenderer *)userData;
  if (!r || !r->backingGraph)
    return;

  std::optional<gviz::Subgraph> face = gviz::layout::FaceSubgraphAt(
      *r->backingGraph, eg, payload.worldX, payload.worldY);
  if (!face) {
    grRendererClearHighlight(r);
    return;
  }

  grRendererSetHighlight(r, *face, GR_RGBA8(255, 210, 80, 255),
                         GR_RGBA8(255, 180, 40, 255));
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

/**
 * Finds the vertex whose drawn circle contains a click at (@p worldX, @p
 * worldY) on embedded graph @p eg, preferring the most centrally-contained
 * one when circles overlap (smaller vertices sitting on/near a larger one
 * must still be selectable, so this can't just take the nearest center and
 * test that one vertex's radius alone -- per-vertex sizes vary, so the
 * nearest center isn't necessarily the vertex whose circle actually reaches
 * the click). Shared by grenderActionPickVertex and the vertex-click action
 * dispatch (grRendererBindVertexClick) so both agree on what counts as
 * "clicked". Returns false (leaving *outVertex untouched) when no vertex's
 * circle contains the click.
 */
static bool grHitTestVertex(grRenderer *r, gviz::layout::EmbeddedGraph &eg,
                            double worldX, double worldY,
                            size_t *outVertex) {
  size_t dim = eg.Dim();
  const double *pos = eg.Positions().data();

  size_t best = SIZE_MAX;
  double bestRatio2 = 0.0;
  for (size_t i = 0; i < r->topo.nodeCount; i++) {
    uint32_t v = r->topo.nodeIds[i];
    const double *p = pos + (size_t)v * dim;
    double dx = p[0] - worldX;
    double dy = p[1] - worldY;
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
  if (best == SIZE_MAX)
    return false;
  *outVertex = best;
  return true;
}

static void grenderActionPickVertex(gviz::layout::EmbeddedGraph &eg,
                                    void *userData,
                                    const gviz::layout::ActionPayload &payload) {
  grRenderer *r = (grRenderer *)userData;
  if (!r || r->topo.nodeCount == 0)
    return;

  size_t nearest;
  if (!grHitTestVertex(r, eg, payload.worldX, payload.worldY, &nearest)) {
    grRendererClearHighlight(r);
    if (r->pickedVertexId != -1) {
      r->pickedVertexId = -1;
      r->vertexOverlayDirty = true;
    }
    return;
  }

  if (r->pickedVertexId != (int64_t)nearest) {
    r->pickedVertexId = (int64_t)nearest;
    r->vertexOverlayScrollPx = 0.0;
    r->vertexOverlayDirty = true;
  }

  // The highlight below needs raw parent-graph access (EnsureLayout, and the
  // fallback neighbor iteration) that gviz::Subgraph deliberately never
  // exposes -- see grRendererSetGraph's doc comment. Without a backingGraph,
  // the picked-vertex state above still updates (so the vertex-info panel
  // still works), but no highlight is drawn.
  if (!r->backingGraph)
    return;

  gviz::Subgraph &structure = eg.Structure();
  try {
    r->backingGraph->EnsureLayout();
    gviz::Subgraph pick = gviz::Subgraph::CreateEmpty(*r->backingGraph);

    pick.ShowVertex(nearest);

    // Shift-click flips the highlight to in-edges (nearest's predecessors)
    // instead of the default out-edges, for directed graphs only -- shift is
    // a no-op on undirected graphs, where every edge already appears both
    // ways in the out rows. Both branches read the embedding's SYNCED
    // adjacency snapshot (EmbeddedGraph::OutNeighbors/InNeighbors), the same
    // structure grTopologyExtract draws edges from, so the highlight can
    // never include an edge that isn't on screen. Embeddings that never
    // synced have no snapshot (the accessors return an empty span); there
    // the live subgraph is the committed structure and out-edges fall back
    // to neighbor iteration, while in-edges have no source at all and
    // shift-click degrades to highlighting just the vertex.
    if (structure.ParentIsDirected() && (payload.iarg & GR_MOD_SHIFT) != 0) {
      for (size_t u : eg.InNeighbors(nearest)) {
        pick.ShowVertex(u);
        highlightShowBoundaryEdge(pick, u, nearest); // edge is u -> nearest
      }
    } else {
      std::span<const size_t> outNbrs = eg.OutNeighbors(nearest);
      if (!outNbrs.empty()) {
        for (size_t v : outNbrs) {
          pick.ShowVertex(v);
          highlightShowBoundaryEdge(pick, nearest, v);
        }
      } else {
        for (size_t v : structure.Neighbors(nearest)) {
          pick.ShowVertex(v);
          highlightShowBoundaryEdge(pick, nearest, v);
        }
      }
    }
    pick.Rebuild();

    grRendererSetHighlight(r, pick, GR_RGBA8(255, 210, 80, 255),
                           GR_RGBA8(255, 180, 40, 255));
  } catch (const std::exception &) {
    // Best-effort highlight; the picked-vertex state above already landed.
  }
}

void grRendererFitView(grRenderer *r) { r->fitRequested = true; }

void grRendererRequestClose(grRenderer *r) { r->closeRequested = true; }

double grRendererDeltaTime(const grRenderer *r) { return r->deltaTime; }

/**
 * Computes the axis-aligned bounding box (in the coordinates actually
 * rendered -- PCA-projected to 3D for a 4D embedding) of the topology's
 * currently visible vertices. Returns false (bmin/bmax left untouched) when
 * there's nothing to bound (no graph, no visible vertices, or a 4D-PCA
 * allocation failure). Shared by fitViewNow (F key / grRendererFitView) and
 * grRendererFocusVertex, so both agree on what "the graph's extent" means
 * for an embedding of any supported dimension.
 */
static bool computeVisibleBoundingBox(grRenderer *r, double bmin[3],
                                      double bmax[3]) {
  if (!r->graph || r->topo.nodeCount == 0)
    return false;

  const double *pos = r->graph->Positions().data();
  size_t srcDim = r->srcDim;
  bmin[0] = INFINITY, bmin[1] = INFINITY, bmin[2] = 0.0;
  bmax[0] = -INFINITY, bmax[1] = -INFINITY, bmax[2] = 0.0;
  if (r->posDim == 3)
    bmin[2] = INFINITY, bmax[2] = -INFINITY;

  if (srcDim == 4) {
    float *proj = (float *)malloc(sizeof(float) * r->posCapacity * 3);
    if (!proj)
      return false;
    if (grPCAProjectTo3(pos, r->posCapacity, srcDim, proj, r->pcaBasis,
                        r->pcaBasisValid ? r->pcaBasis : NULL) < 0) {
      free(proj);
      return false;
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
  return true;
}

static void fitViewNow(grRenderer *r, double fbw, double fbh) {
  double bmin[3], bmax[3];
  if (!computeVisibleBoundingBox(r, bmin, bmax))
    return;
  grCameraFitBox(&r->camera, bmin, bmax, fbw, fbh);
}

// Fraction of the graph's own max extent used to size the "focus" box in
// grRendererFocusVertex: small enough to feel like a deliberate zoom-in on
// one vertex (rather than grCameraFitBox's zero-size box for a lone point,
// which would zoom to the camera's minimum distance and show nothing useful
// around it), while still scaling with the graph's own coordinate units
// instead of an arbitrary absolute distance that would be wrong for a graph
// laid out in [-1, 1] versus one laid out in the thousands.
#define GR_FOCUS_EXTENT_FRACTION 0.08
#define GR_FOCUS_FALLBACK_RADIUS 1.0

/**
 * Centers the camera on vertex @p vertexId and zooms in to comfortably frame
 * a neighborhood around it, sized relative to the graph's current bounding
 * box (see GR_FOCUS_EXTENT_FRACTION) via grCameraFitBox -- shared by the C
 * key (see processInput below) and grConsole.c's "find" command so both
 * behave identically. No-op if @p vertexId is out of range or no vertex
 * position data has been uploaded yet.
 */
void grRendererFocusVertex(grRenderer *r, size_t vertexId) {
  if (!r || !r->posStaging || vertexId >= r->posCapacity)
    return;

  double point[3] = {0.0, 0.0, 0.0};
  for (size_t d = 0; d < r->posDim && d < 3; d++)
    point[d] = (double)r->posStaging[vertexId * r->posDim + d];

  double bmin[3], bmax[3], maxExtent = 0.0;
  if (computeVisibleBoundingBox(r, bmin, bmax)) {
    for (int d = 0; d < 3; d++) {
      double extent = bmax[d] - bmin[d];
      if (extent > maxExtent)
        maxExtent = extent;
    }
  }
  double radius = maxExtent > 0.0 ? maxExtent * GR_FOCUS_EXTENT_FRACTION
                                  : GR_FOCUS_FALLBACK_RADIUS;

  double focusMin[3], focusMax[3];
  for (int d = 0; d < 3; d++) {
    focusMin[d] = point[d] - radius;
    focusMax[d] = point[d] + radius;
  }
  grCameraFitBox(&r->camera, focusMin, focusMax, r->surfaceConfig.width,
                r->surfaceConfig.height);
}

/** Applies built-in navigation and queues action dispatches. */
static void processInput(grRenderer *r, double fbw, double fbh) {
  r->viewportHeightPx = fbh;

  // While the console is open it owns all keyboard/mouse input: typed keys
  // and characters go to the input line (grConsoleProcessInput), and camera
  // navigation / action dispatch below never run, so e.g. typing "find" does
  // not also fit the view (F) or orbit the camera. Clicks that land while
  // the console has focus are discarded rather than queued for later, since
  // by the time the console closes they no longer reflect the cursor's
  // current intent.
  if (r->consoleOpen) {
    grConsoleProcessInput(r);
    r->pendingKeys.clear(); // don't replay this frame's keys as actions
    r->pendingMouse.clear();
    r->draggingPan = false;
    r->draggingOrbit = false;
    grCameraFrameCompute(&r->camera, fbw, fbh, &r->cameraFrame);
    return;
  }
  // The vertex-list search box is the console's only other user of this
  // queue; the two are mutually exclusive (the branch above already
  // returned if the console owns input), so it's safe to route the whole
  // queue to whichever one currently has focus, or discard it if neither does.
  if (r->listSearchFocused)
    grListSearchProcessInput(r);
  else
    r->pendingConsoleEvents.clear();

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

  grVertexOverlayLayout vertexOverlayLayout;
  grVertexOverlayComputeLayout(r, fbw, fbh, &vertexOverlayLayout);
  bool overVertexOverlay =
      vertexOverlayLayout.visible && cxPx >= vertexOverlayLayout.x0 &&
      cxPx <= vertexOverlayLayout.x1 && cyPx >= vertexOverlayLayout.y0 &&
      cyPx <= vertexOverlayLayout.y1;

  if (overVertexOverlay && r->scrollAccum != 0.0) {
    double newScroll = r->vertexOverlayScrollPx +
                       r->scrollAccum * vertexOverlayLayout.lineH * 3.0;
    if (newScroll < 0.0)
      newScroll = 0.0;
    if (newScroll > vertexOverlayLayout.maxScrollPx)
      newScroll = vertexOverlayLayout.maxScrollPx;
    if (newScroll != r->vertexOverlayScrollPx) {
      r->vertexOverlayScrollPx = newScroll;
      r->vertexOverlayDirty = true;
    }
    r->scrollAccum = 0.0;
  }

  grListOverlayLayout listOverlayLayout;
  grListOverlayComputeLayout(r, fbw, fbh, &listOverlayLayout);
  bool overListOverlay =
      listOverlayLayout.visible && cxPx >= listOverlayLayout.x0 &&
      cxPx <= listOverlayLayout.x1 && cyPx >= listOverlayLayout.y0 &&
      cyPx <= listOverlayLayout.y1;

  if (overListOverlay && r->scrollAccum != 0.0) {
    double newScroll =
        r->listScrollPx + r->scrollAccum * listOverlayLayout.lineH * 3.0;
    if (newScroll < 0.0)
      newScroll = 0.0;
    if (newScroll > listOverlayLayout.maxScrollPx)
      newScroll = listOverlayLayout.maxScrollPx;
    if (newScroll != r->listScrollPx) {
      r->listScrollPx = newScroll;
      r->listOverlayDirty = true;
    }
    r->scrollAccum = 0.0;
  }

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

  // Key dispatch: built-in fit on F unless the app bound F itself. Most of
  // this is suppressed while the list's search box has focus, so typing e.g.
  // "field" into it doesn't also fit the view (F), toggle stats (S), or fire
  // any app-bound action -- exactly like the console's own exclusive input
  // capture, just scoped to key dispatch rather than all input. Up/Down list
  // navigation is the one exception: arrow keys never appear in typed text,
  // so they stay live even while the search box is focused.
  {
    const grPendingKey *pendingKeys = r->pendingKeys.data();
    const grKeyBinding *bindings = r->bindings.data();
    for (size_t k = 0; k < r->pendingKeys.size(); k++) {
      int key = pendingKeys[k].key;
      int mods = pendingKeys[k].mods;

      const char *actionName = NULL;
      for (size_t i = 0; i < r->bindings.size(); i++) {
        if (bindings[i].key == key) {
          actionName = bindings[i].actionName;
          break;
        }
      }

      if (!actionName) {
        if (r->listVisible && key == GR_KEY_UP) {
          grListOverlaySelectDelta(r, fbw, fbh, -1);
          continue;
        }
        if (r->listVisible && key == GR_KEY_DOWN) {
          grListOverlaySelectDelta(r, fbw, fbh, 1);
          continue;
        }
        if (r->listSearchFocused)
          continue; // F/S/I/L/C/console-toggle stay suppressed while typing
        if (key == 'F')
          r->fitRequested = true;
        else if (key == 'S')
          grRendererShowStats(r, !r->statsVisible);
        else if (key == 'I')
          grRendererShowTextureMapImage(r, !grRendererTextureMapImageShown(r));
        else if (key == 'L')
          grRendererShowVertexList(r, !r->listVisible);
        else if (key == 'C' && r->listSelectedIdx < r->listFilteredIds.size()) {
          const uint32_t *ids = r->listFilteredIds.data();
          grRendererFocusVertex(r, ids[r->listSelectedIdx]);
        } else if (key == GR_CONSOLE_TOGGLE_KEY)
          grConsoleOpen(r);
        continue;
      }
      if (r->listSearchFocused)
        continue; // app-bound actions stay suppressed while typing too
      if (!r->graph)
        continue;

      gviz::layout::ActionPayload payload{};
      grCameraUnproject(&r->camera, &r->cameraFrame, cxPx, cyPx, fbw, fbh,
                        &payload.worldX, &payload.worldY);
      payload.deltaTime = r->deltaTime;
      payload.iarg = mods; // GLFW mod bits match GR_MOD_*
      r->graph->InvokeAction(actionName, &payload);
    }
  }
  r->pendingKeys.clear();

  const grPendingMouse *pendingMouse = r->pendingMouse.data();
  const grMouseBinding *mouseBindings = r->mouseBindings.data();
  for (size_t m = 0; m < r->pendingMouse.size(); m++) {
    int button = pendingMouse[m].button;
    int mods = pendingMouse[m].mods;

    // Clicks landing on the list panel never reach the graph: inside the
    // search bar they focus it (so subsequent keys type into the query
    // instead of dispatching), anywhere else in the panel they just blur it.
    // A click outside the panel blurs a focused search box too, but then
    // falls through to dispatch normally (e.g. still picks a vertex).
    if (listOverlayLayout.visible &&
        pendingMouse[m].xPx >= listOverlayLayout.x0 &&
        pendingMouse[m].xPx <= listOverlayLayout.x1 &&
        pendingMouse[m].yPx >= listOverlayLayout.y0 &&
        pendingMouse[m].yPx <= listOverlayLayout.y1) {
      r->listSearchFocused =
          pendingMouse[m].yPx >= listOverlayLayout.searchY0 &&
          pendingMouse[m].yPx <= listOverlayLayout.searchY1;
      continue;
    }
    if (r->listSearchFocused)
      r->listSearchFocused = false;

    const char *actionName = NULL;
    for (size_t i = 0; i < r->mouseBindings.size(); i++) {
      if (mouseBindings[i].button == button) {
        actionName = mouseBindings[i].actionName;
        break;
      }
    }
    if (!actionName && button == GR_MOUSE_BUTTON_LEFT)
      actionName = GR_ACTION_PICK_VERTEX;
    if (!r->graph)
      continue;
    // GR_ACTION_VERTEX_CLICKED has no binding to check -- it fires purely
    // off whether the creator registered a handler for it, so look that up
    // instead of unconditionally paying for the O(vertex count) hit test
    // below on every click of every app that doesn't use it.
    bool wantsVertexClicked =
        r->graph->FindAction(GR_ACTION_VERTEX_CLICKED) != nullptr;
    if (!actionName && !wantsVertexClicked)
      continue;

    gviz::layout::ActionPayload payload{};
    grCameraUnproject(&r->camera, &r->cameraFrame, pendingMouse[m].xPx,
                      pendingMouse[m].yPx, fbw, fbh, &payload.worldX,
                      &payload.worldY);
    payload.deltaTime = r->deltaTime;
    payload.iarg = mods;
    if (actionName)
      r->graph->InvokeAction(actionName, &payload);

    // GR_ACTION_VERTEX_CLICKED dispatch is independent of, and in addition
    // to, the per-button action above: it fires on any button whose click
    // actually lands on a vertex's drawn circle (grHitTestVertex, the same
    // test GR_ACTION_PICK_VERTEX uses), carrying the hit vertex's id in iarg
    // rather than the modifier bits regular key/mouse actions get there.
    if (wantsVertexClicked) {
      size_t hitVertex;
      if (grHitTestVertex(r, *r->graph, payload.worldX, payload.worldY,
                          &hitVertex)) {
        gviz::layout::ActionPayload vertexPayload = payload;
        vertexPayload.iarg = (int64_t)hitVertex;
        r->graph->InvokeAction(GR_ACTION_VERTEX_CLICKED, &vertexPayload);
      }
    }
  }
  r->pendingMouse.clear();
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
            (r->hasEdgeColors ? 4u : 0u) |
            ((r->edgeDegreeAlpha && r->hasNodeDegrees) ? 8u : 0u) |
            ((r->edgeWeightWidth && r->hasEdgeWeights) ? 16u : 0u) |
            (r->topo.directed ? 32u : 0u);

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
  g.edgeParams[2] = (float)(r->maxNodeDegree ? r->maxNodeDegree : 1u);
  g.edgeParams[3] = r->meanEdgeWeight > 0.0f ? r->meanEdgeWeight : 1.0f;

  wgpuQueueWriteBuffer(r->queue, r->globalsBuf, 0, &g, sizeof(g));
}

static void uploadPositions(grRenderer *r) {
  const double *src = r->graph->Positions().data();
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
  size_t n = r->graph->StatSeriesCount();
  r->statsSeriesRevisions.clear();
  for (size_t i = 0; i < n; i++) {
    const gviz::layout::StatSeries *series = r->graph->StatSeriesAt(i);
    uint64_t rev = series ? series->revision : 0;
    r->statsSeriesRevisions.push_back(rev);
  }
  r->statsLayoutFbw = fbw;
  r->statsLayoutFbh = fbh;
  r->statsLayoutScale = r->contentScale > 0.0 ? r->contentScale : 1.0;
  r->statsOverlayDirty = false;
}

static bool statsOverlayNeedsRebuild(grRenderer *r, double fbw, double fbh) {
  if (r->statsOverlayDirty || r->vertexOverlayDirty || r->listOverlayDirty)
    return true;
  // The console has no revision counter like the stats/vertex panels do --
  // its text changes on every keystroke -- so just rebuild every frame it's
  // open. The primitive list is tiny (a couple of rects and two short text
  // lines), so this is cheap.
  if (r->consoleOpen)
    return true;
  double scale = r->contentScale > 0.0 ? r->contentScale : 1.0;
  if (fbw != r->statsLayoutFbw || fbh != r->statsLayoutFbh ||
      scale != r->statsLayoutScale)
    return true;
  if (!r->statsVisible)
    return false;
  size_t n = r->graph->StatSeriesCount();
  if (n != r->statsSeriesRevisions.size())
    return true;
  const uint64_t *cached = r->statsSeriesRevisions.data();
  for (size_t i = 0; i < n; i++) {
    const gviz::layout::StatSeries *series = r->graph->StatSeriesAt(i);
    if (!series)
      continue;
    if (series->revision != cached[i])
      return true;
  }
  return false;
}

/** Rebuilds overlay primitives (stat charts if shown, the vertex-info panel,
 *  the vertex-list panel if shown, and the command console if open) when
 *  stat data, the picked vertex, the list's filter, console state, or layout
 *  changed; uploads (grow-only buffer). */
static void uploadStats(grRenderer *r, double fbw, double fbh) {
  if (!statsOverlayNeedsRebuild(r, fbw, fbh))
    return;

  r->statsPrims.clear();
  if (r->statsVisible)
    grStatsOverlayBuild(r, fbw, fbh);
  grVertexOverlayBuild(r, fbw, fbh);
  grListOverlayBuild(r, fbw, fbh);
  grConsoleBuild(r, fbw, fbh);
  statsRevisionCacheSync(r, fbw, fbh);
  r->vertexOverlayDirty = false;
  if (r->statsPrims.empty())
    return;

  if (r->statsPrims.size() > r->statsBufCapacity) {
    GR_RELEASE(wgpuBufferRelease, r->statsBuf);
    r->statsBufCapacity = r->statsPrims.size() * 2;
    r->statsBuf = createBuffer(
        r, sizeof(grStatsPrim) * r->statsBufCapacity,
        WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
        "grender stats prims");
    r->bindGroupDirty = true;
    if (!r->statsBuf) {
      r->statsPrims.clear();
      return;
    }
  }
  wgpuQueueWriteBuffer(r->queue, r->statsBuf, 0, r->statsPrims.data(),
                       sizeof(grStatsPrim) * r->statsPrims.size());
}

/** Encodes the scene render pass (clear + edges + nodes) into @p target. */
static void encodeScenePass(grRenderer *r, WGPUCommandEncoder encoder,
                            WGPUTextureView target, WGPUTextureView depth) {
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(
      encoder,
      grPtr(WGPURenderPassDescriptor{
          .colorAttachmentCount = 1,
          .colorAttachments =
              grPtr(WGPURenderPassColorAttachment{
                  .view = target,
                  .loadOp = WGPULoadOp_Clear,
                  .storeOp = WGPUStoreOp_Store,
                  .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
                  .clearValue = {r->clearColor.r, r->clearColor.g,
                                 r->clearColor.b, r->clearColor.a},
              }),
          .depthStencilAttachment =
              grPtr(WGPURenderPassDepthStencilAttachment{
                  .view = depth,
                  .depthLoadOp = WGPULoadOp_Clear,
                  .depthStoreOp = WGPUStoreOp_Store,
                  .depthClearValue = 1.0f,
              }),
      }));

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
          // Undirected edges are a 6-vertex quad (2 triangles); directed
          // edges append a 3-vertex arrowhead triangle at v (vertices 6-8
          // in vsEdge), so the same instance draws both without a second
          // draw call.
          uint32_t verticesPerEdge = r->topo.directed ? 9 : 6;
          wgpuRenderPassEncoderSetPipeline(pass, r->edgePipeline);
          wgpuRenderPassEncoderDraw(pass, verticesPerEdge,
                                    (uint32_t)r->topo.edgeCount, 0, 0);
        }
      }
    }

    // Stats charts and the vertex-info panel always draw on top of the scene.
    if (!r->statsPrims.empty()) {
      wgpuRenderPassEncoderSetPipeline(pass, r->statsPipeline);
      wgpuRenderPassEncoderDraw(pass, 6, (uint32_t)r->statsPrims.size(), 0, 0);
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
      r->device, grPtr(WGPUTextureDescriptor{
                     .label = {"grender screenshot", WGPU_STRLEN},
                     .usage = WGPUTextureUsage_RenderAttachment |
                              WGPUTextureUsage_CopySrc,
                     .dimension = WGPUTextureDimension_2D,
                     .size = {w, h, 1},
                     .format = r->surfaceFormat,
                     .mipLevelCount = 1,
                     .sampleCount = 1,
                 }));
  if (!target)
    return -1;
  WGPUTextureView targetView = wgpuTextureCreateView(target, NULL);

  const uint32_t bytesPerRow = (w * 4 + 255) & ~255u; // 256-byte alignment
  WGPUBuffer readback = wgpuDeviceCreateBuffer(
      r->device, grPtr(WGPUBufferDescriptor{
                     .label = {"grender readback", WGPU_STRLEN},
                     .size = (uint64_t)bytesPerRow * h,
                     .usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead,
                 }));

  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(r->device, NULL);
  encodeScenePass(r, encoder, targetView, r->depthView);
  grObjOverlayEncode(r, encoder, targetView, r->depthView, w, h);
  wgpuCommandEncoderCopyTextureToBuffer(
      encoder,
      grPtr(WGPUTexelCopyTextureInfo{.texture = target}),
      grPtr(WGPUTexelCopyBufferInfo{
          .layout = {.bytesPerRow = bytesPerRow, .rowsPerImage = h},
          .buffer = readback,
      }),
      grPtr(WGPUExtent3D{w, h, 1}));
  WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, NULL);
  wgpuQueueSubmit(r->queue, 1, &commands);
  wgpuCommandBufferRelease(commands);
  wgpuCommandEncoderRelease(encoder);

  WGPUMapAsyncStatus mapStatus = WGPUMapAsyncStatus_Error;
#ifdef __EMSCRIPTEN__
  // See the adapter/device request in grRendererCreate: browser WebGPU needs
  // WaitAnyOnly + an explicit wait instead of wgpu-native's wgpuDevicePoll.
  WGPUFuture mapFuture = wgpuBufferMapAsync(
      readback, WGPUMapMode_Read, 0, (size_t)bytesPerRow * h,
      (const WGPUBufferMapCallbackInfo){.mode = WGPUCallbackMode_WaitAnyOnly,
                                        .callback = onScreenshotMap,
                                        .userdata1 = &mapStatus});
  WGPUFutureWaitInfo mapWait = {.future = mapFuture};
  wgpuInstanceWaitAny(r->instance, 1, &mapWait, UINT64_MAX);
#else
  wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0,
                     (size_t)bytesPerRow * h,
                     (const WGPUBufferMapCallbackInfo){
                         .callback = onScreenshotMap,
                         .userdata1 = &mapStatus,
                     });
  wgpuDevicePoll(r->device, true, NULL);
#endif

  int result = -1;
  if (mapStatus == WGPUMapAsyncStatus_Success) {
    const uint8_t *data = (const uint8_t *)wgpuBufferGetConstMappedRange(
        readback, 0, (size_t)bytesPerRow * h);
    FILE *f = data ? fopen(path, "wb") : NULL;
    if (f) {
      // Surface formats are 8-bit RGBA or BGRA; swizzle BGRA on write.
      bool bgra = r->surfaceFormat == WGPUTextureFormat_BGRA8Unorm ||
                  r->surfaceFormat == WGPUTextureFormat_BGRA8UnormSrgb;
      fprintf(f, "P6\n%u %u\n255\n", w, h);
      uint8_t *row = (uint8_t *)malloc((size_t)w * 3);
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
    uint64_t rev = r->graph->DrawMaskRevision();
    if (rev != r->drawMaskRevision) {
      r->drawMaskRevision = rev;
      r->topoDirty = true;
    }
    if (r->topoDirty) {
      if (ensurePositionBuffers(r) < 0 || uploadTopology(r) < 0)
        return false;
      r->topoDirty = false;
    }
    applyColorLayers(r);
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
      grPtr(WGPUCommandEncoderDescriptor{.label = {"grender", WGPU_STRLEN}}));

  encodeScenePass(r, encoder, frame, r->depthView);
  grObjOverlayEncode(r, encoder, frame, r->depthView, fbw, fbh);

  WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, NULL);
  wgpuQueueSubmit(r->queue, 1, &commands);
#ifndef __EMSCRIPTEN__
  // Emdawnwebgpu has no wgpuSurfacePresent: the browser presents the canvas
  // automatically once control returns to its requestAnimationFrame loop
  // (see the #ifdef __EMSCRIPTEN__ branch in each example's main loop).
  wgpuSurfacePresent(r->surface);
#endif

  wgpuCommandBufferRelease(commands);
  wgpuCommandEncoderRelease(encoder);
  wgpuTextureViewRelease(frame);
  wgpuTextureRelease(surfaceTexture.texture);
  GR_PROF_FRAME_END();
  return true;
}
