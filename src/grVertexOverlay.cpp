/**
 * Vertex-info overlay: shows the label of the vertex last picked via
 * GR_ACTION_PICK_VERTEX (grenderActionPickVertex in grRenderer.c), in a
 * scrollable panel built the same way as the stats overlay -- primitives
 * appended to r->statsPrims and drawn by the same instanced pass. Labels
 * come entirely from grRendererSetVertexLabels (gviz's graph loader hands
 * back pretty-printed JSON per vertex, one field per real newline); this
 * never touches gviz's graph loader or a gvizGraph directly, keeping the
 * same "supplied by the caller, indexed by parent-graph vertex id" contract
 * as node colors/sizes/degrees.
 *
 * Display cleanup here is generic text layout, not JSON parsing: a label's
 * real newlines start new lines instead of being flattened, runs of
 * whitespace collapse to one space, and word-wrapping only counts
 * characters the bitmap font can actually draw (see grOverlayCharHasGlyph)
 * so punctuation the font has no glyph for -- quotes, braces, commas --
 * doesn't reserve blank columns. The label's content and structure stay
 * entirely gviz's concern.
 */

#include "grInternal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void grRendererShowVertexInfo(grRenderer *r, bool show) {
  if (!r || r->vertexInfoVisible == show)
    return;
  r->vertexInfoVisible = show;
  r->vertexOverlayDirty = true;
}

bool grRendererVertexInfoShown(const grRenderer *r) {
  return r && r->vertexInfoVisible;
}

static bool lineHasContent(const char *s) {
  for (; *s; s++)
    if (isalnum((unsigned char)*s))
      return true;
  return false;
}

/** Drops characters the bitmap font has no glyph for (quotes, braces,
 *  commas, backslashes, ...), keeping spaces/tabs regardless. Without this,
 *  those characters still advance the cursor when drawn (grOverlayPushText
 *  advances unconditionally) even though nothing is drawn for them, leaving
 *  a visible blank gap; dropping them here means what's counted during
 *  word-wrap matches what's actually drawn, with no leftover gap. */
static void stripUnrenderable(const char *in, char *out, size_t outSize) {
  size_t o = 0;
  for (; *in && o + 1 < outSize; in++)
    if (*in == ' ' || *in == '\t' || grOverlayCharHasGlyph(*in))
      out[o++] = *in;
  out[o] = '\0';
}

/** Trims leading/trailing whitespace and collapses internal runs of spaces
 *  and tabs to a single space. */
static void collapseWhitespace(const char *in, char *out, size_t outSize) {
  while (*in == ' ' || *in == '\t')
    in++;
  size_t o = 0;
  bool prevSpace = false;
  for (; *in && o + 1 < outSize; in++) {
    bool isSpace = *in == ' ' || *in == '\t';
    if (isSpace) {
      if (!prevSpace)
        out[o++] = ' ';
      prevSpace = true;
    } else {
      out[o++] = *in;
      prevSpace = false;
    }
  }
  while (o > 0 && out[o - 1] == ' ')
    o--;
  out[o] = '\0';
}

static double glyphWidth(char c, double px) {
  return grOverlayCharHasGlyph(c) ? GR_FONT_ADVANCE * px : 0.0;
}

/** Appends one buffered display line to @p lines, resetting @p buf/@p bufLen/
 *  @p curW for the next line. */
static void flushLine(std::vector<grVertexOverlayLine> *lines,
                      grVertexOverlayLine *buf, size_t *bufLen, double *curW) {
  lines->push_back(*buf);
  (*buf)[0] = '\0';
  *bufLen = 0;
  *curW = 0.0;
}

/** Greedy word-wraps @p line (already whitespace-collapsed) to fit @p innerW
 *  pixels at @p fontPx, appending one grVertexOverlayLine per wrapped row to
 *  @p lines. Only breaks mid-word when a single word alone exceeds innerW. */
static void wordWrapLine(const char *line, double innerW, double fontPx,
                         std::vector<grVertexOverlayLine> *lines) {
  grVertexOverlayLine buf;
  size_t bufLen = 0;
  double curW = 0.0;
  buf[0] = '\0';

  const char *p = line;
  while (*p) {
    const char *wordStart = p;
    while (*p && *p != ' ')
      p++;
    size_t wordLen = (size_t)(p - wordStart);
    if (*p == ' ')
      p++;

    double wordW = 0.0;
    for (size_t i = 0; i < wordLen; i++)
      wordW += glyphWidth(wordStart[i], fontPx);
    double spaceW = GR_FONT_ADVANCE * fontPx;

    if (bufLen > 0 && curW + spaceW + wordW > innerW)
      flushLine(lines, &buf, &bufLen, &curW);
    if (bufLen > 0 && bufLen + 1 < sizeof(buf)) {
      buf[bufLen++] = ' ';
      buf[bufLen] = '\0';
      curW += spaceW;
    }

    size_t i = 0;
    while (i < wordLen) {
      if (bufLen > 0 && curW + glyphWidth(wordStart[i], fontPx) > innerW)
        flushLine(lines, &buf, &bufLen, &curW);
      while (i < wordLen && bufLen + 1 < sizeof(buf)) {
        double cw = glyphWidth(wordStart[i], fontPx);
        if (bufLen > 0 && curW + cw > innerW)
          break;
        buf[bufLen++] = wordStart[i];
        buf[bufLen] = '\0';
        curW += cw;
        i++;
      }
    }
  }
  if (bufLen > 0)
    lines->push_back(buf);
}

