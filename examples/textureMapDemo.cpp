/**
 * Live texture-mapping demo: a spring-Tutte-embedded 2D graph derived from an
 * .obj mesh's own topology drives per-vertex (u, v) texture coordinates for
 * that same mesh, rendered (as it relaxes) in the object overlay panel.
 * Regions of the mesh that don't overlap the movable image rectangle at all
 * render white; regions that partially overlap it are textured normally,
 * with (u, v) interpolated per-fragment across each triangle (so the
 * transition from textured to white lands exactly at the image's edge, not
 * at a whole triangle's boundary).
 *
 * Unlike tutteDemo, an .obj path is mandatory here: texture mapping needs a
 * real mesh (not a synthetic rect mesh) so the mesh's vertex/'f'-line
 * structure matches the embedded graph 1:1 (see grRendererLoadTextureMap's
 * doc comment for why that correspondence holds).
 *
 * Spring-Tutte does not start relaxing right away. Vertices begin scattered
 * at random positions (EmbeddedGraph::RandomizePositions, the same helper
 * the FR-pairwise embedder uses) and idle there, gently oscillating between
 * a few random anchors, while the user cycles through the mesh's
 * combinatorial faces with the right arrow key and highlights one. Pressing
 * B on a highlighted face pins it as the initial outer boundary and
 * relaxation begins from then on, at which point control reverts to the
 * usual step/fix-boundary/move-image flow. Because it's a damped spring
 * (not a direct barycenter blend), interior vertices can overshoot their
 * equilibrium and spring back before settling; K/J and ;/' retune that feel
 * live.
 *
 * Controls:
 *   right arrow  - before the boundary is fixed: highlight the next face
 *                  (wraps around); after: move the image rectangle right
 *   B            - pin the highlighted face as the outer boundary; before
 *                  relaxation has started this also kicks off relaxation
 *   R            - run one spring-Tutte relaxation step
 *   space        - toggle continuous stepping
 *   K/J          - increase/decrease spring stiffness
 *   '/;          - increase/decrease damping
 *   right click  - highlight the face under the cursor
 *   arrow keys   - move the image rectangle in embedding space (left/up/down
 *                  always move the image; right doubles as face-cycling
 *                  before the boundary is fixed, see above)
 *   =/-          - grow/shrink the image rectangle
 *   0            - full reset: image rectangle back to its initial fit,
 *                  highlight cleared, and the graph back to the jumbled,
 *                  oscillating layout (begun flag cleared) so a new
 *                  boundary face can be picked
 *   I            - toggle showing the image directly in the main graph view
 *                  (hidden by default; also in the View menu, so you can see
 *                  how the image and the live graph overlap), independent of
 *                  the always-on textured preview in the object-overlay panel
 *   F            - fit view
 *   S            - toggle the stats overlay and the object-overlay panel
 *                  together
 *   drag         - pan
 *   scroll       - zoom
 *
 * Usage: textureMapDemo <obj> <image> [screenshot.ppm]
 *
 * When a screenshot path is given, the demo skips the interactive scatter
 * phase (picks the first enumerated face as the boundary immediately),
 * auto-steps spring-Tutte for 300 frames, nudges the image rectangle
 * partway through (so the textured region visibly shifts), takes the
 * screenshot, and exits — this lets the feature be verified
 * non-interactively.
 */

#include "grender/grender.h"
#include "grender/grStepProfiling.h"

#include "gviz.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>
#include <span>
#include <vector>

// The scatter phase seeds vertices inside the same size box as the convex
// polygon spring-Tutte later pins the boundary to (see
// SpringTutte::FixConvexPolygon's radius of 200.0), so confirming a face
// doesn't cause a jarring jump in scale.
#define TEXMAP_SCATTER_BOX_EXTENT 200.0
#define TEXMAP_SCATTER_ANCHOR_COUNT 3
#define TEXMAP_SCATTER_PERIOD_SECS 2.5

