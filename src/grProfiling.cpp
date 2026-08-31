#include "grProfiling.h"
#include "grender/grStepProfiling.h"

#if defined(GRENDER_ENABLE_PROFILING) || defined(GRENDER_ENABLE_STEP_PROFILING)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double nowSeconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#endif // GRENDER_ENABLE_PROFILING || GRENDER_ENABLE_STEP_PROFILING

#ifdef GRENDER_ENABLE_PROFILING

static double *g_frameTimesMs = NULL;
static size_t g_frameCount = 0;
static size_t g_frameCapacity = 0;
static double g_frameStart = 0.0;
static int g_frameActive = 0;

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

#ifdef GRENDER_ENABLE_STEP_PROFILING

#include <vector>

// Unlike the frame-time buffer above, this is new code rather than existing
// style being left alone, so it follows this repo's normal convention
// (std::vector, not a hand-rolled malloc/realloc-doubling array).
static std::vector<double> g_stepTimesMs;
static double g_stepStart = 0.0;
static bool g_stepActive = false;

void grProfStepBegin(void) {
  g_stepStart = nowSeconds();
  g_stepActive = true;
}

void grProfStepEnd(void) {
  if (!g_stepActive)
    return;
  g_stepActive = false;
  g_stepTimesMs.push_back((nowSeconds() - g_stepStart) * 1000.0);
}

static void grProfStepPrintReport(void) {
  fprintf(stderr, "\n=== grender step() time report ===\n");
  fprintf(stderr, "Steps taken: %zu\n", g_stepTimesMs.size());
  if (!g_stepTimesMs.empty()) {
    double sum = 0.0, lo = g_stepTimesMs[0], hi = g_stepTimesMs[0];
    for (double t : g_stepTimesMs) {
      sum += t;
      if (t < lo)
        lo = t;
      if (t > hi)
        hi = t;
    }
    fprintf(stderr, "Step time (ms):\n");
    fprintf(stderr, "  avg: %.3f\n", sum / (double)g_stepTimesMs.size());
    fprintf(stderr, "  min: %.3f\n", lo);
    fprintf(stderr, "  max: %.3f\n", hi);
  }
  fprintf(stderr, "===================================\n");
}

// Deliberately not the constructor-attribute + atexit() pattern the frame
// profiler above uses: that pattern only works because g_frameTimesMs is a
// raw malloc'd pointer with no destructor to race against. g_stepTimesMs
// does have one, and atexit() callbacks run interleaved with static
// destructors in registration order -- if grProfStepPrintReport() were
// registered via atexit() from a constructor-attribute function, it can run
// *after* g_stepTimesMs's own static destructor already froze it, printing
// zeroes (observed empirically: the vector was reliably empty at report
// time). A guard object declared after g_stepTimesMs sidesteps the ordering
// question entirely -- C++ guarantees static objects in one translation
// unit are destroyed in the reverse of their construction order, so this
// guard's destructor always runs while g_stepTimesMs is still alive.
namespace {
struct StepReportGuard {
  ~StepReportGuard() { grProfStepPrintReport(); }
};
static StepReportGuard g_stepReportGuard;
} // namespace

#endif // GRENDER_ENABLE_STEP_PROFILING
