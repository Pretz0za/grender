/**
 * Minimal Wavefront .obj mesh parser for the object overlay. Only 'v'
 * (vertex position) and 'f' (face) lines are read; texture/normal indices
 * on face tokens are ignored and per-vertex normals are instead computed as
 * the area-weighted average of adjacent face normals. This is deliberately
 * independent of gviz's gvizGraphLoadFromObjFile, which discards vertex
 * positions and only reconstructs face-boundary edges for graph embedding.
 */
#include "grInternal.h"

#include "ds/gvizArray.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GR_LOG(...) fprintf(stderr, "[grender] " __VA_ARGS__)
#define GR_OBJ_MAX_FACE_VERTS 256

typedef struct grVec3d {
  double x, y, z;
} grVec3d;

/** Parses the leading integer of a face token ("12", "12/5", "12/5/3",
 *  "12//3", or negative/relative forms) into a 0-based vertex index. */
static int parseFaceVertexIndex(const char *tok, size_t vertexCount,
                                size_t *out) {
  char *end = NULL;
  long v = strtol(tok, &end, 10);
  if (end == tok || v == 0)
    return -1;
  long idx = v > 0 ? v - 1 : (long)vertexCount + v;
  if (idx < 0 || (size_t)idx >= vertexCount)
    return -1;
  *out = (size_t)idx;
  return 0;
}

static int parseFaceLine(const char *p, size_t vertexCount,
                         gvizArray *indices) {
  size_t faceVerts[GR_OBJ_MAX_FACE_VERTS];
  size_t faceLen = 0;

  while (*p) {
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '\0' || *p == '\n' || *p == '\r')
      break;

    if (faceLen < GR_OBJ_MAX_FACE_VERTS) {
      size_t idx;
      if (parseFaceVertexIndex(p, vertexCount, &idx) < 0)
        return -1;
      faceVerts[faceLen++] = idx;
    }
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
      p++;
  }

  // Fan-triangulate the polygon (v0, vi, vi+1).
  for (size_t i = 1; i + 1 < faceLen; i++) {
    uint32_t a = (uint32_t)faceVerts[0];
    uint32_t b = (uint32_t)faceVerts[i];
    uint32_t c = (uint32_t)faceVerts[i + 1];
    if (gvizArrayPush(indices, &a) < 0 || gvizArrayPush(indices, &b) < 0 ||
        gvizArrayPush(indices, &c) < 0)
      return -1;
  }
  return 0;
}

static void computeNormals(const grVec3d *pos, size_t vertexCount,
                           const uint32_t *indices, size_t indexCount,
                           float *normals) {
  memset(normals, 0, sizeof(float) * vertexCount * 3);

  for (size_t t = 0; t + 2 < indexCount; t += 3) {
    uint32_t i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
    double e1[3], e2[3], n[3];
    e1[0] = pos[i1].x - pos[i0].x;
    e1[1] = pos[i1].y - pos[i0].y;
    e1[2] = pos[i1].z - pos[i0].z;
    e2[0] = pos[i2].x - pos[i0].x;
    e2[1] = pos[i2].y - pos[i0].y;
    e2[2] = pos[i2].z - pos[i0].z;
    n[0] = e1[1] * e2[2] - e1[2] * e2[1];
    n[1] = e1[2] * e2[0] - e1[0] * e2[2];
    n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    for (int d = 0; d < 3; d++) {
      normals[i0 * 3 + d] += (float)n[d];
      normals[i1 * 3 + d] += (float)n[d];
      normals[i2 * 3 + d] += (float)n[d];
    }
  }

  for (size_t i = 0; i < vertexCount; i++) {
    float *n = normals + i * 3;
    float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 1e-8f) {
      n[0] /= len;
      n[1] /= len;
      n[2] /= len;
    } else {
      n[0] = 0.0f;
      n[1] = 0.0f;
      n[2] = 1.0f;
    }
  }
}

