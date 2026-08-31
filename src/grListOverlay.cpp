/**
 * Vertex-list overlay: an ImGui window ("Vertices") holding a search bar
 * (ImGui::InputText) and a scrollable list of rows below it
 * (ImGui::Selectable inside a child region).
 *
 * The list shows every currently-visible vertex, or, while a highlight is
 * active (grRendererSetHighlight / GR_ACTION_PICK_VERTEX / GR_ACTION_PICK_FACE
 * / grConsole's "find"), only the highlighted vertices -- read straight off
 * r->highlight (grRenderer's own highlight state; gviz::layout::EmbeddedGraph
 * no longer carries one -- see grInternal.h), the same source
 * applyColorLayers (grRenderer.cpp) uses to color them. The search bar
 * further narrows that set with a fuzzy match against each vertex's DATA
 * string from grRendererSetVertexLabels; it never reads gviz's graph loader
 * directly.
 *
 * The fuzzy matcher below is a small self-contained subsequence scorer (no
 * external process, no third-party dependency, and no reason to route it
 * through ImGui's own filter widget) -- unchanged by the ImGui migration:
 * it greedily matches pattern characters against text in order, scoring
 * consecutive runs and word-boundary starts higher than scattered
 * single-character hits.
 */

#include "grInternal.h"

#include <cfloat>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

// ------------------------------------------------------------------------------
// Fuzzy matching
// ------------------------------------------------------------------------------

/**
 * Case-insensitive subsequence fuzzy match: true iff every character of
 * @p pattern appears in @p text in order (not necessarily contiguous). When
 * it matches and @p outScore is non-NULL, writes a score where higher is a
 * better match -- consecutive runs and matches starting right at a word
 * boundary (start of text, or just after a non-alphanumeric character) score
 * above scattered single-character hits, so e.g. pattern "id" ranks
 * "id: 3" above "invalid data". This is a greedy left-to-right scan, not a
 * full alignment search, so it can occasionally rank two matches in an order
 * a more exhaustive matcher wouldn't -- acceptable for filtering a live list,
 * not a correctness property callers depend on. An empty pattern matches
 * everything with score 0.
 */
static bool fuzzyMatch(const char *pattern, const char *text, int *outScore) {
  if (!pattern || !*pattern) {
    if (outScore)
      *outScore = 0;
    return true;
  }
  if (!text || !*text)
    return false;

  int score = 0;
  int run = 0;
  bool atBoundary = true; // start of text counts as a boundary
  const char *p = pattern;

  for (const char *t = text; *t && *p; t++) {
    bool isSep = !isalnum((unsigned char)*t);
    if (tolower((unsigned char)*t) == tolower((unsigned char)*p)) {
      run++;
      score += 1 + run;
      if (atBoundary)
        score += 8;
      p++;
    } else {
      run = 0;
    }
    atBoundary = isSep;
  }

  if (*p)
    return false; // ran out of text before matching every pattern character

  if (outScore)
    *outScore = score;
  return true;
}

// ------------------------------------------------------------------------------
// Filter rebuild
// ------------------------------------------------------------------------------

typedef struct grListMatch {
  uint32_t id;
  int score;
} grListMatch;

/** Higher score first; ties break by id for a stable, deterministic order. */
static bool matchBeforeDesc(const grListMatch &a, const grListMatch &b) {
  if (a.score != b.score)
    return a.score > b.score;
  return a.id < b.id;
}

static const char *listVertexLabel(const grRenderer *r, size_t v) {
  return (r->vertexLabels && v < r->vertexLabelsCount) ? r->vertexLabels[v]
                                                       : NULL;
}

/** Rebuilds r->listFilteredIds from the active highlight (or all visible
 *  topology vertices, with none active) and r->listSearchInput. Clears
 *  listFilterDirty. */