/*
 * SpringTutte::Begun() only ever latches true (via SetBoundary()/
 * FixConvexPolygon()) and has no public way back to false. This demo tracks
 * its own `relaxing` flag instead, driving the scatter-vs-relax branch
 * itself; the underlying embedder stays "begun" forever after the first
 * fix, but FixConvexPolygon() supports being called again to re-pin a
 * different boundary, so re-picking a face after a reset still works
 * correctly even though the class's own Begun() no longer round-trips to
 * false.
 */
typedef struct TexMapDemoState {
  grRenderer *r = nullptr;
  gviz::Graph *graph = nullptr;
  gviz::layout::SpringTutte<gviz::Subgraph> *tutte = nullptr;
  grTextureMap *tm = nullptr;
  gviz::layout::FaceEnumerator *enumerator = nullptr;
  size_t currentFaceIndex = 0;
  double scatterElapsed = 0.0;
  bool relaxing = false;
} TexMapDemoState;

static void actionToggleAuto(gviz::layout::EmbeddedGraph &eg, void *userData,
                             const gviz::layout::ActionPayload &payload) {
  (void)eg;
  (void)payload;
  bool *autoStep = (bool *)userData;
  *autoStep = !*autoStep;
  printf("auto step: %s\n", *autoStep ? "on" : "off");
}

// grRenderer's built-in 'S' binding only toggles the stats overlay; here it's
// tied to the object-overlay panel too so hiding one hides both.
static void actionToggleStats(gviz::layout::EmbeddedGraph &eg, void *userData,
                              const gviz::layout::ActionPayload &payload) {
  (void)eg;
  (void)payload;
  grRenderer *r = (grRenderer *)userData;
  bool show = !grRendererStatsShown(r);
  grRendererShowStats(r, show);
  grRendererShowObjOverlay(r, show);
}

// Key bindings fire once per press/OS-repeat event, not once per rendered
// frame, so scaling a step purely by payload->deltaTime (a single frame's
// time) makes each nudge imperceptibly small. Move/scale by a fixed fraction
// of the image rect's *current* size per event instead: every tap gives a
// clearly visible, scale-independent nudge, and holding the key (via GLFW's
// key-repeat) keeps moving it further.
#define TEXMAP_MOVE_STEP_FRACTION 0.08
#define TEXMAP_SCALE_STEP_FACTOR 1.1
#define TEXMAP_SPRING_PARAM_STEP_FACTOR 1.15

