/**
 * Stats overlay: renders the gvizStatSeries recorded on the attached embedded
 * graph as mini line charts stacked in the top-right corner of the window.
 *
 * This file only decides *where pixels go* (layout, autoscaling, text); the
 * data and chart kinds come from the embedder through the public
 * gvizEmbeddedGraphStatSeries* API, and drawing is a single instanced pass
 * over the grStatsPrim list built here. grRenderer only calls this when a
 * series revision, series count, or viewport layout changes.
 */

#include "grInternal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------------------
// Tiny built-in 5x7 font (uppercase + digits + numeric punctuation). Each
// glyph is 7 rows of 5 cells; '#' marks a lit pixel. Lowercase input is
// rendered with the uppercase glyphs.
// ------------------------------------------------------------------------------

typedef struct grGlyph {
  char c;
  const char *rows; /* 35 chars, row-major */
} grGlyph;

static const grGlyph GR_FONT[] = {
    {'A', ".###."
          "#...#"
          "#...#"
          "#####"
          "#...#"
          "#...#"
          "#...#"},
    {'B', "####."
          "#...#"
          "#...#"
          "####."
          "#...#"
          "#...#"
          "####."},
    {'C', ".###."
          "#...#"
          "#...."
          "#...."
          "#...."
          "#...#"
          ".###."},
    {'D', "####."
          "#...#"
          "#...#"
          "#...#"
          "#...#"
          "#...#"
          "####."},
    {'E', "#####"
          "#...."
          "#...."
          "####."
          "#...."
          "#...."
          "#####"},
    {'F', "#####"
          "#...."
          "#...."
          "####."
          "#...."
          "#...."
          "#...."},
    {'G', ".###."
          "#...#"
          "#...."
          "#.###"
          "#...#"
          "#...#"
          ".###."},
    {'H', "#...#"
          "#...#"
          "#...#"
          "#####"
          "#...#"
          "#...#"
          "#...#"},
    {'I', ".###."
          "..#.."
          "..#.."
          "..#.."
          "..#.."
          "..#.."
          ".###."},
    {'J', "..###"
          "...#."
          "...#."
          "...#."
          "...#."
          "#..#."
          ".##.."},
    {'K', "#...#"
          "#..#."
          "#.#.."
          "##..."
          "#.#.."
          "#..#."
          "#...#"},
    {'L', "#...."
          "#...."
          "#...."
          "#...."
          "#...."
          "#...."
          "#####"},
    {'M', "#...#"
          "##.##"
          "#.#.#"
          "#.#.#"
          "#...#"
          "#...#"
          "#...#"},
    {'N', "#...#"
          "##..#"
          "#.#.#"
          "#..##"
          "#...#"
          "#...#"
          "#...#"},
    {'O', ".###."
          "#...#"
          "#...#"
          "#...#"
          "#...#"
          "#...#"
          ".###."},
    {'P', "####."
          "#...#"
          "#...#"
          "####."
          "#...."
          "#...."
          "#...."},
    {'Q', ".###."
          "#...#"
          "#...#"
          "#...#"
          "#.#.#"
          "#..#."
          ".##.#"},
    {'R', "####."
          "#...#"
          "#...#"
          "####."
          "#.#.."
          "#..#."
          "#...#"},
    {'S', ".####"
          "#...."
          "#...."
          ".###."
          "....#"
          "....#"
          "####."},
    {'T', "#####"
          "..#.."
          "..#.."
          "..#.."
          "..#.."
          "..#.."
          "..#.."},
    {'U', "#...#"
          "#...#"
          "#...#"
          "#...#"
          "#...#"
          "#...#"
          ".###."},
    {'V', "#...#"
          "#...#"
          "#...#"
          "#...#"
          "#...#"
          ".#.#."
          "..#.."},
    {'W', "#...#"
          "#...#"
          "#...#"
          "#.#.#"
          "#.#.#"
          "##.##"
          "#...#"},
    {'X', "#...#"
          "#...#"
          ".#.#."
          "..#.."
          ".#.#."
          "#...#"
          "#...#"},
    {'Y', "#...#"
          "#...#"
          ".#.#."
          "..#.."
          "..#.."
          "..#.."
          "..#.."},
    {'Z', "#####"
          "....#"
          "...#."
          "..#.."
          ".#..."
          "#...."
          "#####"},
    {'0', ".###."
          "#...#"
          "#..##"
          "#.#.#"
          "##..#"
          "#...#"
          ".###."},
    {'1', "..#.."
          ".##.."
          "..#.."
          "..#.."
          "..#.."
          "..#.."
          ".###."},
    {'2', ".###."
          "#...#"
          "....#"
          "...#."
          "..#.."
          ".#..."
          "#####"},
    {'3', ".###."
          "#...#"
          "....#"
          "..##."
          "....#"
          "#...#"
          ".###."},
    {'4', "...#."
          "..##."
          ".#.#."
          "#..#."
          "#####"
          "...#."
          "...#."},
    {'5', "#####"
          "#...."
          "####."
          "....#"
          "....#"
          "#...#"
          ".###."},
    {'6', ".###."
          "#...."
          "#...."
          "####."
          "#...#"
          "#...#"
          ".###."},
    {'7', "#####"
          "....#"
          "...#."
          "..#.."
          "..#.."
          "..#.."
          "..#.."},
    {'8', ".###."
          "#...#"
          "#...#"
          ".###."
          "#...#"
          "#...#"
          ".###."},
    {'9', ".###."
          "#...#"
          "#...#"
          ".####"
          "....#"
          "....#"
          ".###."},
    {'.', "....."
          "....."
          "....."
          "....."
          "....."
          "..##."
          "..##."},
    {'-', "....."
          "....."
          "....."
          ".###."
          "....."
          "....."
          "....."},
    {'+', "....."
          "....."
          "..#.."
          ".###."
          "..#.."
          "....."
          "....."},
    {':', "....."
          "..#.."
          "....."
          "....."
          "....."
          "..#.."
          "....."},
    {'/', "....#"
          "....#"
          "...#."
          "..#.."
          ".#..."
          "#...."
          "#...."},
    {'_', "....."
          "....."
          "....."
          "....."
          "....."
          "....."
          "#####"},
};

