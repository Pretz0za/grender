/**
 * Object overlay: a small picture-in-picture panel that shows a loaded
 * Wavefront .obj mesh with its own orbiting camera, drawn in a second render
 * pass restricted (via viewport + scissor) to the bottom-left corner. It
 * never reads keyboard or mouse input and its camera is entirely separate
 * from grRenderer::camera, so normal navigation keeps controlling only the
 * main scene.
 */
#include "grInternal.h"
#include "grShaders.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GR_LOG(...) fprintf(stderr, "[grender] " __VA_ARGS__)

#define GR_OBJ_PANEL_SIZE 400.0
#define GR_OBJ_PANEL_MARGIN 12.0
#define GR_OBJ_SPIN_RADIANS_PER_SEC 0.6

#define GR_OBJ_RELEASE(fn, x)                                                \
  do {                                                                       \
    if (x) {                                                                 \
      fn(x);                                                                 \
      (x) = NULL;                                                            \
    }                                                                        \
  } while (0)

WGPUBuffer grMakeStorageBuffer(grRenderer *r, const void *data, size_t bytes,
                               const char *label) {
  size_t bufSize = bytes < 4 ? 4 : (bytes + 3) & ~(size_t)3;
  WGPUBuffer buf = wgpuDeviceCreateBuffer(
      r->device, &(const WGPUBufferDescriptor){
                     .label = {label, WGPU_STRLEN},
                     .size = bufSize,
                     .usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                 });
  if (buf && data && bytes)
    wgpuQueueWriteBuffer(r->queue, buf, 0, data, bytes);
  return buf;
}

