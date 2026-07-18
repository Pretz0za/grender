#include "grProfiling.h"

#ifdef GRENDER_ENABLE_PROFILING

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double *g_frameTimesMs = NULL;
static size_t g_frameCount = 0;
static size_t g_frameCapacity = 0;
static double g_frameStart = 0.0;
static int g_frameActive = 0;

static double nowSeconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void grProfFrameBegin(void) {
  g_frameStart = nowSeconds();
  g_frameActive = 1;
}

void grProfFrameEnd(void) {
  if (!g_frameActive)
    return;
  g_frameActive = 0;
  double elapsedMs = (nowSeconds() - g_frameStart) * 1000.0;
  if (g_frameCount >= g_frameCapacity) {
    size_t newCapacity = g_frameCapacity ? g_frameCapacity * 2 : 1024;
    double *newArr = realloc(g_frameTimesMs, newCapacity * sizeof(double));
    if (!newArr)
      return;
    g_frameTimesMs = newArr;
    g_frameCapacity = newCapacity;
  }
  g_frameTimesMs[g_frameCount++] = elapsedMs;
}

static int compareDouble(const void *a, const void *b) {
  double da = *(const double *)a, db = *(const double *)b;
  return (da > db) - (da < db);
}

static double percentile(const double *sorted, size_t n, double p) {
  size_t idx = (size_t)(p * (double)(n - 1));
  return sorted[idx];
}

static void grProfPrintReport(void) {
  fprintf(stderr, "\n=== grender frame time report ===\n");
  fprintf(stderr, "Frames rendered: %zu\n", g_frameCount);
  if (g_frameCount == 0) {
    fprintf(stderr, "==================================\n");
    return;
  }

  double *sorted = malloc(g_frameCount * sizeof(double));
  if (!sorted) {
    fprintf(stderr, "==================================\n");
    return;
  }
  memcpy(sorted, g_frameTimesMs, g_frameCount * sizeof(double));
  qsort(sorted, g_frameCount, sizeof(double), compareDouble);

  double sum = 0.0;
  for (size_t i = 0; i < g_frameCount; i++)
    sum += g_frameTimesMs[i];

  fprintf(stderr, "Frame render time (ms):\n");
  fprintf(stderr, "  avg: %.3f\n", sum / (double)g_frameCount);
  fprintf(stderr, "  p50: %.3f\n", percentile(sorted, g_frameCount, 0.50));
  fprintf(stderr, "  p95: %.3f\n", percentile(sorted, g_frameCount, 0.95));
  fprintf(stderr, "  p99: %.3f\n", percentile(sorted, g_frameCount, 0.99));
  fprintf(stderr, "  max: %.3f\n", sorted[g_frameCount - 1]);
  fprintf(stderr, "==================================\n");

  free(sorted);
}

__attribute__((constructor)) static void grProfInit(void) {
  atexit(grProfPrintReport);
}

#endif // GRENDER_ENABLE_PROFILING