int grObjMeshLoad(const char *path, grObjMesh *out) {
  memset(out, 0, sizeof(*out));
  if (!path)
    return -1;

  FILE *file = fopen(path, "r");
  if (!file) {
    GR_LOG("failed to open obj file '%s'\n", path);
    return -1;
  }

  gvizArray positions, indices;
  if (gvizArrayInit(&positions, sizeof(grVec3d)) < 0 ||
      gvizArrayInit(&indices, sizeof(uint32_t)) < 0) {
    gvizArrayRelease(&positions);
    fclose(file);
    return -1;
  }

  char *line = NULL;
  size_t lineCap = 0;
  int err = 0;

  while (getline(&line, &lineCap, file) != -1) {
    const char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;

    if (p[0] == 'v' && (p[1] == ' ' || p[1] == '\t')) {
      grVec3d v;
      if (sscanf(p + 1, "%lf %lf %lf", &v.x, &v.y, &v.z) != 3) {
        err = -1;
        break;
      }
      if (gvizArrayPush(&positions, &v) < 0) {
        err = -1;
        break;
      }
      continue;
    }

    if (p[0] == 'f' && (p[1] == ' ' || p[1] == '\t')) {
      if (parseFaceLine(p + 1, positions.count, &indices) < 0) {
        err = -1;
        break;
      }
    }
  }

  free(line);
  fclose(file);

  if (err < 0 || positions.count == 0 || indices.count == 0) {
    GR_LOG("failed to parse obj file '%s'\n", path);
    gvizArrayRelease(&positions);
    gvizArrayRelease(&indices);
    return -1;
  }

  size_t vertexCount = positions.count;
  size_t indexCount = indices.count;
  const uint32_t *idx = indices.arr;

  // .obj files are Y-up by convention; grender's world is Z-up, so rotate
  // the mesh +90 degrees about X (y,z) -> (-z,y) to stand it upright facing
  // the overlay camera instead of lying on its back.
  grVec3d *posMut = positions.arr;
  for (size_t i = 0; i < vertexCount; i++) {
    double y = posMut[i].y, z = posMut[i].z;
    posMut[i].y = -z;
    posMut[i].z = y;
  }
  const grVec3d *pos = positions.arr;

  float *outPositions = malloc(sizeof(float) * vertexCount * 3);
  float *outNormals = malloc(sizeof(float) * vertexCount * 3);
  uint32_t *outIndices = malloc(sizeof(uint32_t) * indexCount);
  if (!outPositions || !outNormals || !outIndices) {
    free(outPositions);
    free(outNormals);
    free(outIndices);
    gvizArrayRelease(&positions);
    gvizArrayRelease(&indices);
    return -1;
  }

  double bmin[3] = {INFINITY, INFINITY, INFINITY};
  double bmax[3] = {-INFINITY, -INFINITY, -INFINITY};
  for (size_t i = 0; i < vertexCount; i++) {
    double c[3] = {pos[i].x, pos[i].y, pos[i].z};
    for (int d = 0; d < 3; d++) {
      outPositions[i * 3 + d] = (float)c[d];
      if (c[d] < bmin[d])
        bmin[d] = c[d];
      if (c[d] > bmax[d])
        bmax[d] = c[d];
    }
  }
  memcpy(outIndices, idx, sizeof(uint32_t) * indexCount);
  computeNormals(pos, vertexCount, idx, indexCount, outNormals);

  gvizArrayRelease(&positions);
  gvizArrayRelease(&indices);

  out->positions = outPositions;
  out->normals = outNormals;
  out->indices = outIndices;
  out->vertexCount = vertexCount;
  out->indexCount = indexCount;
  memcpy(out->bmin, bmin, sizeof(bmin));
  memcpy(out->bmax, bmax, sizeof(bmax));
  return 0;
}

void grObjMeshRelease(grObjMesh *mesh) {
  if (!mesh)
    return;
  free(mesh->positions);
  free(mesh->normals);
  free(mesh->indices);
  memset(mesh, 0, sizeof(*mesh));
}
