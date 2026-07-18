/**
 * Texture mapping: derives (u, v) texture coordinates for the object
 * overlay's mesh from where a live 2D Tutte-embedded graph's vertices land
 * relative to a movable image rectangle in embedding space. Relies on
 * gvizGraphLoadFromObjFile (gviz side) and grObjMeshLoad (this side) parsing
 * '.obj' 'v' lines in identical file order, so embedding vertex i and mesh
 * vertex i are always the same physical vertex; see grObjMesh.c for the mesh
 * side of that contract.
 *
 * This file is the sole translation unit that defines
 * STB_IMAGE_IMPLEMENTATION; every other translation unit that needs
 * stb_image must only declare/include the header without the macro.
 */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "grInternal.h"
#include "grShaders.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GR_LOG(...) fprintf(stderr, "[grender] " __VA_ARGS__)

static void texMapFreeStaging(grTextureMap *tm) {
  free(tm->uvStaging);
  free(tm->insideStaging);
  free(tm->faceValidStaging);
  free(tm->triValidStaging);
  tm->uvStaging = NULL;
  tm->insideStaging = NULL;
  tm->faceValidStaging = NULL;
  tm->triValidStaging = NULL;
}

static void texMapReleaseImageQuadPipeline(grTextureMap *tm) {
  if (tm->imgQuadBindGroup)
    wgpuBindGroupRelease(tm->imgQuadBindGroup);
  if (tm->imgQuadUniformBuf)
    wgpuBufferRelease(tm->imgQuadUniformBuf);
  if (tm->imgQuadPipeline)
    wgpuRenderPipelineRelease(tm->imgQuadPipeline);
  if (tm->imgQuadPipelineLayout)
    wgpuPipelineLayoutRelease(tm->imgQuadPipelineLayout);
  if (tm->imgQuadBindGroupLayout)
    wgpuBindGroupLayoutRelease(tm->imgQuadBindGroupLayout);
  if (tm->imgQuadShaderModule)
    wgpuShaderModuleRelease(tm->imgQuadShaderModule);
  tm->imgQuadBindGroup = NULL;
  tm->imgQuadUniformBuf = NULL;
  tm->imgQuadPipeline = NULL;
  tm->imgQuadPipelineLayout = NULL;
  tm->imgQuadBindGroupLayout = NULL;
  tm->imgQuadShaderModule = NULL;
}

static void texMapReleaseGpu(grTextureMap *tm) {
  texMapReleaseImageQuadPipeline(tm);
  if (tm->uvBuf)
    wgpuBufferRelease(tm->uvBuf);
  if (tm->triValidBuf)
    wgpuBufferRelease(tm->triValidBuf);
  if (tm->imageView)
    wgpuTextureViewRelease(tm->imageView);
  if (tm->imageTexture) {
    wgpuTextureDestroy(tm->imageTexture);
    wgpuTextureRelease(tm->imageTexture);
  }
  if (tm->imageSampler)
    wgpuSamplerRelease(tm->imageSampler);
  tm->uvBuf = NULL;
  tm->triValidBuf = NULL;
  tm->imageView = NULL;
  tm->imageTexture = NULL;
  tm->imageSampler = NULL;
}