static void actionMoveUp(gviz::layout::EmbeddedGraph &eg, void *userData,
                         const gviz::layout::ActionPayload &payload) {
  (void)eg;
  (void)payload;
  double cx, cy, hw, hh;
  grTextureMapGetImageRect((grTextureMap *)userData, &cx, &cy, &hw, &hh);
  grTextureMapMoveImage((grTextureMap *)userData, 0.0, hh * TEXMAP_MOVE_STEP_FRACTION);
}
static void actionMoveDown(gviz::layout::EmbeddedGraph &eg, void *userData,
                           const gviz::layout::ActionPayload &payload) {
  (void)eg;
  (void)payload;
  double cx, cy, hw, hh;
  grTextureMapGetImageRect((grTextureMap *)userData, &cx, &cy, &hw, &hh);
  grTextureMapMoveImage((grTextureMap *)userData, 0.0, -hh * TEXMAP_MOVE_STEP_FRACTION);
}
static void actionMoveLeft(gviz::layout::EmbeddedGraph &eg, void *userData,
                           const gviz::layout::ActionPayload &payload) {
  (void)eg;
  (void)payload;
  double cx, cy, hw, hh;
  grTextureMapGetImageRect((grTextureMap *)userData, &cx, &cy, &hw, &hh);
  grTextureMapMoveImage((grTextureMap *)userData, -hw * TEXMAP_MOVE_STEP_FRACTION, 0.0);
}
static void actionGrow(gviz::layout::EmbeddedGraph &eg, void *userData,
                       const gviz::layout::ActionPayload &payload) {
  (void)eg;
  (void)payload;
  grTextureMapScaleImage((grTextureMap *)userData, TEXMAP_SCALE_STEP_FACTOR);
}
static void actionShrink(gviz::layout::EmbeddedGraph &eg, void *userData,
                         const gviz::layout::ActionPayload &payload) {
  (void)eg;
  (void)payload;
  grTextureMapScaleImage((grTextureMap *)userData, 1.0 / TEXMAP_SCALE_STEP_FACTOR);
}
static void actionStiffnessUp(gviz::layout::EmbeddedGraph &eg, void *userData,
                              const gviz::layout::ActionPayload &payload) {
  (void)eg;
  (void)payload;
  auto *tutte = (gviz::layout::SpringTutte<gviz::Subgraph> *)userData;
  tutte->Configure(tutte->Stiffness() * TEXMAP_SPRING_PARAM_STEP_FACTOR, 0);
  printf("stiffness: %.2f\n", tutte->Stiffness());
}
static void actionStiffnessDown(gviz::layout::EmbeddedGraph &eg, void *userData,
                                const gviz::layout::ActionPayload &payload) {
  (void)eg;
  (void)payload;
  auto *tutte = (gviz::layout::SpringTutte<gviz::Subgraph> *)userData;
  tutte->Configure(tutte->Stiffness() / TEXMAP_SPRING_PARAM_STEP_FACTOR, 0);
  printf("stiffness: %.2f\n", tutte->Stiffness());
}
static void actionDampingUp(gviz::layout::EmbeddedGraph &eg, void *userData,
                            const gviz::layout::ActionPayload &payload) {
  (void)eg;
  (void)payload;
  auto *tutte = (gviz::layout::SpringTutte<gviz::Subgraph> *)userData;
  tutte->Configure(0, tutte->Damping() * TEXMAP_SPRING_PARAM_STEP_FACTOR);
  printf("damping: %.2f\n", tutte->Damping());
}
static void actionDampingDown(gviz::layout::EmbeddedGraph &eg, void *userData,
                              const gviz::layout::ActionPayload &payload) {
  (void)eg;
  (void)payload;
  auto *tutte = (gviz::layout::SpringTutte<gviz::Subgraph> *)userData;
  tutte->Configure(0, tutte->Damping() / TEXMAP_SPRING_PARAM_STEP_FACTOR);
  printf("damping: %.2f\n", tutte->Damping());
}
// 0 resets everything back to the pre-pick state: the image rect snaps back
// to its initial fit, any highlighted face is cleared, and the graph goes
// back to the jumbled/oscillating layout (ds->relaxing = false) so a new
// boundary face can be picked. See TexMapDemoState's doc comment for why
// this is a demo-local flag rather than the embedder's own Begun().
static void actionReset(gviz::layout::EmbeddedGraph &eg, void *userData,
                        const gviz::layout::ActionPayload &payload) {
  (void)eg;
  (void)payload;
  TexMapDemoState *ds = (TexMapDemoState *)userData;
  grTextureMapResetImage(ds->tm);
  grRendererClearHighlight(ds->r);
  ds->relaxing = false;
  ds->scatterElapsed = 0.0;
}

// SpringTutte no longer has FixOuterFace() (it dropped all planarity/
// highlight-derived boundary logic -- see SpringTutte.hpp's class doc: only
// SetBoundary()/FixConvexPolygon(), both taking an explicit vertex list, now
// establish a boundary). This demo already tracks the picked face's own
// vertex list (ds->currentFaceIndex into ds->enumerator->Faces()), so it
// pins that directly via FixConvexPolygon instead of relying on the
// embedder reading back a highlight it no longer has.
static void actionFixOuterFace(gviz::layout::EmbeddedGraph &eg, void *userData,
                               const gviz::layout::ActionPayload &payload) {
  (void)eg;
  (void)payload;
  TexMapDemoState *ds = (TexMapDemoState *)userData;
  std::vector<std::vector<size_t>> &faces = ds->enumerator->Faces();
  if (ds->currentFaceIndex >= faces.size())
    return;
  if (ds->tutte->FixConvexPolygon(faces[ds->currentFaceIndex],
                                  TEXMAP_SCATTER_BOX_EXTENT))
    ds->relaxing = true;
}

// Advances the face cursor by one face, wrapping around once exhausted.
// Returns 0 when ds->currentFaceIndex is valid, -1 if the mesh has no faces
// at all.
static int demoAdvanceFace(TexMapDemoState *ds) {
  std::vector<std::vector<size_t>> &faces = ds->enumerator->Faces();
  if (faces.empty())
    return -1;
  ds->currentFaceIndex = (ds->currentFaceIndex + 1) % faces.size();
  return 0;
}

