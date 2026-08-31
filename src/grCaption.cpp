/**
 * Caption overlay: a single small ImGui window, centered near the bottom of
 * the window, for an application to narrate the current step of whatever
 * it's showing (e.g. a teaching visualization's "comparing right contour of
 * A against left contour of B" text) without building its own on-screen text
 * drawing. Deliberately simple compared to the stats/vertex-list/vertex-info
 * panels: one string, no scrolling, no search, no per-series bookkeeping, no
 * title bar or resize/move handles -- it's meant to just sit there and be
 * read, not be interacted with.
 */

#include "grInternal.h"

void grRendererSetCaption(grRenderer *r, const char *text) {
  if (!r)
    return;
  r->captionText = text ? text : "";
}

void grRendererShowCaption(grRenderer *r, bool show) {
  if (!r)
    return;
  r->captionVisible = show;
}

bool grRendererCaptionShown(const grRenderer *r) {
  return r && r->captionVisible;
}

void grCaptionBuild(grRenderer *r, double fbw, double fbh) {
  if (!r->captionVisible || r->captionText.empty())
    return;

  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;

  ImGui::SetNextWindowBgAlpha(0.85f);
  // Centered horizontally, pinned a fixed distance above the bottom edge;
  // pivot (0.5, 1.0) anchors the window's horizontal center / bottom edge
  // to that point so it grows in place as the text changes instead of
  // drifting.
  ImGui::SetNextWindowPos(ImVec2((float)fbw * 0.5f, (float)fbh - 28.0f),
                          ImGuiCond_Always, ImVec2(0.5f, 1.0f));

  ImGui::Begin("##Caption", nullptr, flags);
  ImGui::TextUnformatted(r->captionText.c_str());
  ImGui::End();
}