grTextureMap *grRendererLoadTextureMap(grRenderer *r, gvizEmbeddedGraph *graph,
                                       const char *objPath,
                                       const char *imagePath) {
  if (!r || !r->device || !graph || !objPath || !imagePath) {
    GR_LOG("grRendererLoadTextureMap: missing argument\n");
    return NULL;
  }
  if (gvizEmbeddedGraphDim(graph) != 2) {
    GR_LOG("grRendererLoadTextureMap: embedding must be 2D\n");
    return NULL;
  }

  if (grObjOverlayLoad(r, objPath) < 0) {
    GR_LOG("grRendererLoadTextureMap: failed to load obj '%s'\n", objPath);
    return NULL;
  }

  grObjOverlay *ov = &r->objOverlay;
  size_t vertexCount = ov->mesh.vertexCount;
  size_t indexCount = ov->mesh.indexCount;
  size_t faceCount = ov->mesh.faceCount;
  size_t triangleCount = indexCount / 3;

  if (vertexCount != gvizEmbeddedGraphPositionCount(graph)) {
    GR_LOG("grRendererLoadTextureMap: mesh vertex count %zu != embedding "
          "vertex count %zu\n",
          vertexCount, gvizEmbeddedGraphPositionCount(graph));
    grObjOverlayClear(r);
    return NULL;
  }

  int w = 0, h = 0, channels = 0;
  unsigned char *pixels = stbi_load(imagePath, &w, &h, &channels, 4);
  if (!pixels) {
    GR_LOG("grRendererLoadTextureMap: failed to load image '%s' (%s)\n",
          imagePath, stbi_failure_reason());
    grObjOverlayClear(r);
    return NULL;
  }

  grTextureMap *tm = &ov->texMap;
  memset(tm, 0, sizeof(*tm));

  tm->imageTexture = wgpuDeviceCreateTexture(
      r->device,
      &(const WGPUTextureDescriptor){
          .label = {"grender texmap image", WGPU_STRLEN},
          .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
          .dimension = WGPUTextureDimension_2D,
          .size = {(uint32_t)w, (uint32_t)h, 1},
          .format = WGPUTextureFormat_RGBA8Unorm,
          .mipLevelCount = 1,
          .sampleCount = 1,
      });
  if (tm->imageTexture)
    tm->imageView = wgpuTextureCreateView(tm->imageTexture, NULL);
  if (tm->imageTexture && tm->imageView) {
    wgpuQueueWriteTexture(
        r->queue,
        &(const WGPUTexelCopyTextureInfo){
            .texture = tm->imageTexture,
            .mipLevel = 0,
            .origin = {0, 0, 0},
        },
        pixels, (size_t)w * (size_t)h * 4,
        &(const WGPUTexelCopyBufferLayout){
            .offset = 0,
            .bytesPerRow = (uint32_t)w * 4,
            .rowsPerImage = (uint32_t)h,
        },
        &(const WGPUExtent3D){(uint32_t)w, (uint32_t)h, 1});
  }
  stbi_image_free(pixels);

  tm->imageSampler = wgpuDeviceCreateSampler(
      r->device, &(const WGPUSamplerDescriptor){
                     .label = {"grender texmap sampler", WGPU_STRLEN},
                     .addressModeU = WGPUAddressMode_ClampToEdge,
                     .addressModeV = WGPUAddressMode_ClampToEdge,
                     .addressModeW = WGPUAddressMode_ClampToEdge,
                     .magFilter = WGPUFilterMode_Linear,
                     .minFilter = WGPUFilterMode_Linear,
                     .mipmapFilter = WGPUMipmapFilterMode_Linear,
                     .lodMinClamp = 0.0f,
                     .lodMaxClamp = 32.0f,
                     .maxAnisotropy = 1,
                 });

  if (!tm->imageTexture || !tm->imageView || !tm->imageSampler) {
    GR_LOG("grRendererLoadTextureMap: failed to create GPU image resources\n");
    texMapReleaseGpu(tm);
    grObjOverlayClear(r);
    return NULL;
  }

  tm->uvBuf = grMakeStorageBuffer(r, NULL, sizeof(float) * 2 * vertexCount,
                                  "grender texmap uv");
  tm->triValidBuf = grMakeStorageBuffer(
      r, NULL, sizeof(uint32_t) * triangleCount, "grender texmap triValid");
  if (!tm->uvBuf || !tm->triValidBuf) {
    GR_LOG("grRendererLoadTextureMap: failed to create GPU buffers\n");
    texMapReleaseGpu(tm);
    grObjOverlayClear(r);
    return NULL;
  }

  tm->uvStaging = malloc(sizeof(float) * 2 * vertexCount);
  tm->insideStaging = malloc(sizeof(uint32_t) * vertexCount);
  tm->faceValidStaging = malloc(sizeof(uint32_t) * faceCount);
  tm->triValidStaging = malloc(sizeof(uint32_t) * triangleCount);
  if (!tm->uvStaging || !tm->insideStaging || !tm->faceValidStaging ||
      !tm->triValidStaging) {
    GR_LOG("grRendererLoadTextureMap: staging allocation failed\n");
    texMapFreeStaging(tm);
    texMapReleaseGpu(tm);
    grObjOverlayClear(r);
    return NULL;
  }

  const double *pos = gvizEmbeddedGraphPositions(graph);
  double bmin[2] = {INFINITY, INFINITY};
  double bmax[2] = {-INFINITY, -INFINITY};
  for (size_t i = 0; i < vertexCount; i++) {
    double x = pos[i * 2 + 0], y = pos[i * 2 + 1];
    if (x < bmin[0])
      bmin[0] = x;
    if (x > bmax[0])
      bmax[0] = x;
    if (y < bmin[1])
      bmin[1] = y;
    if (y > bmax[1])
      bmax[1] = y;
  }
  // Fit the image's aspect ratio inside the bbox, matching whichever
  // dimension the bbox is proportionally larger in, so the whole bbox stays
  // covered without distorting the image.
  double bboxW = bmax[0] - bmin[0];
  double bboxH = bmax[1] - bmin[1];
  if (bboxW < 1e-9)
    bboxW = 1e-9;
  if (bboxH < 1e-9)
    bboxH = 1e-9;
  double cx = (bmin[0] + bmax[0]) * 0.5;
  double cy = (bmin[1] + bmax[1]) * 0.5;
  double aspect = (h > 0) ? (double)w / (double)h : 1.0;
  double halfW, halfH;
  if (bboxW / bboxH > aspect) {
    halfW = bboxW * 0.5;
    halfH = halfW / aspect;
  } else {
    halfH = bboxH * 0.5;
    halfW = halfH * aspect;
  }

  tm->imgCenter[0] = tm->initCenter[0] = cx;
  tm->imgCenter[1] = tm->initCenter[1] = cy;
  tm->imgHalfExtent[0] = tm->initHalfExtent[0] = halfW;
  tm->imgHalfExtent[1] = tm->initHalfExtent[1] = halfH;
  tm->graph = graph;
  tm->imageW = w;
  tm->imageH = h;
  tm->active = true;
  tm->visible = true;

  // The bind group must be rebuilt so slots 4-7 pick up the newly-active
  // texture map's real buffers/image instead of the overlay's placeholders.
  ov->bindGroupDirty = true;

  grPlatformTextureMapMenuRefresh(r);

  return tm;
}