// Highlights whatever face the cursor currently points at.
static void demoHighlightCurrentFace(TexMapDemoState *ds) {
  std::vector<std::vector<size_t>> &faces = ds->enumerator->Faces();
  if (ds->currentFaceIndex >= faces.size())
    return;
  gviz::Subgraph face =
      gviz::layout::FaceSubgraph(*ds->graph, faces[ds->currentFaceIndex]);
  grRendererSetHighlight(ds->r, face, GR_RGBA8(255, 210, 80, 255),
                         GR_RGBA8(255, 180, 40, 255));
}

// Right-arrow binding: cycles to the next face, of any size, and highlights
// it (wraps around). Meshes vary in what their smallest combinatorial face
// is (triangulated meshes give triangles, quad meshes like Blender exports
// give quads, ...), so no size filtering here — every face is fair game.
static void demoCycleFace(TexMapDemoState *ds) {
  if (demoAdvanceFace(ds) != 0)
    return;
  demoHighlightCurrentFace(ds);
}

// Before the boundary is fixed, the right arrow cycles the face highlight
// instead of moving the (not yet meaningful) image rectangle.
static void actionNextFaceOrMoveRight(gviz::layout::EmbeddedGraph &eg, void *userData,
                                      const gviz::layout::ActionPayload &payload) {
  (void)eg;
  (void)payload;
  TexMapDemoState *ds = (TexMapDemoState *)userData;
  if (!ds->relaxing) {
    demoCycleFace(ds);
    return;
  }
  double cx, cy, hw, hh;
  grTextureMapGetImageRect(ds->tm, &cx, &cy, &hw, &hh);
  grTextureMapMoveImage(ds->tm, hw * TEXMAP_MOVE_STEP_FRACTION, 0.0);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr,
            "usage: textureMapDemo <obj> <image> [screenshot.ppm] [move]\n");
    return 1;
  }
  const char *obj = argv[1];
  const char *imagePath = argv[2];
  const char *screenshotPath = argc > 3 ? argv[3] : NULL;
  // Opt-in, verification-only: when a 4th arg is present, the image rect is
  // nudged partway through the auto-step run so a second screenshot can be
  // diffed against a first (plain) run to show the textured region moving.
  bool doMoveDemo = argc > 4;

  gviz::Graph graph = [&]() -> gviz::Graph {
    try {
      return gviz::io::LoadFromObjFile(obj);
    } catch (const std::exception &e) {
      fprintf(stderr, "failed to load obj '%s': %s\n", obj, e.what());
      exit(1);
    }
  }();
  graph.BuildLayout();

  gviz::Subgraph sg = gviz::Subgraph::CreateFull(graph);

  try {
    gviz::layout::ApplyPlanarRotation(graph, sg);
  } catch (const gviz::NotPlanarError &e) {
    fprintf(stderr, "mesh topology is non-planar: %s\n", e.what());
    return 1;
  } catch (const gviz::LayoutError &e) {
    fprintf(stderr, "planar embedding failed: %s\n", e.what());
    return 1;
  }

  gviz::layout::FaceEnumerator enumerator(graph, sg);

  // SpringTutte<G> no longer takes a Graph& (it dropped the Planar.hpp
  // dependency entirely -- see SpringTutte.hpp's class doc) and no longer
  // has Begin(): there is no more auto-planarity-check/auto-boundary-pin to
  // run up front, so construction goes straight from the Subgraph to
  // FixConvexPolygon (called later, from actionFixOuterFace, once the user
  // -- or screenshot mode -- has picked a face).
  std::optional<gviz::layout::SpringTutte<gviz::Subgraph>> tutteOpt;
  try {
    tutteOpt.emplace(std::move(sg), 2);
  } catch (const std::exception &e) {
    fprintf(stderr, "spring-Tutte init failed: %s\n", e.what());
    return 1;
  }
  gviz::layout::SpringTutte<gviz::Subgraph> &tutte = *tutteOpt;
  tutte.Configure(20, 0.5);

  TexMapDemoState ds;
  ds.tutte = &tutte;
  ds.graph = &graph;
  ds.enumerator = &enumerator;

  // Scatter phase: seed a handful of random anchors per vertex (same random
  // box placement the FR-pairwise embedder uses) and idle-oscillate between
  // them until the user fixes the initial boundary.
  size_t vcount = graph.Size();
  size_t dim = tutte.Dim();
  size_t posN = vcount * dim;
  std::vector<double> scatterAnchors(TEXMAP_SCATTER_ANCHOR_COUNT * posN);
  srand((unsigned int)time(NULL));
  for (size_t i = 0; i < TEXMAP_SCATTER_ANCHOR_COUNT; i++) {
    unsigned int seed = (unsigned int)rand();
    if (seed == 0)
      seed = 1;
    tutte.RandomizePositions(TEXMAP_SCATTER_BOX_EXTENT, seed);
    std::span<const double> pos = tutte.Positions();
    memcpy(scatterAnchors.data() + i * posN, pos.data(), sizeof(double) * posN);
  }
  bool autoStep = false;
  tutte.AddAction("demo.toggleAuto", actionToggleAuto, &autoStep);

  grRendererDesc desc;
  grRendererDescInit(&desc);
  desc.title = "grender - Texture Map (right arrow: pick face, B: fix, R: "
               "step, space: auto, K/J: stiffness, '/;: damping, =/-: scale "
               "image, 0: reset)";
  desc.nodeStyle.radius = 3.0f;
  desc.nodeStyle.fillColor = GR_COLOR(0.55f, 0.78f, 1.0f, 1.0f);
  desc.edgeStyle.color = GR_COLOR(0.45f, 0.55f, 0.75f, 0.45f);
  desc.edgeStyle.width = 1.5f;

  grRenderer *r = grRendererCreate(&desc);
  if (!r) {
    fprintf(stderr, "renderer creation failed\n");
    return 1;
  }

  if (grRendererSetGraph(r, tutte.Structure(), tutte, &graph) < 0) {
    fprintf(stderr, "graph attach failed\n");
    grRendererDestroy(r);
    return 1;
  }

  grTextureMap *tm = grRendererLoadTextureMap(r, tutte, obj, imagePath);
  if (!tm) {
    fprintf(stderr,
            "texture map load failed for obj '%s' / image '%s' (vertex-count "
            "mismatch, bad image, or non-2D embedding)\n",
            obj, imagePath);
    grRendererDestroy(r);
    return 1;
  }
  // The image rect isn't meaningful yet during the scatter/pick phase, and
  // once spring-Tutte is running the always-on object-overlay preview already
  // shows the texture; keep the main-view image hidden until the user asks
  // for it with I.
  grRendererShowTextureMapImage(r, false);

  ds.r = r;
  ds.tm = tm;

  tutte.AddAction("texmap.moveUp", actionMoveUp, tm);
  tutte.AddAction("texmap.moveDown", actionMoveDown, tm);
  tutte.AddAction("texmap.moveLeft", actionMoveLeft, tm);
  tutte.AddAction("texmap.moveRight", actionNextFaceOrMoveRight, &ds);
  tutte.AddAction("texmap.grow", actionGrow, tm);
  tutte.AddAction("texmap.shrink", actionShrink, tm);
  tutte.AddAction("texmap.reset", actionReset, &ds);
  tutte.AddAction("texmap.fixOuterFace", actionFixOuterFace, &ds);
  tutte.AddAction("demo.toggleStats", actionToggleStats, r);
  tutte.AddAction("texmap.stiffnessUp", actionStiffnessUp, &tutte);
  tutte.AddAction("texmap.stiffnessDown", actionStiffnessDown, &tutte);
  tutte.AddAction("texmap.dampingUp", actionDampingUp, &tutte);
  tutte.AddAction("texmap.dampingDown", actionDampingDown, &tutte);

  grRendererBindKey(r, 'R', "springTutte.step");
  grRendererBindKey(r, 'B', "texmap.fixOuterFace");
  grRendererBindKey(r, 'S', "demo.toggleStats");
  grRendererBindKey(r, GR_KEY_SPACE, "demo.toggleAuto");
  grRendererBindKey(r, 'K', "texmap.stiffnessUp");
  grRendererBindKey(r, 'J', "texmap.stiffnessDown");
  grRendererBindKey(r, 'H', "texmap.dampingUp");
  grRendererBindKey(r, 'L', "texmap.dampingDown");
  grRendererBindMouse(r, GR_MOUSE_BUTTON_RIGHT, GR_ACTION_PICK_FACE);
  grRendererBindKey(r, GR_KEY_UP, "texmap.moveUp");
  grRendererBindKey(r, GR_KEY_DOWN, "texmap.moveDown");
  grRendererBindKey(r, GR_KEY_LEFT, "texmap.moveLeft");
  grRendererBindKey(r, GR_KEY_RIGHT, "texmap.moveRight");
  grRendererBindKey(r, '=', "texmap.grow");
  grRendererBindKey(r, '-', "texmap.shrink");
  grRendererBindKey(r, '0', "texmap.reset");

  const size_t stepsBeforeShot = screenshotPath ? 300 : SIZE_MAX;
  const size_t moveAtStep = 150;
  size_t totalSteps = 0;

  if (screenshotPath) {
    // Skip the interactive scatter/pick phase: highlight the first
    // enumerated face and fix it immediately so relaxation can auto-run.
    demoCycleFace(&ds);
    std::vector<std::vector<size_t>> &faces = enumerator.Faces();
    if (ds.currentFaceIndex < faces.size() &&
        ds.tutte->FixConvexPolygon(faces[ds.currentFaceIndex],
                                   TEXMAP_SCATTER_BOX_EXTENT))
      ds.relaxing = true;
    else
      fprintf(stderr,
              "warning: failed to auto-fix initial boundary for screenshot\n");
  }

  grRendererShowStats(r, false);
  grRendererShowObjOverlay(r, false);

  while (grRendererFrame(r)) {
    if (!ds.relaxing) {
      ds.scatterElapsed += grRendererDeltaTime(r);
      double phase = fmod(ds.scatterElapsed / TEXMAP_SCATTER_PERIOD_SECS,
                          (double)TEXMAP_SCATTER_ANCHOR_COUNT);
      size_t idx0 = (size_t)phase;
      size_t idx1 = (idx0 + 1) % TEXMAP_SCATTER_ANCHOR_COUNT;
      double t = phase - (double)idx0;
      double s = t * t * (3.0 - 2.0 * t); // smoothstep ease in/out
      double pos[2];
      for (size_t u = 0; u < vcount; u++) {
        const double *a = scatterAnchors.data() + idx0 * posN + u * dim;
        const double *b = scatterAnchors.data() + idx1 * posN + u * dim;
        pos[0] = a[0] + (b[0] - a[0]) * s;
        pos[1] = a[1] + (b[1] - a[1]) * s;
        tutte.SetVPosition(u, pos);
      }
    } else if (autoStep && !tutte.Converged()) {
      double dt = grRendererDeltaTime(r);
      for (size_t i = 0; i < 20; i++) {
        GR_PROF_STEP_BEGIN();
        tutte.Step(dt);
        GR_PROF_STEP_END();
      }
    }

    if (screenshotPath) {
      totalSteps++;
      if (doMoveDemo && totalSteps == moveAtStep) {
        // Demonstrate a live-updating image rect: shift it by a fixed
        // amount so the second screenshot's textured region visibly moves
        // relative to the first.
        double cx, cy, hw, hh;
        grTextureMapGetImageRect(tm, &cx, &cy, &hw, &hh);
        grTextureMapSetImageRect(tm, cx + hw * 0.6, cy, hw, hh);
      }
      if (totalSteps >= stepsBeforeShot) {
        grRendererFitView(r);
        grRendererFrame(r);
        if (grRendererSaveScreenshot(r, screenshotPath) == 0)
          printf("screenshot saved to %s\n", screenshotPath);
        else
          fprintf(stderr, "screenshot failed\n");
        grRendererRequestClose(r);
      }
    }
  }

  grRendererDestroy(r);
  return 0;
}