static const char *glyphRows(char c) {
  if (c >= 'a' && c <= 'z')
    c = (char)(c - 'a' + 'A');
  for (size_t i = 0; i < sizeof(GR_FONT) / sizeof(GR_FONT[0]); i++)
    if (GR_FONT[i].c == c)
      return GR_FONT[i].rows;
  return NULL;
}

// ------------------------------------------------------------------------------
// Primitive list building
// ------------------------------------------------------------------------------

static int pushPrim(grRenderer *r, grStatsPrim prim) {
  return gvizArrayPush(&r->statsPrims, &prim);
}

static void pushRect(grRenderer *r, double x0, double y0, double x1, double y1,
                     uint32_t color) {
  pushPrim(r, (grStatsPrim){
                  .ab = {(float)x0, (float)y0, (float)x1, (float)y1},
                  .color = color,
                  .kind = 0,
              });
}

static void pushLine(grRenderer *r, double x0, double y0, double x1, double y1,
                     double halfWidth, uint32_t color) {
  pushPrim(r, (grStatsPrim){
                  .ab = {(float)x0, (float)y0, (float)x1, (float)y1},
                  .color = color,
                  .kind = 1,
                  .halfWidth = (float)halfWidth,
              });
}

static void pushFrame(grRenderer *r, double x0, double y0, double x1, double y1,
                      double thickness, uint32_t color) {
  pushRect(r, x0, y0, x1, y0 + thickness, color);
  pushRect(r, x0, y1 - thickness, x1, y1, color);
  pushRect(r, x0, y0, x0 + thickness, y1, color);
  pushRect(r, x1 - thickness, y0, x1, y1, color);
}

