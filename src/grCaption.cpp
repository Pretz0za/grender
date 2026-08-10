/**
 * Caption overlay: a single small text banner centered near the bottom of
 * the window, for an application to narrate the current step of whatever
 * it's showing (e.g. a teaching visualization's "comparing right contour of
 * A against left contour of B" text) without building its own screen-space
 * text drawing. Deliberately simple compared to the stats/vertex-list/
 * vertex-info panels: one string, no scrolling, no search, no per-series
 * bookkeeping -- see grStats.cpp for the shared bitmap-font primitives this
 * reuses (grOverlayPushRect/grOverlayPushText).
 */

#include "grInternal.h"

#include <cstring>

void grRendererSetCaption(grRenderer *r, const char *text) {
  if (!r)
    return;
  std::string next = text ? text : "";
  if (next == r->captionText)
    return;
  r->captionText = std::move(next);
  r->captionDirty = true;
}

void grRendererShowCaption(grRenderer *r, bool show) {
  if (!r || r->captionVisible == show)
    return;
  r->captionVisible = show;
  r->captionDirty = true;
}

bool grRendererCaptionShown(const grRenderer *r) {
  return r && r->captionVisible;
}

void grCaptionBuild(grRenderer *r, double fbw, double fbh) {
  if (!r->captionVisible || r->captionText.empty())
    return;

  double s = r->contentScale > 0.0 ? r->contentScale : 1.0;
  double fontPx = 2.2 * s;
  const double padX = 16.0 * s, padY = 10.0 * s;
  const double marginBottom = 28.0 * s;
  const uint32_t bgColor = GR_RGBA8(12, 13, 17, 225);
  const uint32_t frameColor = GR_RGBA8(255, 255, 255, 35);
  const uint32_t textColor = GR_RGBA8(240, 240, 245, 255);

  double textW = grOverlayTextWidth(r->captionText.c_str(), fontPx);
  double maxW = fbw - 2.0 * 24.0 * s;
  if (textW > maxW) {
    // Caption too wide for one line at this font size: shrink uniformly
    // rather than wrapping -- captions are meant to be one short sentence,
    // and a wrapped multi-line banner would fight the "small" brief this
    // overlay exists for.
    double scale = maxW / textW;
    if (scale < 0.4)
      scale = 0.4; // floor so it never becomes unreadably small
    fontPx *= scale;
    textW = grOverlayTextWidth(r->captionText.c_str(), fontPx);
  }

  double boxW = textW + 2.0 * padX;
  double boxH = GR_FONT_ROWS * fontPx + 2.0 * padY;
  double x0 = (fbw - boxW) * 0.5;
  double y1 = fbh - marginBottom;
  double y0 = y1 - boxH;
  double x1 = x0 + boxW;

  grOverlayPushRect(r, x0, y0, x1, y1, bgColor);
  grOverlayPushFrame(r, x0, y0, x1, y1, 1.0 * s, frameColor);
  grOverlayPushText(r, x0 + padX, y0 + padY, fontPx, textColor,
                    r->captionText.c_str());
}