static int ensureImageQuadPipeline(grRenderer *r, grTextureMap *tm) {
  if (tm->imgQuadPipeline)
    return 0;

  tm->imgQuadShaderModule = wgpuDeviceCreateShaderModule(
      r->device,
      &(const WGPUShaderModuleDescriptor){
          .label = {"grender texmap image shaders", WGPU_STRLEN},
          .nextInChain =
              (WGPUChainedStruct *)&(WGPUShaderSourceWGSL){
                  .chain = {.sType = WGPUSType_ShaderSourceWGSL},
                  .code = {GR_WGSL_TEXMAP_IMAGE_SOURCE, WGPU_STRLEN},
              },
      });
  if (!tm->imgQuadShaderModule)
    return -1;

  const WGPUBindGroupLayoutEntry entries[3] = {
      {.binding = 0,
       .visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment,
       .buffer = {.type = WGPUBufferBindingType_Uniform}},
      {.binding = 1,
       .visibility = WGPUShaderStage_Fragment,
       .sampler = {.type = WGPUSamplerBindingType_Filtering}},
      {.binding = 2,
       .visibility = WGPUShaderStage_Fragment,
       .texture = {.sampleType = WGPUTextureSampleType_Float,
                   .viewDimension = WGPUTextureViewDimension_2D,
                   .multisampled = false}},
  };
  tm->imgQuadBindGroupLayout = wgpuDeviceCreateBindGroupLayout(
      r->device, &(const WGPUBindGroupLayoutDescriptor){
                     .label = {"grender texmap image bgl", WGPU_STRLEN},
                     .entryCount = 3,
                     .entries = entries,
                 });
  tm->imgQuadPipelineLayout = wgpuDeviceCreatePipelineLayout(
      r->device,
      &(const WGPUPipelineLayoutDescriptor){
          .label = {"grender texmap image layout", WGPU_STRLEN},
          .bindGroupLayoutCount = 1,
          .bindGroupLayouts =
              (const WGPUBindGroupLayout[]){tm->imgQuadBindGroupLayout},
      });
  if (!tm->imgQuadBindGroupLayout || !tm->imgQuadPipelineLayout)
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
  // Drawn first (before nodes/edges), never occluding them: no depth write,
  // always passes so it never fights the scene's cleared depth buffer.
  const WGPUDepthStencilState depthState = {
      .format = WGPUTextureFormat_Depth24Plus,
      .depthWriteEnabled = WGPUOptionalBool_False,
      .depthCompare = WGPUCompareFunction_Always,
      .stencilFront = {.compare = WGPUCompareFunction_Always},
      .stencilBack = {.compare = WGPUCompareFunction_Always},
      .stencilReadMask = 0xFFFFFFFF,
      .stencilWriteMask = 0xFFFFFFFF,
  };
  tm->imgQuadPipeline = wgpuDeviceCreateRenderPipeline(
      r->device,
      &(const WGPURenderPipelineDescriptor){
          .label = {"grender texmap image", WGPU_STRLEN},
          .layout = tm->imgQuadPipelineLayout,
          .vertex = {.module = tm->imgQuadShaderModule,
                     .entryPoint = {"vsTexMapImage", WGPU_STRLEN}},
          .fragment =
              &(const WGPUFragmentState){
                  .module = tm->imgQuadShaderModule,
                  .entryPoint = {"fsTexMapImage", WGPU_STRLEN},
                  .targetCount = 1,
                  .targets = &colorTarget,
              },
          .primitive = {.topology = WGPUPrimitiveTopology_TriangleList,
                        .cullMode = WGPUCullMode_None},
          .depthStencil = &depthState,
          .multisample = {.count = 1, .mask = 0xFFFFFFFF},
      });
  if (!tm->imgQuadPipeline)
    return -1;

  tm->imgQuadUniformBuf = wgpuDeviceCreateBuffer(
      r->device,
      &(const WGPUBufferDescriptor){
          .label = {"grender texmap image globals", WGPU_STRLEN},
          .size = sizeof(grTexMapImageUBO),
          .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
      });
  if (!tm->imgQuadUniformBuf)
    return -1;

  const WGPUBindGroupEntry bgEntries[3] = {
      {.binding = 0,
       .buffer = tm->imgQuadUniformBuf,
       .size = sizeof(grTexMapImageUBO)},
      {.binding = 1, .sampler = tm->imageSampler},
      {.binding = 2, .textureView = tm->imageView},
  };
  tm->imgQuadBindGroup = wgpuDeviceCreateBindGroup(
      r->device, &(const WGPUBindGroupDescriptor){
                     .label = {"grender texmap image bind group", WGPU_STRLEN},
                     .layout = tm->imgQuadBindGroupLayout,
                     .entryCount = 3,
                     .entries = bgEntries,
                 });
  return tm->imgQuadBindGroup ? 0 : -1;
}