/** Advance of one character cell, in font pixels. */
#define GR_FONT_ADVANCE 6.0
#define GR_FONT_ROWS 7

static double textWidth(const char *text, double px) {
  return (double)strlen(text) * GR_FONT_ADVANCE * px;
}

/** Draws @p text with its top-left corner at (x, y); @p px is the size of one
 *  font pixel. Horizontal runs of lit pixels are merged into single rects. */
static void pushText(grRenderer *r, double x, double y, double px,
                     uint32_t color, const char *text) {
  for (; *text; text++, x += GR_FONT_ADVANCE * px) {
    const char *rows = glyphRows(*text);
    if (!rows)
      continue;
    for (int row = 0; row < GR_FONT_ROWS; row++) {
      int col = 0;
      while (col < 5) {
        if (rows[row * 5 + col] != '#') {
          col++;
          continue;
        }
        int runEnd = col;
        while (runEnd < 5 && rows[row * 5 + runEnd] == '#')
          runEnd++;
        pushRect(r, x + col * px, y + row * px, x + runEnd * px,
                 y + (row + 1) * px, color);
        col = runEnd;
      }
    }
  }
}

// ------------------------------------------------------------------------------
// Chart layout
// ------------------------------------------------------------------------------

static const uint32_t GR_STATS_PALETTE[] = {
    GR_RGBA8(101, 197, 255, 255), GR_RGBA8(255, 176, 92, 255),
    GR_RGBA8(126, 217, 130, 255), GR_RGBA8(255, 118, 118, 255),
    GR_RGBA8(197, 152, 245, 255), GR_RGBA8(240, 221, 106, 255),
};
#define GR_STATS_PALETTE_COUNT                                                 \
  (sizeof(GR_STATS_PALETTE) / sizeof(GR_STATS_PALETTE[0]))

static double mapSampleY(double v, double lo, double hi, bool logScale,
                         double yTop, double yBottom) {
  double t;
  if (logScale)
    t = (log10(v) - log10(lo)) / (log10(hi) - log10(lo));
  else
    t = (v - lo) / (hi - lo);
  if (!isfinite(t))
    t = 0.5;
  if (t < 0.0)
    t = 0.0;
  if (t > 1.0)
    t = 1.0;
  return yBottom - t * (yBottom - yTop);
}

/** Computes the plotted range of @p series. Log charts only consider positive
 *  samples; returns false when nothing is plottable. */
static bool seriesRange(const gvizStatSeries *series, bool logScale, double *lo,
                        double *hi) {
  bool any = false;
  double mn = 0.0, mx = 0.0;
  for (size_t i = 0; i < series->count; i++) {
    double v = series->samples[i];
    if (!isfinite(v) || (logScale && v <= 0.0))
      continue;
    if (!any || v < mn)
      mn = v;
    if (!any || v > mx)
      mx = v;
    any = true;
  }
  if (!any)
    return false;
  if (mn == mx) { // flat series: pad the range so the line sits mid-plot
    if (logScale) {
      mn *= 0.5;
      mx *= 2.0;
    } else {
      mn -= 0.5;
      mx += 0.5;
    }
  }
  *lo = mn;
  *hi = mx;
  return true;
}