static int createObjPipelines(grRenderer *r) {
  grObjOverlay *ov = &r->objOverlay;

  ov->shaderModule = wgpuDeviceCreateShaderModule(
      r->device,
      &(const WGPUShaderModuleDescriptor){
          .label = {"grender obj shaders", WGPU_STRLEN},
          .nextInChain =
              (WGPUChainedStruct *)&(WGPUShaderSourceWGSL){
                  .chain = {.sType = WGPUSType_ShaderSourceWGSL},
                  .code = {GR_WGSL_OBJ_SOURCE, WGPU_STRLEN},
              },
      });
  if (!ov->shaderModule)
    return -1;

  // 0: uniform. 1-4: read-only vertex storage (positions/normals/indices/uv).
  // 5-6: fragment sampler + texture (image; whitening is a per-fragment UV
  // bounds check in fsObj, no per-triangle validity buffer needed).
  WGPUBindGroupLayoutEntry entries[7] = {0};
  entries[0] = (WGPUBindGroupLayoutEntry){
      .binding = 0,
      .visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment,
      .buffer = {.type = WGPUBufferBindingType_Uniform},
  };
  for (int i = 1; i < 5; i++) {
    entries[i] = (WGPUBindGroupLayoutEntry){
        .binding = (uint32_t)i,
        .visibility = WGPUShaderStage_Vertex,
        .buffer = {.type = WGPUBufferBindingType_ReadOnlyStorage},
    };
  }
  entries[5] = (WGPUBindGroupLayoutEntry){
      .binding = 5,
      .visibility = WGPUShaderStage_Fragment,
      .sampler = {.type = WGPUSamplerBindingType_Filtering},
  };
  entries[6] = (WGPUBindGroupLayoutEntry){
      .binding = 6,
      .visibility = WGPUShaderStage_Fragment,
      .texture = {.sampleType = WGPUTextureSampleType_Float,
                  .viewDimension = WGPUTextureViewDimension_2D,
                  .multisampled = false},
  };

  ov->bindGroupLayout = wgpuDeviceCreateBindGroupLayout(
      r->device, &(const WGPUBindGroupLayoutDescriptor){
                     .label = {"grender obj bgl", WGPU_STRLEN},
                     .entryCount = 7,
                     .entries = entries,
                 });
  ov->pipelineLayout = wgpuDeviceCreatePipelineLayout(
      r->device, &(const WGPUPipelineLayoutDescriptor){
                     .label = {"grender obj layout", WGPU_STRLEN},
                     .bindGroupLayoutCount = 1,
                     .bindGroupLayouts =
                         (const WGPUBindGroupLayout[]){ov->bindGroupLayout},
                 });
  if (!ov->bindGroupLayout || !ov->pipelineLayout)
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

  const WGPUDepthStencilState bgDepth = {
      .format = WGPUTextureFormat_Depth24Plus,
      .depthWriteEnabled = WGPUOptionalBool_False,
      .depthCompare = WGPUCompareFunction_Always,
      .stencilFront = {.compare = WGPUCompareFunction_Always},
      .stencilBack = {.compare = WGPUCompareFunction_Always},
      .stencilReadMask = 0xFFFFFFFF,
      .stencilWriteMask = 0xFFFFFFFF,
  };
  ov->bgPipeline = wgpuDeviceCreateRenderPipeline(
      r->device,
      &(const WGPURenderPipelineDescriptor){
          .label = {"grender obj bg", WGPU_STRLEN},
          .layout = ov->pipelineLayout,
          .vertex = {.module = ov->shaderModule,
                     .entryPoint = {"vsObjBg", WGPU_STRLEN}},
          .fragment =
              &(const WGPUFragmentState){
                  .module = ov->shaderModule,
                  .entryPoint = {"fsObjBg", WGPU_STRLEN},
                  .targetCount = 1,
                  .targets = &colorTarget,
              },
          .primitive = {.topology = WGPUPrimitiveTopology_TriangleList,
                        .cullMode = WGPUCullMode_None},
          .depthStencil = &bgDepth,
          .multisample = {.count = 1, .mask = 0xFFFFFFFF},
      });

  const WGPUDepthStencilState meshDepth = {
      .format = WGPUTextureFormat_Depth24Plus,
      .depthWriteEnabled = WGPUOptionalBool_True,
      .depthCompare = WGPUCompareFunction_LessEqual,
      .stencilFront = {.compare = WGPUCompareFunction_Always},
      .stencilBack = {.compare = WGPUCompareFunction_Always},
      .stencilReadMask = 0xFFFFFFFF,
      .stencilWriteMask = 0xFFFFFFFF,
  };
  ov->meshPipeline = wgpuDeviceCreateRenderPipeline(
      r->device,
      &(const WGPURenderPipelineDescriptor){
          .label = {"grender obj mesh", WGPU_STRLEN},
          .layout = ov->pipelineLayout,
          .vertex = {.module = ov->shaderModule,
                     .entryPoint = {"vsObj", WGPU_STRLEN}},
          .fragment =
              &(const WGPUFragmentState){
                  .module = ov->shaderModule,
                  .entryPoint = {"fsObj", WGPU_STRLEN},
                  .targetCount = 1,
                  .targets = &colorTarget,
              },
          // Arbitrary/unknown winding from input files: don't cull faces.
          .primitive = {.topology = WGPUPrimitiveTopology_TriangleList,
                        .cullMode = WGPUCullMode_None},
          .depthStencil = &meshDepth,
          .multisample = {.count = 1, .mask = 0xFFFFFFFF},
      });

  if (!ov->bgPipeline || !ov->meshPipeline)
    return -1;

  ov->uniformBuf = wgpuDeviceCreateBuffer(
      r->device, &(const WGPUBufferDescriptor){
                     .label = {"grender obj globals", WGPU_STRLEN},
                     .size = sizeof(grObjOverlayUBO),
                     .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                 });
  return ov->uniformBuf ? 0 : -1;
}

/** Lazily creates the overlay-lifetime placeholder resources bound at slots
 *  4-6 whenever no texture map is active: a 4-byte storage buffer and a 1x1
 *  white texture/view/sampler. Mirrors the "always-complete bind group with
 *  dummies when unused" convention used for nodeColorsBuf/nodeSizesBuf/
 *  edgeColorsBuf in grRenderer.c's rebuildBindGroup. */
