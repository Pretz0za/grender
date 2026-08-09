# grender

A GPU renderer for [gviz](../gviz) embedded graphs, built on WebGPU
([wgpu-native](https://github.com/gfx-rs/wgpu-native)) and GLFW.

grender lives strictly on the consumer side of the gviz abstraction barrier:
it reads embedded graphs only through the public `gviz::layout::EmbeddedGraph`
/ `gviz::Subgraph` API and never depends on which embedding algorithm produced
the positions. Both grender and gviz are C++20.

## Design

- **Embedder-agnostic** (spec 1): the only input is a
  `gviz::layout::EmbeddedGraph&`. Structure comes from its subgraph, geometry
  from its position buffer, and interactivity from its action registry. 2D
  and 3D embeddings are supported.
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
  registers named handlers on it via `EmbeddedGraph::AddAction` (e.g. the
  GRIP embedder registers `"grip.refineRound"` and `"grip.nextStage"`).
  The application binds inputs to names with
  `grRendererBindKey(r, 'R', "grip.refineRound")`; the renderer fills a
  `gviz::layout::ActionPayload` (cursor position in embedding coordinates,
  modifiers, frame delta time) and dispatches. Neither side knows about the
  other.

## Why WebGPU / wgpu-native

- A standardized C API (`webgpu.h`) with first-class storage buffers and
  instancing - the exact features the two-draw-call design needs.
- Runs natively on Metal/Vulkan/D3D12 with no OpenGL emulation layers.
- The same API is implemented by browsers and Emscripten, so a future web
  build can reuse the renderer core unchanged; only the platform layer
  (window/surface creation, currently GLFW + `grSurfaceCocoa.mm`) is swapped.
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

Requirements: CMake >= 3.20, a C++20 compiler, OpenBLAS in `$HOME/lib` (needed
by grender for 4D PCA projection), and the gviz repo as a sibling directory
(or set `-DGRENDER_GVIZ_DIR=/path/to/gviz`). Network access is only needed
when running `scripts/setup-deps.sh`.

### Example apps

All examples require a built `gviz` target from gviz.

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

grender only consumes the public gviz API (`gviz::layout::EmbeddedGraph`,
`gviz::Subgraph`, …). Embedding algorithms live in the sibling
[`gviz`](../gviz) repo and are linked into the example apps via the
`gviz` shared library. **Do not modify gviz embedder code from grender
unless you are intentionally fixing or extending gviz itself.**

### Data structures: use gviz's / the standard library, never reimplement

gviz's own data structures (`gviz::Graph`, `gviz::Subgraph`, `gviz::BitSet`,
`gviz::QuadTree`, ...) are real C++ classes under its `include/`, and
grender already links against them. grender-internal dynamic state (pending
input queues, key/mouse bindings, staging buffers, ...) is plain
`std::vector<T>` — never a hand-rolled `malloc`/`realloc`-doubling loop.
Before adding a new container of any kind, check whether gviz or the
standard library already provides it; the goal is one implementation of each
data structure in the combined codebase, not one per call site.

### Graph layout before subgraphs

`gviz::Subgraph::CreateFull`/`CreateEmpty` **throw `gviz::NoLayoutError`**
when the parent graph has no built layout. Every example follows the same
order:

```cpp
gviz::Graph graph(/*directed=*/false);   // build vertices and edges first
graph.BuildLayout();                     // required — builds the shared edge layout
gviz::Subgraph sg = gviz::Subgraph::CreateFull(graph);
```

Skipping `BuildLayout()` throws instead of silently producing an invalid
subgraph. Rebuild the layout after any structural change to vertices or
edges (add/remove), then recreate affected subgraphs. A vertex-induced
subgraph (`gviz::Subgraph::CreateVertexInduced(graph)`) never throws and
needs no built layout at all — prefer it for anything dynamic (see gviz's
own `CLAUDE.md` on `Subgraph`'s two kinds).

### Embedder-specific notes

| embedder | header | graph requirements | dimensions |
| -------- | ------ | ------------------ | ---------- |
| `gviz::layout::GRIP` | `GRIP.hpp` | any graph; undirected is fine | 2, 3, or 4 |
| `gviz::layout::ReingoldTilford` | `ReingoldTilford.hpp` | **directed tree**, rooted (constructor throws `gviz::NotATreeError` otherwise) | 2 only |
| `gviz::layout::ForceAtlas` | `ForceAtlas.hpp` | any graph, directed OK | 2 only |
| `gviz::layout::Tutte` / `SpringTutte` | `Tutte.hpp` / `SpringTutte.hpp` | planar (`Begin()` throws `gviz::PlanarNotPlanarError` otherwise) | 2 only |
| (manual positions) | `EmbeddedGraph.hpp` | any | 2, 3, or 4 |

Tree layout workflow (`treeDemo`):

```cpp
gviz::layout::ReingoldTilford tree(graph, root);  // throws gviz::NotATreeError if not a rooted directed tree
tree.CalculateOffsets(root, 0);
double pos[2] = {0.0, 0.0};
tree.Embed(root, pos);
grRendererSetGraph(r, tree, &graph);   // tree publicly inherits gviz::layout::EmbeddedGraph
```

### Attaching an embedded graph to grender

- Pass any embedder object directly — every embedder (`ForceAtlas`, `GRIP`,
  `Tutte`, `SpringTutte`, `ReingoldTilford`, `Planar`) publicly inherits
  `gviz::layout::EmbeddedGraph`, so no cast is needed.
- Pass the backing `gviz::Graph*` too (`grRendererSetGraph`'s second,
  optional parameter) if you want the highlight/pick/console features that
  need raw parent-graph access (`EnsureLayout`, `GetEdgeWeight`, planar face
  queries) — `gviz::Subgraph` deliberately never exposes its parent graph on
  its own, so grender needs it passed in explicitly. Those features simply
  no-op without it.
- Call `grRendererGraphStructureChanged(r)` only when vertices/edges are
  added, removed, or hidden/shown — not for position-only updates.
- Register embedder actions with `EmbeddedGraph::AddAction` and bind keys
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

```cpp
grRendererDesc desc;
grRendererDescInit(&desc);
grRenderer *r = grRendererCreate(&desc);

grRendererSetGraph(r, embeddedGraph, &graph);    // any gviz embedder output
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
src/grRenderer.cpp          device setup, frame loop, input, GPU buffers
src/grCamera.cpp            2D ortho + 3D orbit camera, picking math
src/grTopology.cpp          the only code that reads gviz structure
src/grStats.cpp             stats overlay: chart layout, text, primitive list
src/grVertexOverlay.cpp     vertex-info overlay: label word-wrap, scroll panel
src/grListOverlay.cpp       vertex list overlay: fuzzy search over vertex data
src/grConsole.cpp           command console: input line, built-in commands
src/grPCA.cpp               4D -> 3D PCA projection (OpenBLAS)
src/grObjMesh.cpp           .obj mesh parsing for the object overlay
src/grObjOverlay.cpp        object overlay: own pipelines, camera, render pass
src/grShaders.h             WGSL (instanced nodes/edges, vertex pulling)
src/grSurfaceCocoa.mm       macOS CAMetalLayer surface glue
src/grMenuCocoa.mm          macOS menu bar (Charts submenu, ...)
src/grPlatformMenu.cpp      non-macOS no-op menu stub
examples/gripDemo.cpp       live GRIP embedding with bound actions
examples/treeDemo.cpp       Reingold-Tilford tree layout (gviz::layout::ReingoldTilford)
examples/datasetDemo.cpp    GRIP on graphs from gviz data/
examples/millionDemo.cpp    1M-vertex online-update stress test
examples/tutteDemo.cpp      live Tutte embedding; optional object overlay
```

`grRendererSaveScreenshot` renders the current scene offscreen and writes a
PPM - useful for automated visual checks in CI.
