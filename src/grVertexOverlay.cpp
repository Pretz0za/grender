/**
 * Vertex-info overlay: shows the label of the vertex last picked via
 * GR_ACTION_PICK_VERTEX (grenderActionPickVertex in grRenderer.cpp), or
 * selected from the vertex-list overlay, in a scrollable ImGui window.
 * Labels come entirely from grRendererSetVertexLabels (gviz's graph loader
 * hands back pretty-printed JSON per vertex, one field per real newline);
 * this never touches gviz's graph loader or a gviz::Graph directly, keeping
 * the same "supplied by the caller, indexed by parent-graph vertex id"
 * contract as node colors/sizes/degrees.
 *
 * Display used to need manual word-wrap and a whitespace-collapsing/
 * unrenderable-character-stripping pass because the old renderer's bitmap
 * font (a) had no glyphs for most punctuation and (b) couldn't wrap text
 * itself. ImGui::TextWrapped does both correctly out of the box -- it wraps
 * on word boundaries AND respects embedded '\n' the same way plain text
 * would -- so all of that is gone; the label's content and structure stay
 * entirely gviz's concern, and this file's job shrinks to "call
 * TextWrapped".
 */

#include "grInternal.h"

void grRendererShowVertexInfo(grRenderer *r, bool show) {
  if (!r)
    return;
  r->vertexInfoVisible = show;
}

bool grRendererVertexInfoShown(const grRenderer *r) {
  return r && r->vertexInfoVisible;
}

void grVertexOverlayBuild(grRenderer *r) {
  if (!r->vertexInfoVisible || !r->graph || r->pickedVertexId < 0 ||
      !r->vertexLabels || (size_t)r->pickedVertexId >= r->vertexLabelsCount)
    return;
  const char *text = r->vertexLabels[r->pickedVertexId];
  if (!text || !*text)
    return;

  ImGui::SetNextWindowPos(ImVec2(20, 520), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(320, 220), ImGuiCond_FirstUseEver);
  char title[64];
  snprintf(title, sizeof(title), "Vertex %lld###VertexInfo",
          (long long)r->pickedVertexId);
  if (!ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImGui::End();
    return;
  }

  // Reset scroll to the top when a different vertex was just picked --
  // mirrors the old renderer's vertexOverlayScrollPx reset, since otherwise
  // whatever scroll offset was left over from the previous vertex's (likely
  // differently-sized) label would carry over and could scroll straight
  // past the new one's content.
  if (r->pickedVertexId != r->vertexOverlayShownId) {
    ImGui::SetScrollY(0.0f);
    r->vertexOverlayShownId = r->pickedVertexId;
  }

  ImGui::TextWrapped("%s", text);
  ImGui::End();
}