void grVertexOverlayComputeLayout(grRenderer *r, double fbw, double fbh,
                                  grVertexOverlayLayout *out) {
  (void)fbw;
  memset(out, 0, sizeof(*out));
  r->vertexOverlayLines.clear();

  if (!r->vertexInfoVisible || !r->graph || r->pickedVertexId < 0 ||
      !r->vertexLabels ||
      (size_t)r->pickedVertexId >= r->vertexLabelsCount)
    return;
  const char *text = r->vertexLabels[r->pickedVertexId];
  if (!text || !*text)
    return;

  double s = r->contentScale > 0.0 ? r->contentScale : 1.0;
  const double margin = 12.0 * s;
  const double pad = 10.0 * s;
  const double fontPx = 1.3 * s;
  const double lineH = GR_FONT_ROWS * fontPx + 4.0 * s;
  const double panelW = 320.0 * s;
  const double innerW = panelW - 2.0 * pad;

  // Split on real newlines -- gviz's graph loader pretty-prints vertex-data
  // JSON with one field per line -- then whitespace-collapse and word-wrap
  // each resulting line independently, so a field boundary actually starts
  // a new display line instead of being flattened into one blob that then
  // gets chopped at an arbitrary column.
  char raw[192];
  char stripped[192];
  char cleaned[192];
  const char *lineStart = text;
  for (const char *p = text;; p++) {
    if (*p == '\n' || *p == '\0') {
      size_t n = (size_t)(p - lineStart);
      if (n >= sizeof(raw))
        n = sizeof(raw) - 1;
      memcpy(raw, lineStart, n);
      raw[n] = '\0';
      stripUnrenderable(raw, stripped, sizeof(stripped));
      collapseWhitespace(stripped, cleaned, sizeof(cleaned));
      if (lineHasContent(cleaned))
        wordWrapLine(cleaned, innerW, fontPx, &r->vertexOverlayLines);
      lineStart = p + 1;
      if (*p == '\0')
        break;
    }
  }

  double titleH = lineH + 4.0 * s;
  double contentH = (double)r->vertexOverlayLines.size() * lineH;
  double availH = fbh - 2.0 * margin;
  double wantH = pad * 2.0 + titleH + contentH;
  double panelH = wantH < availH ? wantH : availH;

  double x0 = margin, x1 = margin + panelW;
  double y1 = fbh - margin, y0 = y1 - panelH;

  double visibleContentH = panelH - pad * 2.0 - titleH;
  double maxScrollPx = contentH - visibleContentH;
  if (maxScrollPx < 0.0)
    maxScrollPx = 0.0;

  out->visible = true;
  out->x0 = x0;
  out->y0 = y0;
  out->x1 = x1;
  out->y1 = y1;
  out->contentY0 = y0 + pad + titleH;
  out->contentY1 = y1 - pad;
  out->lineH = lineH;
  out->maxScrollPx = maxScrollPx;
}

void grVertexOverlayBuild(grRenderer *r, double fbw, double fbh) {
  grVertexOverlayLayout L;
  grVertexOverlayComputeLayout(r, fbw, fbh, &L);
  if (!L.visible)
    return;

  double s = r->contentScale > 0.0 ? r->contentScale : 1.0;
  const uint32_t bgColor = GR_RGBA8(15, 17, 22, 230);
  const uint32_t frameColor = GR_RGBA8(255, 255, 255, 40);
  const uint32_t textColor = GR_RGBA8(235, 235, 240, 255);
  const uint32_t titleColor = GR_RGBA8(101, 197, 255, 255);
  const uint32_t scrollTrackColor = GR_RGBA8(255, 255, 255, 20);
  const uint32_t scrollThumbColor = GR_RGBA8(255, 255, 255, 90);

  const double pad = 10.0 * s;
  const double fontPx = 1.3 * s;

  char title[64];
  snprintf(title, sizeof(title), "VERTEX %lld", (long long)r->pickedVertexId);

  grOverlayPushRect(r, L.x0, L.y0, L.x1, L.y1, bgColor);
  grOverlayPushFrame(r, L.x0, L.y0, L.x1, L.y1, 1.0 * s, frameColor);
  grOverlayPushText(r, L.x0 + pad, L.y0 + pad, fontPx, titleColor, title);

  // The layout above may have shrunk (e.g. window resize) since scroll was
  // last set from input, so clamp before using it to pick visible lines.
  if (r->vertexOverlayScrollPx > L.maxScrollPx)
    r->vertexOverlayScrollPx = L.maxScrollPx;
  if (r->vertexOverlayScrollPx < 0.0)
    r->vertexOverlayScrollPx = 0.0;

  const grVertexOverlayLine *lines = r->vertexOverlayLines.data();
  double ty = L.contentY0 - r->vertexOverlayScrollPx;
  for (size_t i = 0; i < r->vertexOverlayLines.size(); i++, ty += L.lineH)
    grOverlayPushTextClipped(r, L.x0 + pad, ty, fontPx, textColor, lines[i],
                             L.contentY0, L.contentY1);

  if (L.maxScrollPx > 0.0) {
    double trackX0 = L.x1 - 4.0 * s, trackX1 = L.x1 - 2.0 * s;
    double trackH = L.contentY1 - L.contentY0;
    double contentH = trackH + L.maxScrollPx;
    grOverlayPushRect(r, trackX0, L.contentY0, trackX1, L.contentY1,
                      scrollTrackColor);
    double thumbH = trackH * (trackH / contentH);
    if (thumbH < 12.0 * s)
      thumbH = 12.0 * s;
    double thumbY0 = L.contentY0 + (trackH - thumbH) *
                                       (r->vertexOverlayScrollPx /
                                        L.maxScrollPx);
    grOverlayPushRect(r, trackX0, thumbY0, trackX1, thumbY0 + thumbH,
                      scrollThumbColor);
  }
}