static int ensureObjDummyResources(grRenderer *r) {
  grObjOverlay *ov = &r->objOverlay;

  if (!ov->dummyUvBuf)
    ov->dummyUvBuf = grMakeStorageBuffer(r, NULL, 0, "grender obj dummy uv");

  if (!ov->dummyTexture) {
    ov->dummyTexture = wgpuDeviceCreateTexture(
        r->device,
        &(const WGPUTextureDescriptor){
            .label = {"grender obj dummy tex", WGPU_STRLEN},
            .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
            .dimension = WGPUTextureDimension_2D,
            .size = {1, 1, 1},
            .format = WGPUTextureFormat_RGBA8Unorm,
            .mipLevelCount = 1,
            .sampleCount = 1,
        });
    if (ov->dummyTexture) {
      ov->dummyView = wgpuTextureCreateView(ov->dummyTexture, NULL);
      const uint8_t whitePixel[4] = {255, 255, 255, 255};
      wgpuQueueWriteTexture(
          r->queue,
          &(const WGPUTexelCopyTextureInfo){
              .texture = ov->dummyTexture, .mipLevel = 0, .origin = {0, 0, 0}},
          whitePixel, sizeof(whitePixel),
          &(const WGPUTexelCopyBufferLayout){
              .offset = 0, .bytesPerRow = 4, .rowsPerImage = 1},
          &(const WGPUExtent3D){1, 1, 1});
    }
  }

  if (!ov->dummySampler)
    ov->dummySampler = wgpuDeviceCreateSampler(
        r->device, &(const WGPUSamplerDescriptor){
                       .label = {"grender obj dummy sampler", WGPU_STRLEN},
                       .addressModeU = WGPUAddressMode_ClampToEdge,
                       .addressModeV = WGPUAddressMode_ClampToEdge,
                       .addressModeW = WGPUAddressMode_ClampToEdge,
                       .magFilter = WGPUFilterMode_Nearest,
                       .minFilter = WGPUFilterMode_Nearest,
                       .mipmapFilter = WGPUMipmapFilterMode_Nearest,
                       .maxAnisotropy = 1,
                   });

  return (ov->dummyUvBuf && ov->dummyTexture && ov->dummyView &&
          ov->dummySampler)
             ? 0
             : -1;
}

static int rebuildObjBindGroup(grRenderer *r) {
  grObjOverlay *ov = &r->objOverlay;
  GR_OBJ_RELEASE(wgpuBindGroupRelease, ov->bindGroup);

  if (ensureObjDummyResources(r) < 0)
    return -1;

  bool tex = ov->texMap.active;
  WGPUBuffer uvBuf = tex ? ov->texMap.uvBuf : ov->dummyUvBuf;
  WGPUSampler sampler = tex ? ov->texMap.imageSampler : ov->dummySampler;
  WGPUTextureView view = tex ? ov->texMap.imageView : ov->dummyView;

  const WGPUBindGroupEntry bgEntries[7] = {
      {.binding = 0, .buffer = ov->uniformBuf, .size = sizeof(grObjOverlayUBO)},
      {.binding = 1,
       .buffer = ov->positionsBuf,
       .size = wgpuBufferGetSize(ov->positionsBuf)},
      {.binding = 2,
       .buffer = ov->normalsBuf,
       .size = wgpuBufferGetSize(ov->normalsBuf)},
      {.binding = 3,
       .buffer = ov->indicesBuf,
       .size = wgpuBufferGetSize(ov->indicesBuf)},
      {.binding = 4, .buffer = uvBuf, .size = wgpuBufferGetSize(uvBuf)},
      {.binding = 5, .sampler = sampler},
      {.binding = 6, .textureView = view},
  };
  ov->bindGroup = wgpuDeviceCreateBindGroup(
      r->device, &(const WGPUBindGroupDescriptor){
                     .label = {"grender obj bind group", WGPU_STRLEN},
                     .layout = ov->bindGroupLayout,
                     .entryCount = 7,
                     .entries = bgEntries,
                 });
  ov->bindGroupDirty = false;
  return ov->bindGroup ? 0 : -1;
}