void grTextureMapEncodeImageQuad(grRenderer *r, WGPURenderPassEncoder pass) {
  if (!r || !pass)
    return;
  grTextureMap *tm = &r->objOverlay.texMap;
  if (!tm->active || !tm->visible)
    return;
  if (ensureImageQuadPipeline(r, tm) < 0)
    return;

  grTexMapImageUBO u = {0};
  memcpy(u.viewProj, r->cameraFrame.viewProj, sizeof(u.viewProj));
  u.rectCenter[0] = (float)tm->imgCenter[0];
  u.rectCenter[1] = (float)tm->imgCenter[1];
  u.rectHalfExtent[0] = (float)tm->imgHalfExtent[0];
  u.rectHalfExtent[1] = (float)tm->imgHalfExtent[1];
  u.opacity = 1.0f;
  wgpuQueueWriteBuffer(r->queue, tm->imgQuadUniformBuf, 0, &u, sizeof(u));

  wgpuRenderPassEncoderSetBindGroup(pass, 0, tm->imgQuadBindGroup, 0, NULL);
  wgpuRenderPassEncoderSetPipeline(pass, tm->imgQuadPipeline);
  wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
}

void grRendererShowTextureMapImage(grRenderer *r, bool show) {
  if (!r || !r->objOverlay.texMap.active)
    return;
  r->objOverlay.texMap.visible = show;
  grPlatformTextureMapMenuRefresh(r);
}

bool grRendererTextureMapImageShown(const grRenderer *r) {
  return r && r->objOverlay.texMap.active && r->objOverlay.texMap.visible;
}

void grTextureMapRelease(grRenderer *r) {
  if (!r)
    return;
  grTextureMap *tm = &r->objOverlay.texMap;
  texMapFreeStaging(tm);
  texMapReleaseGpu(tm);
  memset(tm, 0, sizeof(*tm));
}

