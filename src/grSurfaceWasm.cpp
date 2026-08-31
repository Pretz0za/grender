// Emscripten's counterpart to grSurfaceCocoa.mm: instead of wrapping a
// native window handle (there is none -- GLFW's Emscripten emulation has no
// real OS window), the WebGPU surface is created straight from the page's
// canvas via the Dawn/Emscripten-specific chained struct. The canvas
// selector must match the <canvas> element id in wasm/index.html.
#include "grInternal.h"

#include <GLFW/glfw3.h>

WGPUSurface grPlatformCreateSurface(WGPUInstance instance, GLFWwindow *window) {
  (void)window;

  return wgpuInstanceCreateSurface(
      instance,
      grPtr(WGPUSurfaceDescriptor{
          .nextInChain =
              (WGPUChainedStruct *)grPtr(WGPUEmscriptenSurfaceSourceCanvasHTMLSelector{
                  .chain = {.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector},
                  .selector = {"#canvas", WGPU_STRLEN},
              }),
      }));
}
