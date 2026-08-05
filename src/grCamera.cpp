#include "grInternal.h"

#include <math.h>
#include <string.h>

#define GR_FOVY (M_PI / 4.0)
#define GR_MIN_DISTANCE 1e-6
#define GR_MAX_PITCH (M_PI / 2.0 - 0.01)

void grCameraInit2D(grCamera *cam) {
  memset(cam, 0, sizeof(*cam));
  cam->perspective = false;
  cam->distance = 100.0;
}

void grCameraInit3D(grCamera *cam) {
  memset(cam, 0, sizeof(*cam));
  cam->perspective = true;
  cam->distance = 100.0;
  cam->yaw = -M_PI / 2.0;
  cam->pitch = M_PI / 6.0;
}

void grCameraOrbit(grCamera *cam, double dYaw, double dPitch) {
  if (!cam->perspective)
    return;
  cam->yaw += dYaw;
  cam->pitch += dPitch;
  if (cam->pitch > GR_MAX_PITCH)
    cam->pitch = GR_MAX_PITCH;
  if (cam->pitch < -GR_MAX_PITCH)
    cam->pitch = -GR_MAX_PITCH;
}

void grCameraZoom(grCamera *cam, double factor) {
  cam->distance *= factor;
  if (cam->distance < GR_MIN_DISTANCE)
    cam->distance = GR_MIN_DISTANCE;
}

/** World-space unit axes spanning the view plane and the view direction. */
static void cameraBasis(const grCamera *cam, double right[3], double up[3],
                        double forward[3]) {
  if (!cam->perspective) {
    right[0] = 1.0, right[1] = 0.0, right[2] = 0.0;
    up[0] = 0.0, up[1] = 1.0, up[2] = 0.0;
    forward[0] = 0.0, forward[1] = 0.0, forward[2] = -1.0;
    return;
  }
  double cy = cos(cam->yaw), sy = sin(cam->yaw);
  double cp = cos(cam->pitch), sp = sin(cam->pitch);
  // Eye sits at target - forward * distance; Z is world "up" for the orbit.
  forward[0] = -cy * cp;
  forward[1] = -sy * cp;
  forward[2] = -sp;
  right[0] = -sy;
  right[1] = cy;
  right[2] = 0.0;
  // up = right x forward (orthonormal, right x up == -forward)
  up[0] = right[1] * forward[2] - right[2] * forward[1];
  up[1] = right[2] * forward[0] - right[0] * forward[2];
  up[2] = right[0] * forward[1] - right[1] * forward[0];
}

void grCameraPanPixels(grCamera *cam, double dxPx, double dyPx,
                       double viewportHPx) {
  double right[3], up[3], forward[3];
  cameraBasis(cam, right, up, forward);

  // World units per pixel at the target plane.
  double worldPerPx;
  if (cam->perspective)
    worldPerPx = 2.0 * cam->distance * tan(GR_FOVY / 2.0) / viewportHPx;
  else
    worldPerPx = cam->distance / viewportHPx;

  for (int i = 0; i < 3; i++) {
    cam->target[i] -= right[i] * dxPx * worldPerPx;
    cam->target[i] += up[i] * dyPx * worldPerPx; // window y grows downward
  }
}

static void mat4Multiply(const double a[16], const double b[16], double out[16]) {
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++) {
      double sum = 0.0;
      for (int k = 0; k < 4; k++)
        sum += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = sum;
    }
}

void grCameraFrameCompute(const grCamera *cam, double viewportWPx,
                          double viewportHPx, grCameraFrame *out) {
  double aspect = viewportWPx / (viewportHPx > 0 ? viewportHPx : 1.0);
  double right[3], up[3], forward[3];
  cameraBasis(cam, right, up, forward);

  double eye[3];
  for (int i = 0; i < 3; i++)
    eye[i] = cam->target[i] - forward[i] * (cam->perspective ? cam->distance : 1.0);

  // View matrix: rows are the camera basis (look-at with basis above).
  double view[16] = {0};
  for (int i = 0; i < 3; i++) {
    view[i * 4 + 0] = right[i];
    view[i * 4 + 1] = up[i];
    view[i * 4 + 2] = -forward[i];
  }
  view[15] = 1.0;
  view[12] = -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]);
  view[13] = -(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2]);
  view[14] = forward[0] * eye[0] + forward[1] * eye[1] + forward[2] * eye[2];

  // Projection with WebGPU clip conventions (z in [0, 1]).
  double proj[16] = {0};
  if (cam->perspective) {
    double f = 1.0 / tan(GR_FOVY / 2.0);
    double zn = cam->distance * 1e-3;
    double zf = cam->distance * 1e4;
    proj[0] = f / aspect;
    proj[5] = f;
    proj[10] = zf / (zn - zf);
    proj[11] = -1.0;
    proj[14] = zn * zf / (zn - zf);
  } else {
    // cam->distance is the visible world height.
    double halfH = cam->distance / 2.0;
    double halfW = halfH * aspect;
    double zn = -1e6, zf = 1e6;
    proj[0] = 1.0 / halfW;
    proj[5] = 1.0 / halfH;
    proj[10] = 1.0 / (zn - zf);
    proj[14] = zn / (zn - zf);
    proj[15] = 1.0;
  }

  double viewProj[16];
  mat4Multiply(proj, view, viewProj);
  for (int i = 0; i < 16; i++)
    out->viewProj[i] = (float)viewProj[i];
  for (int i = 0; i < 3; i++) {
    out->camRight[i] = (float)right[i];
    out->camUp[i] = (float)up[i];
    out->eye[i] = eye[i];
    out->forward[i] = forward[i];
  }
  out->proj11 = (float)proj[5];
}