void grTextureMapComputeUV(const double *pos2D, size_t vertexCount,
                           const double center[2], const double halfExtent[2],
                           float *uvOut, uint32_t *insideOut) {
  double x0 = center[0] - halfExtent[0];
  double y0 = center[1] - halfExtent[1];
  double invW = 1.0 / (2.0 * halfExtent[0]);
  double invH = 1.0 / (2.0 * halfExtent[1]);
  for (size_t i = 0; i < vertexCount; i++) {
    double x = pos2D[i * 2 + 0];
    double y = pos2D[i * 2 + 1];
    double u = (x - x0) * invW;
    double v = 1.0 - (y - y0) * invH;
    uvOut[i * 2 + 0] = (float)u;
    uvOut[i * 2 + 1] = (float)v;
    insideOut[i] = (u >= 0.0 && u <= 1.0 && v >= 0.0 && v <= 1.0) ? 1u : 0u;
  }
}

void grTextureMapComputeFaceValidity(const uint32_t *insideOut,
                                     size_t vertexCount,
                                     const uint32_t *indices,
                                     const uint32_t *triangleFaceIds,
                                     size_t triangleCount, size_t faceCount,
                                     uint32_t *faceValidOut,
                                     uint32_t *triValidOut) {
  (void)vertexCount;
  for (size_t f = 0; f < faceCount; f++)
    faceValidOut[f] = 1u;

  for (size_t t = 0; t < triangleCount; t++) {
    uint32_t a = indices[t * 3 + 0];
    uint32_t b = indices[t * 3 + 1];
    uint32_t c = indices[t * 3 + 2];
    uint32_t triAllInside =
        (insideOut[a] && insideOut[b] && insideOut[c]) ? 1u : 0u;
    faceValidOut[triangleFaceIds[t]] &= triAllInside;
  }

  for (size_t t = 0; t < triangleCount; t++)
    triValidOut[t] = faceValidOut[triangleFaceIds[t]];
}

void grTextureMapUpdate(grRenderer *r) {
  if (!r)
    return;
  grTextureMap *tm = &r->objOverlay.texMap;
  if (!tm->active)
    return;

  size_t vertexCount = r->objOverlay.mesh.vertexCount;

  // Whitening for out-of-image faces is a per-fragment UV bounds check in
  // fsObj (see grShaders.h), so only uv needs to be recomputed/uploaded here
  // every frame. grTextureMapComputeFaceValidity/triValidStaging/triValidBuf
  // are unused by the render path but kept as pure, unit-tested CPU logic
  // (see examples/textureMapUnitTest.c) in case a per-triangle mode is
  // wanted again later.
  const double *pos = gvizEmbeddedGraphPositions(tm->graph);
  grTextureMapComputeUV(pos, vertexCount, tm->imgCenter, tm->imgHalfExtent,
                        tm->uvStaging, tm->insideStaging);

  wgpuQueueWriteBuffer(r->queue, tm->uvBuf, 0, tm->uvStaging,
                       sizeof(float) * 2 * vertexCount);
}

void grTextureMapSetImageRect(grTextureMap *tm, double cx, double cy,
                              double halfW, double halfH) {
  if (!tm)
    return;
  tm->imgCenter[0] = cx;
  tm->imgCenter[1] = cy;
  tm->imgHalfExtent[0] = halfW;
  tm->imgHalfExtent[1] = halfH;
}

void grTextureMapMoveImage(grTextureMap *tm, double dx, double dy) {
  if (!tm)
    return;
  tm->imgCenter[0] += dx;
  tm->imgCenter[1] += dy;
}

void grTextureMapScaleImage(grTextureMap *tm, double factor) {
  if (!tm)
    return;
  tm->imgHalfExtent[0] *= factor;
  tm->imgHalfExtent[1] *= factor;
}

void grTextureMapResetImage(grTextureMap *tm) {
  if (!tm)
    return;
  tm->imgCenter[0] = tm->initCenter[0];
  tm->imgCenter[1] = tm->initCenter[1];
  tm->imgHalfExtent[0] = tm->initHalfExtent[0];
  tm->imgHalfExtent[1] = tm->initHalfExtent[1];
}

void grTextureMapGetImageRect(const grTextureMap *tm, double *cx, double *cy,
                              double *halfW, double *halfH) {
  double zcx = 0.0, zcy = 0.0, zhw = 0.0, zhh = 0.0;
  if (tm) {
    zcx = tm->imgCenter[0];
    zcy = tm->imgCenter[1];
    zhw = tm->imgHalfExtent[0];
    zhh = tm->imgHalfExtent[1];
  }
  if (cx)
    *cx = zcx;
  if (cy)
    *cy = zcy;
  if (halfW)
    *halfW = zhw;
  if (halfH)
    *halfH = zhh;
}