int grObjOverlayLoad(grRenderer *r, const char *path) {
  if (!r || !r->device)
    return -1;

  grObjMesh mesh;
  if (grObjMeshLoad(path, &mesh) < 0)
    return -1;

  grObjOverlay *ov = &r->objOverlay;
  if (!ov->pipelineLayout && createObjPipelines(r) < 0) {
    grObjMeshRelease(&mesh);
    return -1;
  }

  GR_OBJ_RELEASE(wgpuBufferRelease, ov->positionsBuf);
  GR_OBJ_RELEASE(wgpuBufferRelease, ov->normalsBuf);
  GR_OBJ_RELEASE(wgpuBufferRelease, ov->indicesBuf);
  grObjMeshRelease(&ov->mesh);

  ov->positionsBuf =
      grMakeStorageBuffer(r, mesh.positions, sizeof(float) * mesh.vertexCount * 3,
                          "grender obj positions");
  ov->normalsBuf =
      grMakeStorageBuffer(r, mesh.normals, sizeof(float) * mesh.vertexCount * 3,
                          "grender obj normals");
  ov->indicesBuf = grMakeStorageBuffer(
      r, mesh.indices, sizeof(uint32_t) * mesh.indexCount, "grender obj indices");
  if (!ov->positionsBuf || !ov->normalsBuf || !ov->indicesBuf) {
    grObjMeshRelease(&mesh);
    return -1;
  }

  ov->mesh = mesh;
  ov->bindGroupDirty = true;

  grCameraInit3D(&ov->camera);
  grCameraFitBox(&ov->camera, mesh.bmin, mesh.bmax, 1.0, 1.0);

  ov->loaded = true;
  ov->visible = true;
  return 0;
}

void grObjOverlayClear(grRenderer *r) {
  if (!r)
    return;
  grObjOverlay *ov = &r->objOverlay;
  grTextureMapRelease(r);
  GR_OBJ_RELEASE(wgpuBufferRelease, ov->positionsBuf);
  GR_OBJ_RELEASE(wgpuBufferRelease, ov->normalsBuf);
  GR_OBJ_RELEASE(wgpuBufferRelease, ov->indicesBuf);
  GR_OBJ_RELEASE(wgpuBindGroupRelease, ov->bindGroup);
  grObjMeshRelease(&ov->mesh);
  ov->loaded = false;
  ov->visible = false;
}

void grObjOverlayUpdate(grRenderer *r, double dt) {
  if (!r || !r->objOverlay.loaded)
    return;
  r->objOverlay.camera.yaw += dt * GR_OBJ_SPIN_RADIANS_PER_SEC;
  grTextureMapUpdate(r);
}