static int unprojectEmbeddingPlane(const float viewProj[16], double xPx,
                                 double yPx, double viewportWPx,
                                 double viewportHPx, double *worldX,
                                 double *worldY) {
  double ndcX = 2.0 * xPx / viewportWPx - 1.0;
  double ndcY = 1.0 - 2.0 * yPx / viewportHPx;

  double a00 = (double)viewProj[0] - ndcX * (double)viewProj[3];
  double a01 = (double)viewProj[4] - ndcX * (double)viewProj[7];
  double b0 = ndcX * (double)viewProj[15] - (double)viewProj[12];

  double a10 = (double)viewProj[1] - ndcY * (double)viewProj[3];
  double a11 = (double)viewProj[5] - ndcY * (double)viewProj[7];
  double b1 = ndcY * (double)viewProj[15] - (double)viewProj[13];

  double det = a00 * a11 - a01 * a10;
  if (fabs(det) < 1e-12)
    return -1;

  *worldX = (b0 * a11 - b1 * a01) / det;
  *worldY = (a00 * b1 - a10 * b0) / det;
  return 0;
}

void grCameraUnproject(const grCamera *cam, const grCameraFrame *frame,
                       double xPx, double yPx, double viewportWPx,
                       double viewportHPx, double *worldX, double *worldY) {
  if (frame &&
      unprojectEmbeddingPlane(frame->viewProj, xPx, yPx, viewportWPx,
                              viewportHPx, worldX, worldY) == 0)
    return;

  // NDC of the cursor; window y grows downward.
  double nx = 2.0 * xPx / viewportWPx - 1.0;
  double ny = 1.0 - 2.0 * yPx / viewportHPx;

  double right[3], up[3], forward[3];
  cameraBasis(cam, right, up, forward);

  if (!cam->perspective) {
    double aspect = viewportWPx / (viewportHPx > 0 ? viewportHPx : 1.0);
    double halfH = cam->distance / 2.0;
    *worldX = cam->target[0] + nx * halfH * aspect;
    *worldY = cam->target[1] + ny * halfH;
    return;
  }

  // Ray through the cursor, intersected with the plane through the target
  // perpendicular to the view direction.
  double tanHalf = tan(GR_FOVY / 2.0);
  double aspect = viewportWPx / (viewportHPx > 0 ? viewportHPx : 1.0);
  double dir[3];
  for (int i = 0; i < 3; i++)
    dir[i] = forward[i] + right[i] * nx * tanHalf * aspect +
             up[i] * ny * tanHalf;

  double denom = 0.0, num = 0.0;
  for (int i = 0; i < 3; i++) {
    denom += dir[i] * forward[i];
    num += (cam->target[i] - frame->eye[i]) * forward[i];
  }
  double t = denom != 0.0 ? num / denom : 0.0;
  *worldX = frame->eye[0] + dir[0] * t;
  *worldY = frame->eye[1] + dir[1] * t;
}

void grCameraFitBox(grCamera *cam, const double bmin[3], const double bmax[3],
                    double viewportWPx, double viewportHPx) {
  double aspect = viewportWPx / (viewportHPx > 0 ? viewportHPx : 1.0);
  double size[3], maxExtent = GR_MIN_DISTANCE;
  for (int i = 0; i < 3; i++) {
    cam->target[i] = (bmin[i] + bmax[i]) / 2.0;
    size[i] = bmax[i] - bmin[i];
    if (size[i] > maxExtent)
      maxExtent = size[i];
  }

  if (cam->perspective) {
    // Fit the bounding sphere with ~10% margin.
    double radius =
        sqrt(size[0] * size[0] + size[1] * size[1] + size[2] * size[2]) / 2.0;
    if (radius < GR_MIN_DISTANCE)
      radius = GR_MIN_DISTANCE;
    double fit = fmin(tan(GR_FOVY / 2.0), tan(GR_FOVY / 2.0) * aspect);
    cam->distance = 1.1 * radius / fit;
  } else {
    double neededH = size[1];
    if (aspect > 0 && size[0] / aspect > neededH)
      neededH = size[0] / aspect;
    cam->distance = neededH > GR_MIN_DISTANCE ? neededH * 1.1 : 1.0;
  }
}