static void listOverlayRefreshFilter(grRenderer *r) {
  r->listFilteredIds.clear();
  r->listFilterDirty = false;
  r->listSelectedIdx = SIZE_MAX; // stale index would point at the wrong row
  if (!r->graph)
    return;

  bool hasQuery = r->listSearchInput[0] != '\0';
  const gviz::Subgraph *highlight =
      (r->highlightActive && r->highlight.has_value()) ? &*r->highlight : NULL;

  if (!hasQuery) {
    // No search text: keep natural order, no scoring needed.
    if (highlight && r->structureView) {
      // *highlight iterates raw ids; listFilteredIds (like every other
      // by-index part of this API) is LOCAL-indexed -- see
      // grRendererSetGraph's INDEXING CONVENTION.
      for (size_t u : *highlight)
        r->listFilteredIds.push_back((uint32_t)r->structureView->RawToLocal(u));
    } else {
      for (size_t i = 0; i < r->topo.nodeCount; i++)
        r->listFilteredIds.push_back(r->topo.nodeIds[i]);
    }
    return;
  }

  std::vector<grListMatch> matches;

  if (highlight && r->structureView) {
    for (size_t u : *highlight) {
      uint32_t local = (uint32_t)r->structureView->RawToLocal(u);
      const char *label = listVertexLabel(r, local);
      int score;
      if (label && fuzzyMatch(r->listSearchInput, label, &score))
        matches.push_back({local, score});
    }
  } else {
    for (size_t i = 0; i < r->topo.nodeCount; i++) {
      uint32_t v = r->topo.nodeIds[i];
      const char *label = listVertexLabel(r, v);
      int score;
      if (label && fuzzyMatch(r->listSearchInput, label, &score))
        matches.push_back({v, score});
    }
  }

  std::sort(matches.begin(), matches.end(), matchBeforeDesc);
  for (const grListMatch &m : matches)
    r->listFilteredIds.push_back(m.id);
}

// ------------------------------------------------------------------------------
// Selection
// ------------------------------------------------------------------------------

void grListOverlaySelectDelta(grRenderer *r, int delta) {
  size_t count = r->listFilteredIds.size();
  if (count == 0)
    return;

  if (r->listSelectedIdx >= count) // SIZE_MAX (no selection yet) or stale
    r->listSelectedIdx = delta >= 0 ? 0 : count - 1;
  else if (delta > 0)
    r->listSelectedIdx =
        r->listSelectedIdx + 1 < count ? r->listSelectedIdx + 1 : count - 1;
  else if (delta < 0)
    r->listSelectedIdx = r->listSelectedIdx > 0 ? r->listSelectedIdx - 1 : 0;

  r->pickedVertexId = (int64_t)r->listFilteredIds[r->listSelectedIdx];
  r->listScrollToSelected = true; // consumed by the next grListOverlayBuild
}

// ------------------------------------------------------------------------------
// Drawing
// ------------------------------------------------------------------------------

void grListOverlayBuild(grRenderer *r) {
  r->listSearchInputFocused = false;
  if (!r->listVisible || !r->graph)
    return;

  // Deferred while hidden: a highlight/topology/label change queued behind a
  // hidden panel just leaves listFilterDirty set (grRendererShowVertexList
  // doesn't clear it), so the first build after the panel is shown again
  // still catches it -- no need to pay the O(vertex count) cost while
  // nothing is on screen to show it. Already gated on r->listVisible above.
  if (r->listFilterDirty)
    listOverlayRefreshFilter(r);

  ImGui::SetNextWindowPos(ImVec2(20, 250), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(280, 260), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Vertices", nullptr, ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImGui::End();
    return;
  }

  bool filtered = r->highlightActive && r->highlight.has_value();
  ImGui::Text("%zu vertices%s", r->listFilteredIds.size(),
             filtered ? " (filtered)" : "");

  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::InputTextWithHint("##search", "Search data...",
                               r->listSearchInput,
                               sizeof(r->listSearchInput)))
    r->listFilterDirty = true;
  r->listSearchInputFocused = ImGui::IsItemFocused();

  ImGui::Separator();
  ImGui::BeginChild("##rows", ImVec2(0, 0), ImGuiChildFlags_Borders);

  const uint32_t *ids = r->listFilteredIds.data();
  size_t count = r->listFilteredIds.size();
  char label[32];
  ImGuiListClipper clipper;
  clipper.Begin((int)count);
  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
      snprintf(label, sizeof(label), "Vertex %u", ids[i]);
      bool isSelected = (size_t)i == r->listSelectedIdx;
      if (ImGui::Selectable(label, isSelected)) {
        r->listSelectedIdx = (size_t)i;
        r->pickedVertexId = (int64_t)ids[i];
      }
      if (isSelected && r->listScrollToSelected) {
        ImGui::SetScrollHereY(0.5f);
        r->listScrollToSelected = false;
      }
    }
  }

  ImGui::EndChild();
  ImGui::End();
}
