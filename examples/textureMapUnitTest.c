/**
 * Standalone CPU-only test for the texture-map math in src/grTextureMap.c:
 * grTextureMapComputeUV and grTextureMapComputeFaceValidity. No GPU, no
 * window, no gviz dependency beyond the plain data types these functions
 * take. Not wired into CMakeLists.txt yet (a later pass adds the
 * `textureMapUnitTest` executable target alongside the demo).
 */
#include "../src/grInternal.h"

#include <assert.h>
#include <math.h>

static int approxEq(double a, double b, double eps) {
  return fabs(a - b) < eps;
}

static void testSingleVertexAtCenter(void) {
  double center[2] = {0.0, 0.0};
  double halfExtent[2] = {1.0, 1.0};
  double pos[2] = {0.0, 0.0};
  float uv[2];
  uint32_t inside[1];

  grTextureMapComputeUV(pos, 1, center, halfExtent, uv, inside);

  assert(approxEq(uv[0], 0.5, 1e-6));
  assert(approxEq(uv[1], 0.5, 1e-6));
  assert(inside[0] == 1u);
}

static void testVerticesOutsideEachSide(void) {
  double center[2] = {0.0, 0.0};
  double halfExtent[2] = {1.0, 1.0};
  double pos[8] = {
      -2.0, 0.0, // left of rect
      2.0,  0.0, // right of rect
      0.0,  -2.0, // below rect
      0.0,  2.0,  // above rect
  };
  float uv[8];
  uint32_t inside[4];

  grTextureMapComputeUV(pos, 4, center, halfExtent, uv, inside);

  assert(inside[0] == 0u);
  assert(inside[1] == 0u);
  assert(inside[2] == 0u);
  assert(inside[3] == 0u);
}

// Quad face a,b,c,d fan-triangulated (matching parseFaceLine in
// grObjMesh.c) into two triangles sharing v0: (a,b,c) and (a,c,d).
static const uint32_t kQuadIndices[6] = {0, 1, 2, 0, 2, 3};
static const uint32_t kQuadFaceIds[2] = {0, 0};

static void testQuadFullyInside(void) {
  // A small square around the origin, well within the [-1,1]^2 rect.
  double pos[8] = {
      -0.1, -0.1, // a
      0.1,  -0.1, // b
      0.1,  0.1,  // c
      -0.1, 0.1,  // d
  };
  double center[2] = {0.0, 0.0};
  double halfExtent[2] = {1.0, 1.0};
  float uv[8];
  uint32_t inside[4];
  grTextureMapComputeUV(pos, 4, center, halfExtent, uv, inside);
  assert(inside[0] && inside[1] && inside[2] && inside[3]);

  uint32_t faceValid[1];
  uint32_t triValid[2];
  grTextureMapComputeFaceValidity(inside, 4, kQuadIndices, kQuadFaceIds, 2, 1,
                                  faceValid, triValid);

  assert(faceValid[0] == 1u);
  assert(triValid[0] == 1u);
  assert(triValid[1] == 1u);
}

// The load-bearing n-gon check: vertex d (index 3) is only referenced by the
// *second* fan triangle (a,c,d), not the first (a,b,c). A naive per-triangle
// check would leave triValid[0] (the untouched triangle) wrongly valid; the
// correct behavior is that faceValid gates both triangles, since d outside
// invalidates the whole quad.
static void testQuadOneVertexOutsideOnlyInSecondTriangle(void) {
  double pos[8] = {
      -0.1, -0.1, // a - inside
      0.1,  -0.1, // b - inside
      0.1,  0.1,  // c - inside
      5.0,  5.0,  // d - outside, only used by triangle (a,c,d)
  };
  double center[2] = {0.0, 0.0};
  double halfExtent[2] = {1.0, 1.0};
  float uv[8];
  uint32_t inside[4];
  grTextureMapComputeUV(pos, 4, center, halfExtent, uv, inside);
  assert(inside[0] == 1u);
  assert(inside[1] == 1u);
  assert(inside[2] == 1u);
  assert(inside[3] == 0u);

  uint32_t faceValid[1];
  uint32_t triValid[2];
  grTextureMapComputeFaceValidity(inside, 4, kQuadIndices, kQuadFaceIds, 2, 1,
                                  faceValid, triValid);

  assert(faceValid[0] == 0u);
  // Both triangles must be invalidated, even triangle 0 = (a,b,c), which
  // never directly references the outside vertex d.
  assert(triValid[0] == 0u);
  assert(triValid[1] == 0u);
}

static void testQuadFullyOutside(void) {
  double pos[8] = {
      5.0, 5.0, // a
      6.0, 5.0, // b
      6.0, 6.0, // c
      5.0, 6.0, // d
  };
  double center[2] = {0.0, 0.0};
  double halfExtent[2] = {1.0, 1.0};
  float uv[8];
  uint32_t inside[4];
  grTextureMapComputeUV(pos, 4, center, halfExtent, uv, inside);
  assert(!inside[0] && !inside[1] && !inside[2] && !inside[3]);

  uint32_t faceValid[1];
  uint32_t triValid[2];
  grTextureMapComputeFaceValidity(inside, 4, kQuadIndices, kQuadFaceIds, 2, 1,
                                  faceValid, triValid);

  assert(faceValid[0] == 0u);
  assert(triValid[0] == 0u);
  assert(triValid[1] == 0u);
}

int main(void) {
  testSingleVertexAtCenter();
  testVerticesOutsideEachSide();
  testQuadFullyInside();
  testQuadOneVertexOutsideOnlyInSecondTriangle();
  testQuadFullyOutside();
  return 0;
}