void grObjOverlayEncode(grRenderer *r, WGPUCommandEncoder encoder,
                        WGPUTextureView colorTarget, WGPUTextureView depthView,
                        double fbw, double fbh) {
  grObjOverlay *ov = &r->objOverlay;
  if (!ov->loaded || !ov->visible)
    return;

  double s = r->contentScale > 0.0 ? r->contentScale : 1.0;
  double size = GR_OBJ_PANEL_SIZE * s;
  double margin = GR_OBJ_PANEL_MARGIN * s;
  double x0 = margin, y1 = fbh - margin, y0 = y1 - size, x1 = x0 + size;
  if (size <= 1.0 || x0 < 0.0 || y0 < 0.0 || x1 > fbw || y1 > fbh)
    return; // window too small to fit the panel

  if (ov->bindGroupDirty && rebuildObjBindGroup(r) < 0)
    return;

  grCameraFrame frame;
  grCameraFrameCompute(&ov->camera, size, size, &frame);

  grObjOverlayUBO u = {0};
  memcpy(u.viewProj, frame.viewProj, sizeof(u.viewProj));
  // Headlight from the orbit camera, lifted slightly toward its "up" so the
  // top of the mesh doesn't go fully flat.
  for (int i = 0; i < 3; i++)
    u.lightDir[i] = (float)(-frame.forward[i] + 0.35 * frame.camUp[i]);
  u.baseColor[0] = 0.62f;
  u.baseColor[1] = 0.80f;
  u.baseColor[2] = 0.98f;
  u.baseColor[3] = 1.0f;
  u.panelSizePx[0] = (float)size;
  u.panelSizePx[1] = (float)size;
  u.texFlags[0] = ov->texMap.active ? 1.0f : 0.0f;
  wgpuQueueWriteBuffer(r->queue, ov->uniformBuf, 0, &u, sizeof(u));

  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(
      encoder,
      &(const WGPURenderPassDescriptor){
          .colorAttachmentCount = 1,
          .colorAttachments =
              &(const WGPURenderPassColorAttachment){
                  .view = colorTarget,
                  .loadOp = WGPULoadOp_Load,
                  .storeOp = WGPUStoreOp_Store,
                  .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
              },
          .depthStencilAttachment =
              &(const WGPURenderPassDepthStencilAttachment){
                  .view = depthView,
                  .depthLoadOp = WGPULoadOp_Clear,
                  .depthStoreOp = WGPUStoreOp_Discard,
                  .depthClearValue = 1.0f,
              },
      });

  wgpuRenderPassEncoderSetViewport(pass, (float)x0, (float)y0, (float)size,
                                   (float)size, 0.0f, 1.0f);
  wgpuRenderPassEncoderSetScissorRect(pass, (uint32_t)x0, (uint32_t)y0,
                                      (uint32_t)size, (uint32_t)size);
  wgpuRenderPassEncoderSetBindGroup(pass, 0, ov->bindGroup, 0, NULL);

  wgpuRenderPassEncoderSetPipeline(pass, ov->bgPipeline);
  wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);

  wgpuRenderPassEncoderSetPipeline(pass, ov->meshPipeline);
  wgpuRenderPassEncoderDraw(pass, (uint32_t)ov->mesh.indexCount, 1, 0, 0);

  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);
}

void grObjOverlayRelease(grRenderer *r) {
  if (!r)
    return;
  grObjOverlay *ov = &r->objOverlay;
  grTextureMapRelease(r);
  GR_OBJ_RELEASE(wgpuBufferRelease, ov->positionsBuf);
  GR_OBJ_RELEASE(wgpuBufferRelease, ov->normalsBuf);
  GR_OBJ_RELEASE(wgpuBufferRelease, ov->indicesBuf);
  GR_OBJ_RELEASE(wgpuBufferRelease, ov->uniformBuf);
  GR_OBJ_RELEASE(wgpuBufferRelease, ov->dummyUvBuf);
  GR_OBJ_RELEASE(wgpuTextureViewRelease, ov->dummyView);
  if (ov->dummyTexture) {
    wgpuTextureDestroy(ov->dummyTexture);
    GR_OBJ_RELEASE(wgpuTextureRelease, ov->dummyTexture);
  }
  GR_OBJ_RELEASE(wgpuSamplerRelease, ov->dummySampler);
  GR_OBJ_RELEASE(wgpuBindGroupRelease, ov->bindGroup);
  GR_OBJ_RELEASE(wgpuRenderPipelineRelease, ov->bgPipeline);
  GR_OBJ_RELEASE(wgpuRenderPipelineRelease, ov->meshPipeline);
  GR_OBJ_RELEASE(wgpuPipelineLayoutRelease, ov->pipelineLayout);
  GR_OBJ_RELEASE(wgpuBindGroupLayoutRelease, ov->bindGroupLayout);
  GR_OBJ_RELEASE(wgpuShaderModuleRelease, ov->shaderModule);
  grObjMeshRelease(&ov->mesh);
}

// ------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------

int grRendererLoadObjOverlay(grRenderer *r, const char *path) {
  if (!r || !path)
    return -1;
  return grObjOverlayLoad(r, path);
}

void grRendererShowObjOverlay(grRenderer *r, bool show) {
  if (!r || !r->objOverlay.loaded)
    return;
  r->objOverlay.visible = show;
}

bool grRendererObjOverlayShown(const grRenderer *r) {
  return r && r->objOverlay.loaded && r->objOverlay.visible;
}

void grRendererClearObjOverlay(grRenderer *r) { grObjOverlayClear(r); }