static void buildChart(grRenderer *r, const gvizStatSeries *series,
                       size_t paletteIdx, double x0, double y0, double x1,
                       double y1, double s) {
  const uint32_t bgColor = GR_RGBA8(15, 17, 22, 215);
  const uint32_t frameColor = GR_RGBA8(255, 255, 255, 40);
  const uint32_t textColor = GR_RGBA8(235, 235, 240, 255);
  const uint32_t dimColor = GR_RGBA8(170, 170, 180, 200);
  const uint32_t lineColor =
      GR_STATS_PALETTE[paletteIdx % GR_STATS_PALETTE_COUNT];

  const double pad = 8.0 * s;
  const double fontPx = 1.4 * s;
  const double fontSmallPx = 1.1 * s;
  const double titleH = GR_FONT_ROWS * fontPx + 5.0 * s;
  const bool logScale = series->kind == GVIZ_STAT_CHART_LINE_LOG;

  pushRect(r, x0, y0, x1, y1, bgColor);

  char buf[64];
  pushText(r, x0 + pad, y0 + pad, fontPx, textColor, series->name);
  if (series->count > 0) {
    snprintf(buf, sizeof(buf), "%.4g", series->samples[series->count - 1]);
    pushText(r, x1 - pad - textWidth(buf, fontPx), y0 + pad, fontPx, lineColor,
             buf);
  }

  double px0 = x0 + pad, px1 = x1 - pad;
  double py0 = y0 + pad + titleH, py1 = y1 - pad;
  pushFrame(r, px0, py0, px1, py1, 1.0 * s, frameColor);

  if (series->count == 0)
    return;

  double lo, hi;
  if (!seriesRange(series, logScale, &lo, &hi))
    return;

  double yTop = py0 + 3.0 * s, yBottom = py1 - 3.0 * s;
  double xLeft = px0 + 2.0 * s, xRight = px1 - 2.0 * s;

  // One segment per horizontal pixel is enough; stride over dense series.
  size_t count = series->count;
  size_t maxPoints = (size_t)(xRight - xLeft) + 2;
  size_t stride = count > maxPoints ? (count + maxPoints - 1) / maxPoints : 1;

  double prevX = 0.0, prevY = 0.0;
  bool havePrev = false;
  for (size_t i = 0; i < count; i += stride) {
    size_t idx = i + stride < count ? i : count - 1; // always include the tail
    double v = series->samples[idx];
    if (!isfinite(v) || (logScale && v <= 0.0)) {
      havePrev = false;
      continue;
    }
    double x = count > 1
                   ? xLeft + (xRight - xLeft) * (double)idx / (double)(count - 1)
                   : (xLeft + xRight) * 0.5;
    double y = mapSampleY(v, lo, hi, logScale, yTop, yBottom);
    if (havePrev)
      pushLine(r, prevX, prevY, x, y, 0.75 * s, lineColor);
    else
      pushLine(r, x, y, x, y, 1.0 * s, lineColor); // isolated sample: dot
    prevX = x;
    prevY = y;
    havePrev = true;
  }

  snprintf(buf, sizeof(buf), "%.3g", hi);
  pushText(r, px0 + 3.0 * s, py0 + 3.0 * s, fontSmallPx, dimColor, buf);
  snprintf(buf, sizeof(buf), "%.3g", lo);
  pushText(r, px0 + 3.0 * s, py1 - 3.0 * s - GR_FONT_ROWS * fontSmallPx,
           fontSmallPx, dimColor, buf);
}

void grStatsOverlayBuild(grRenderer *r, double fbw, double fbh) {
  r->statsPrims.count = 0;
  if (!r->graph)
    return;

  size_t total = gvizEmbeddedGraphStatSeriesCount(r->graph);
  double s = r->contentScale > 0.0 ? r->contentScale : 1.0;

  const double margin = 12.0 * s;
  const double chartW = 240.0 * s;
  const double chartH = 96.0 * s;
  const double gap = 8.0 * s;

  double x1 = fbw - margin;
  double x0 = x1 - chartW;
  double y = margin;

  size_t chartIdx = 0;
  for (size_t i = 0; i < total; i++) {
    const gvizStatSeries *series = gvizEmbeddedGraphStatSeriesAt(r->graph, i);
    if (!series)
      continue;
    if (i < r->statsSeriesVisibleCount && !r->statsSeriesVisible[i])
      continue;
    if (y + chartH > fbh - margin) // no space for another chart
      break;
    buildChart(r, series, chartIdx, x0, y, x1, y + chartH, s);
    y += chartH + gap;
    chartIdx++;
  }
}
