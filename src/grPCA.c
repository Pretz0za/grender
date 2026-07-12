#include "grInternal.h"

#include <lapacke.h>

#include <math.h>
#include <string.h>

#define GR_PCA_OUT_DIM 3
#define GR_PCA_MAX_SRC_DIM 16

static void grPCACopyLeading(size_t n, size_t srcDim, const double *src,
                             float *dst) {
  for (size_t i = 0; i < n; i++) {
    const double *p = src + i * srcDim;
    float *q = dst + i * GR_PCA_OUT_DIM;
    q[0] = (float)p[0];
    q[1] = srcDim > 1 ? (float)p[1] : 0.0f;
    q[2] = srcDim > 2 ? (float)p[2] : 0.0f;
  }
}

static void grPCAAlignBasis(size_t srcDim, double *cov, size_t pcRows[3],
                            const double *prevBasis) {
  if (!prevBasis)
    return;
  for (size_t k = 0; k < GR_PCA_OUT_DIM; k++) {
    double *ev = cov + pcRows[k] * srcDim;
    const double *prev = prevBasis + k * srcDim;
    double dot = 0.0;
    for (size_t d = 0; d < srcDim; d++)
      dot += ev[d] * prev[d];
    if (dot < 0.0)
      for (size_t d = 0; d < srcDim; d++)
        ev[d] = -ev[d];
  }
}

/**
 * Projects @p n points from @p srcDim dimensions to 3D with PCA on the
 * covariance of the centered data. @p src and @p dst are vertex-major
 * (n * srcDim and n * 3 respectively).
 *
 * When @p basisOut is non-NULL it receives the three row-major eigenvectors
 * used (srcDim * 3 doubles). If @p basisIn matches a prior @p basisOut,
 * eigenvector signs are flipped to stay consistent and reduce view jumps.
 *
 * @return 0 on success, -1 on failure or unsupported @p srcDim.
 */
int grPCAProjectTo3(const double *src, size_t n, size_t srcDim, float *dst,
                    double *basisOut, const double *basisIn) {
  if (!src || !dst || n == 0)
    return -1;
  if (srcDim <= GR_PCA_OUT_DIM) {
    grPCACopyLeading(n, srcDim, src, dst);
    if (basisOut)
      memset(basisOut, 0, sizeof(double) * srcDim * GR_PCA_OUT_DIM);
    return 0;
  }
  if (n < 2 || srcDim > GR_PCA_MAX_SRC_DIM)
    return -1;

  double mean[GR_PCA_MAX_SRC_DIM];
  memset(mean, 0, sizeof(double) * srcDim);

  for (size_t i = 0; i < n; i++) {
    const double *p = src + i * srcDim;
    for (size_t d = 0; d < srcDim; d++)
      mean[d] += p[d];
  }
  for (size_t d = 0; d < srcDim; d++)
    mean[d] /= (double)n;

  double cov[GR_PCA_MAX_SRC_DIM * GR_PCA_MAX_SRC_DIM];
  memset(cov, 0, sizeof(cov));
  for (size_t i = 0; i < n; i++) {
    const double *p = src + i * srcDim;
    for (size_t a = 0; a < srcDim; a++) {
      double da = p[a] - mean[a];
      for (size_t b = a; b < srcDim; b++)
        cov[a * srcDim + b] += da * (p[b] - mean[b]);
    }
  }
  double inv = 1.0 / (double)(n - 1);
  for (size_t a = 0; a < srcDim; a++) {
    for (size_t b = a; b < srcDim; b++) {
      cov[a * srcDim + b] *= inv;
      if (b != a)
        cov[b * srcDim + a] = cov[a * srcDim + b];
    }
  }

  double evals[GR_PCA_MAX_SRC_DIM];
  lapack_int info =
      LAPACKE_dsyev(LAPACK_ROW_MAJOR, 'V', 'U', (lapack_int)srcDim, cov,
                    (lapack_int)srcDim, evals);
  if (info != 0)
    return -1;

  size_t pcRows[GR_PCA_OUT_DIM];
  for (size_t k = 0; k < GR_PCA_OUT_DIM; k++)
    pcRows[k] = srcDim - GR_PCA_OUT_DIM + k;

  grPCAAlignBasis(srcDim, cov, pcRows, basisIn);

  if (basisOut) {
    for (size_t k = 0; k < GR_PCA_OUT_DIM; k++)
      memcpy(basisOut + k * srcDim, cov + pcRows[k] * srcDim,
             sizeof(double) * srcDim);
  }

  for (size_t i = 0; i < n; i++) {
    const double *p = src + i * srcDim;
    float *q = dst + i * GR_PCA_OUT_DIM;
    for (size_t k = 0; k < GR_PCA_OUT_DIM; k++) {
      const double *ev = cov + pcRows[k] * srcDim;
      double sum = 0.0;
      for (size_t d = 0; d < srcDim; d++)
        sum += (p[d] - mean[d]) * ev[d];
      q[k] = (float)sum;
    }
  }
  return 0;
}
