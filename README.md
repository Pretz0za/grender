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
- Prebuilt static libraries live under `third-party/` - no Rust toolchain
  required.

## Building

One-time dependency setup (downloads pinned wgpu-native and GLFW, or imports
from an existing `build/_deps/` cache if present):

```sh
./scripts/setup-deps.sh
```

Then configure and build:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

See **Example apps** below for runnable demos.

Requirements: CMake >= 3.20, a C11 compiler, OpenBLAS in `$HOME/lib` (needed by
grender for 4D PCA projection), and the gviz repo as a sibling directory (or
set `-DGRENDER_GVIZ_DIR=/path/to/gviz`). Network access is only needed when
running `scripts/setup-deps.sh`.

### Example apps

All examples require a built `graphvis` target from gviz.

```sh
./build/gripDemo              # live GRIP on a Möbius mesh (default 24×48, 3D)
./build/gripDemo 60 40 3      # larger mesh, 3D
./build/treeDemo              # Reingold-Tilford tree layout (binary, depth 7)
./build/treeDemo 3 5          # 3-ary tree, depth 5
./build/millionDemo           # 1M-vertex online position-update stress test
./build/datasetDemo human-jung-2015 2   # GRIP on a gviz data/ graph
```

`datasetDemo` needs the gviz `data/` tree; CMake passes
`GRENDER_GVIZ_DATA_DIR` automatically when gviz is built as a subdirectory.

## Working with gviz

grender only consumes the public gviz API (`gvizEmbeddedGraph`,
`gvizSubgraph`, …). Embedding algorithms live in the sibling
[`gviz`](../gviz) repo and are linked into the example apps via the
`graphvis` static library. **Do not modify gviz embedder code from grender
unless you are intentionally fixing or extending gviz itself.**

### Graph layout before subgraphs

`gvizSubgraphCreateFull` returns an **empty** subgraph (null graph pointer)
when the parent graph has no built layout. Every example follows the same
order:

```c
gvizGraph graph = ...;          // build vertices and edges first
gvizGraphBuildLayout(&graph);   // required — builds the shared edge layout
gvizSubgraph sg = gvizSubgraphCreateFull(&graph);
```

Skipping `gvizGraphBuildLayout` silently produces an invalid subgraph; embedder
init and rendering will then crash or fail. Rebuild the layout after any
structural change to vertices or edges (add/remove), then recreate affected
subgraphs.

### Embedder-specific notes

| embedder | header | graph requirements | dimensions |
| -------- | ------ | ------------------ | ---------- |
| GRIP | `embedders/gvizGRIPEmbedder.h` | any graph; undirected is fine | 2, 3, or 4 |
| Reingold-Tilford tree | `embedders/gvizEmbeddedTree.h` | **directed tree** rooted at the chosen vertex (`gvizGraphInit(..., 1)`) | 2 only |
| (manual positions) | `embedders/gvizEmbeddedGraph.h` | any | 2, 3, or 4 |

Tree layout workflow (`treeDemo`):

```c
gvizEmbeddedTree tree = {0};
gvizEmbeddedTreeRTInit(&tree, sg, root);
gvizEmbeddedTreeCalculateOffsets(&tree, root, 0);
double pos[2] = {0.0, 0.0};
gvizEmbeddedTreeEmbed(&tree, root, pos);
grRendererSetGraph(r, (gvizEmbeddedGraph *)&tree);  // tree embeds gvizEmbeddedGraph
```

`gvizEmbeddedTreeRTInit` calls `gvizGraphIsTree` and returns `-1` if the graph
is not a directed tree (undirected graphs return `-2` from the tree check).

### Attaching an embedded graph to grender

- Pass any embedder state cast to `gvizEmbeddedGraph*` (the embedder struct
  must have `gvizEmbeddedGraph` as its first member).
- Call `grRendererGraphStructureChanged(r)` only when vertices/edges are
  added, removed, or hidden/shown — not for position-only updates.
- Register embedder actions with `gvizEmbeddedGraphAddAction` and bind keys
  with `grRendererBindKey`; the renderer dispatches without knowing the
  embedder type.
- 4D embeddings are PCA-projected to 3D inside grender each frame.

### Automated screenshots

Pass a `.ppm` path as the last argument to `gripDemo`, `treeDemo`, or
`millionDemo`; the app renders a few frames, saves the image, and exits.
Useful for headless CI checks.


Dependencies (pinned in `scripts/setup-deps.sh`):

| dependency  | version   | default path                         |
| ----------- | --------- | ------------------------------------ |
| wgpu-native | v29.0.1.1 | `third-party/wgpu-native/`           |
| GLFW        | 3.4       | `third-party/glfw/`                  |

Override with `-DGRENDER_WGPU_NATIVE_DIR=...` or `-DGRENDER_GLFW_DIR=...` if you
install them elsewhere. To upgrade wgpu-native, bump the version in
`scripts/setup-deps.sh`, re-run the script, and check release notes for
`webgpu.h` API changes.

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
examples/treeDemo.c       Reingold-Tilford tree layout (gvizEmbeddedTree)
examples/datasetDemo.c    GRIP on graphs from gviz data/
examples/millionDemo.c      1M-vertex online-update stress test
```

`grRendererSaveScreenshot` renders the current scene offscreen and writes a
PPM - useful for automated visual checks in CI.
