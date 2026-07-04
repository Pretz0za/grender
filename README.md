# grender

A GPU renderer for [gviz](../gviz) embedded graphs, built on WebGPU
([wgpu-native](https://github.com/gfx-rs/wgpu-native)) and GLFW.

grender lives strictly on the consumer side of the gviz abstraction barrier:
it reads embedded graphs only through the public `gvizEmbeddedGraph` /
`gvizSubgraph` API and never depends on which embedding algorithm produced the
positions.

## Design

- **Embedder-agnostic** (spec 1): the only input is a `gvizEmbeddedGraph*`.
  Structure comes from its subgraph, geometry from its position buffer, and
  interactivity from its action registry. 2D and 3D embeddings are supported.
- **Scales to millions of elements** (spec 2): a frame is exactly **two
  instanced draw calls** (one for all edges, one for all nodes), regardless of
  graph size. There are no per-vertex CPU draw calls and no CPU-side geometry:
  vertex positions, per-node/per-edge styles, and edge endpoints live in GPU
  storage buffers, and the vertex shaders pull from them by instance index.
  Nodes are antialiased SDF circles on billboarded quads; edges are
  screen-space-expanded quads. Per-frame CPU cost is a single
  double-to-float conversion pass over the position array.
- **Online rendering** (spec 3): positions are re-read from the embedded graph
  and re-uploaded every frame, so mutating the embedding between frames (force
  ticks, GRIP rounds, ...) is immediately visible. Only *structural* changes
  (adding/removing/hiding vertices or edges) require a call to
  `grRendererGraphStructureChanged`.
- **Creator-defined actions** (spec 4): the creator of an embedded graph
  registers named handlers on it via `gvizEmbeddedGraphAddAction` (e.g. the
  GRIP embedder registers `"grip.refineRound"` and `"grip.nextStage"`).
  The application binds inputs to names with
  `grRendererBindKey(r, 'R', "grip.refineRound")`; the renderer fills a
  `gvizActionPayload` (cursor position in embedding coordinates, modifiers,
  frame delta time) and dispatches. Neither side knows about the other.

## Why WebGPU / wgpu-native

- A standardized C API (`webgpu.h`) with first-class storage buffers and
  instancing - the exact features the two-draw-call design needs.
- Runs natively on Metal/Vulkan/D3D12 with no OpenGL emulation layers.
- The same API is implemented by browsers and Emscripten, so a future web
  build can reuse the renderer core unchanged; only the platform layer
  (window/surface creation, currently GLFW + `grSurfaceCocoa.m`) is swapped.
- Prebuilt static libraries are fetched at configure time - no Rust toolchain
  required.

## Building

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/gripDemo DEPTH DIM  # DEPTH deep sierpinski tetra embedded live by GRIP
./build/gripDemo 60 40 3    # same in 3D
./build/millionDemo         # 1M vertices / 2M edges, all positions
                            # rewritten every frame (~16 ms/frame on M-series)
```

Requirements: CMake >= 3.20, a C11 compiler, network access on first configure
(fetches pinned wgpu-native and GLFW), and the gviz repo as a sibling
directory (or set `-DGRENDER_GVIZ_DIR=/path/to/gviz`).

Dependencies (pinned in `CMakeLists.txt`):

| dependency  | version   | source                        |
| ----------- | --------- | ----------------------------- |
| wgpu-native | v29.0.1.1 | gfx-rs GitHub release binary  |
| GLFW        | 3.4       | glfw GitHub release source    |

To upgrade wgpu-native, bump `WGPU_NATIVE_VERSION` and check the release notes
for `webgpu.h` API changes.

## API sketch

```c
grRendererDesc desc;
grRendererDescInit(&desc);
grRenderer *r = grRendererCreate(&desc);

grRendererSetGraph(r, embeddedGraph);            // any gviz embedder output
grRendererBindKey(r, 'R', "grip.refineRound");   // creator-defined action

while (grRendererFrame(r)) {
  // mutate the embedding here; changes appear next frame
}

grRendererDestroy(r);
```

Styling: global `grNodeStyle` / `grEdgeStyle` (fill, stroke, radius, width,
pixel- or world-space sizing) plus optional per-node color/size and per-edge
color arrays (`grRendererSetNodeColors` / `grRendererSetNodeSizes` /
`grRendererSetEdgeColors`, packed with `GR_RGBA8`).

Controls built into the renderer: drag to pan (2D) or orbit (3D,
right-drag/shift-drag to pan), scroll to zoom, `F` to fit the graph.

## Layout

```
include/grender/grender.h   public API (the only header consumers include)
src/grRenderer.c            device setup, frame loop, input, GPU buffers
src/grCamera.c              2D ortho + 3D orbit camera, picking math
src/grTopology.c            the only code that reads gviz structure
src/grShaders.h             WGSL (instanced nodes/edges, vertex pulling)
src/grSurfaceCocoa.m        macOS CAMetalLayer surface glue
examples/gripDemo.c         live GRIP embedding with bound actions
examples/millionDemo.c      1M-vertex online-update stress test
```

`grRendererSaveScreenshot` renders the current scene offscreen and writes a
PPM - useful for automated visual checks in CI.
