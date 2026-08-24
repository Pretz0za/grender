/**
 * Vertex-list overlay: a scrollable "Vertex N" list with a search bar on top,
 * built the same way as the stats and vertex-info overlays -- primitives
 * appended to r->statsPrims and drawn by the same instanced pass.
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
 * external process, no third-party dependency), consistent with the rest of
 * grender: it greedily matches pattern characters against text in order,
 * scoring consecutive runs and word-boundary starts higher than scattered
 * single-character hits.
 */

#include "grInternal.h"

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
// Search input (mirrors grConsole's input handling, scoped to one line)
// ------------------------------------------------------------------------------

/** Appends @p cp to the search input if it's printable ASCII (the bitmap
 *  font, see grOverlayCharHasGlyph, can't draw anything else) and there's
 *  room, marking the filter dirty so the next build re-runs it. */
static void listSearchAppendChar(grRenderer *r, uint32_t cp) {
  if (cp < 32 || cp > 126)
    return;
  if (r->listSearchInputLen + 1 < sizeof(r->listSearchInput)) {
    r->listSearchInput[r->listSearchInputLen++] = (char)cp;
    r->listSearchInput[r->listSearchInputLen] = '\0';
    r->listFilterDirty = true;
    r->listOverlayDirty = true;
  }
}

/**
 * Consumes this frame's queued input (r->pendingConsoleEvents, in delivery
 * order -- see grPendingConsoleEvent) as search-box text editing: printable
 * characters append, Backspace deletes, Enter defocuses (keeping the query),
 * Escape clears the query and defocuses. Only meaningful while
 * r->listSearchFocused; grRendererFrame drains this queue into either the
 * console or the search box, never both, since the two are mutually
 * exclusive (the console owns all input while open).
 */
void grListSearchProcessInput(grRenderer *r) {
  const grPendingConsoleEvent *events = r->pendingConsoleEvents.data();
  for (size_t i = 0; i < r->pendingConsoleEvents.size(); i++) {
    const grPendingConsoleEvent *ev = &events[i];
    if (ev->isChar) {
      listSearchAppendChar(r, (uint32_t)ev->code);
      continue;
    }
    switch (ev->code) {
    case GR_KEY_ENTER:
      r->listSearchFocused = false;
      break;
    case GR_KEY_ESCAPE:
      if (r->listSearchInputLen > 0) {
        r->listSearchInput[0] = '\0';
        r->listSearchInputLen = 0;
        r->listFilterDirty = true;
        r->listOverlayDirty = true;
      }
      r->listSearchFocused = false;
      break;
    case GR_KEY_BACKSPACE:
      if (r->listSearchInputLen > 0) {
        r->listSearchInput[--r->listSearchInputLen] = '\0';
        r->listFilterDirty = true;
        r->listOverlayDirty = true;
      }
      break;
    default:
      break;
    }
  }
  r->pendingConsoleEvents.clear();
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
 *  listFilterDirty; does not touch r->statsPrims. */
static void listOverlayRefreshFilter(grRenderer *r) {
  r->listFilteredIds.clear();
  r->listFilterDirty = false;
  r->listSelectedIdx = SIZE_MAX; // stale index would point at the wrong row
  if (!r->graph)
    return;

  bool hasQuery = r->listSearchInputLen > 0;
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
// Layout + drawing
// ------------------------------------------------------------------------------

void grListOverlayComputeLayout(grRenderer *r, double fbw, double fbh,
                                grListOverlayLayout *out) {
  (void)fbw;
  memset(out, 0, sizeof(*out));
  if (!r->listVisible || !r->graph)
    return;

  double s = r->contentScale > 0.0 ? r->contentScale : 1.0;
  const double margin = 12.0 * s;
  const double pad = 10.0 * s;
  const double fontPx = 1.3 * s;
  const double lineH = GR_FONT_ROWS * fontPx + 4.0 * s;
  const double titleH = lineH + 4.0 * s;
  const double searchH = lineH + 6.0 * s;
  const double panelW = 260.0 * s;
  const double panelH = 320.0 * s;

  double x0 = margin, x1 = margin + panelW;
  double y0 = margin, y1 = y0 + panelH;
  if (y1 > fbh - margin)
    y1 = fbh - margin;
  if (x1 - x0 < panelW * 0.5 || y1 - y0 < titleH + searchH + lineH * 2.0)
    return; // window too small for a useful panel

  out->visible = true;
  out->x0 = x0;
  out->y0 = y0;
  out->x1 = x1;
  out->y1 = y1;
  out->searchY0 = y0 + pad + titleH;
  out->searchY1 = out->searchY0 + searchH;
  out->contentY0 = out->searchY1 + 4.0 * s;
  out->contentY1 = y1 - pad;
  out->lineH = lineH;

  double contentH = out->contentY1 - out->contentY0;
  double wantH = (double)r->listFilteredIds.size() * lineH;
  double maxScrollPx = wantH - contentH;
  out->maxScrollPx = maxScrollPx > 0.0 ? maxScrollPx : 0.0;
}

void grListOverlaySelectDelta(grRenderer *r, double fbw, double fbh,
                              int delta) {
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

  const uint32_t *ids = r->listFilteredIds.data();
  int64_t vertexId = (int64_t)ids[r->listSelectedIdx];
  if (r->pickedVertexId != vertexId) {
    r->pickedVertexId = vertexId;
    r->vertexOverlayScrollPx = 0.0;
    r->vertexOverlayDirty = true;
  }

  // Scroll just enough to bring the newly selected row into view, same
  // clamping grListOverlayBuild does before using listScrollPx to draw.
  grListOverlayLayout L;
  grListOverlayComputeLayout(r, fbw, fbh, &L);
  if (L.visible) {
    double rowY0 =
        L.contentY0 - r->listScrollPx + (double)r->listSelectedIdx * L.lineH;
    double rowY1 = rowY0 + L.lineH;
    if (rowY0 < L.contentY0)
      r->listScrollPx -= (L.contentY0 - rowY0);
    else if (rowY1 > L.contentY1)
      r->listScrollPx += (rowY1 - L.contentY1);
    if (r->listScrollPx < 0.0)
      r->listScrollPx = 0.0;
    if (r->listScrollPx > L.maxScrollPx)
      r->listScrollPx = L.maxScrollPx;
  }

  r->listOverlayDirty = true; // redraw to show the new selection + scroll
}

void grListOverlayBuild(grRenderer *r, double fbw, double fbh) {
  // Deferred while hidden: a highlight/topology/label change queued behind a
  // hidden panel just leaves listFilterDirty set (grRendererShowVertexList
  // doesn't clear it), so the first build after the panel is shown again
  // still catches it -- no need to pay the O(vertex count) cost while
  // nothing is on screen to show it.
  if (r->listVisible && r->listFilterDirty)
    listOverlayRefreshFilter(r);
  r->listOverlayDirty = false;

  grListOverlayLayout L;
  grListOverlayComputeLayout(r, fbw, fbh, &L);
  if (!L.visible)
    return;

  double s = r->contentScale > 0.0 ? r->contentScale : 1.0;
  const uint32_t bgColor = GR_RGBA8(15, 17, 22, 230);
  const uint32_t frameColor = GR_RGBA8(255, 255, 255, 40);
  const uint32_t textColor = GR_RGBA8(235, 235, 240, 255);
  const uint32_t titleColor = GR_RGBA8(101, 197, 255, 255);
  const uint32_t searchBgColor = GR_RGBA8(255, 255, 255, 18);
  const uint32_t searchFocusFrame = GR_RGBA8(101, 197, 255, 160);
  const uint32_t cursorColor = GR_RGBA8(235, 235, 240, 210);
  const uint32_t dimColor = GR_RGBA8(170, 170, 180, 200);
  const uint32_t scrollTrackColor = GR_RGBA8(255, 255, 255, 20);
  const uint32_t scrollThumbColor = GR_RGBA8(255, 255, 255, 90);
  const uint32_t selectedRowColor = GR_RGBA8(101, 197, 255, 45);

  const double pad = 10.0 * s;
  const double fontPx = 1.3 * s;

  grOverlayPushRect(r, L.x0, L.y0, L.x1, L.y1, bgColor);
  grOverlayPushFrame(r, L.x0, L.y0, L.x1, L.y1, 1.0 * s, frameColor);

  // "(", ")", and "*" have no glyph in the tiny bitmap font (see GR_FONT in
  // grStats.c -- only A-Z, 0-9, and ".-+:/_"), so a "(n) *" style indicator
  // would silently drop those characters and leave blank gaps; "FILTERED"
  // spelled out avoids the whole class of punctuation the font can't draw.
  bool filtered = r->highlightActive && r->highlight.has_value();
  char title[64];
  snprintf(title, sizeof(title), "VERTICES: %zu%s",
          r->listFilteredIds.size(), filtered ? " FILTERED" : "");
  grOverlayPushText(r, L.x0 + pad, L.y0 + pad, fontPx, titleColor, title);

  // Search bar.
  grOverlayPushRect(r, L.x0 + pad, L.searchY0, L.x1 - pad, L.searchY1,
                    searchBgColor);
  if (r->listSearchFocused)
    grOverlayPushFrame(r, L.x0 + pad, L.searchY0, L.x1 - pad, L.searchY1,
                       1.0 * s, searchFocusFrame);

  double searchTextY = L.searchY0 + (L.searchY1 - L.searchY0 -
                                     GR_FONT_ROWS * fontPx) *
                                        0.5;
  double searchTextX = L.x0 + pad + 4.0 * s;
  if (r->listSearchInputLen > 0) {
    grOverlayPushText(r, searchTextX, searchTextY, fontPx, textColor,
                      r->listSearchInput);
  } else if (!r->listSearchFocused) {
    grOverlayPushText(r, searchTextX, searchTextY, fontPx, dimColor,
                      "SEARCH DATA...");
  }
  if (r->listSearchFocused) {
    double cursorX =
        searchTextX + grOverlayTextWidth(r->listSearchInput, fontPx);
    grOverlayPushRect(r, cursorX, searchTextY, cursorX + 1.5 * s,
                      searchTextY + GR_FONT_ROWS * fontPx, cursorColor);
  }

  // The layout above may have shrunk (e.g. window resize) since scroll was
  // last set from input, so clamp before using it to pick visible rows.
  if (r->listScrollPx > L.maxScrollPx)
    r->listScrollPx = L.maxScrollPx;
  if (r->listScrollPx < 0.0)
    r->listScrollPx = 0.0;

  // Jump straight to the first row the current scroll offset would show
  // instead of scanning from row 0 -- with the list showing every vertex in
  // a million-vertex graph, a linear scan here would redo a million no-op
  // clip checks on every rebuild (e.g. every scroll tick) just to skip past
  // rows above the fold.
  const uint32_t *ids = r->listFilteredIds.data();
  size_t count = r->listFilteredIds.size();
  size_t startIdx = (size_t)(r->listScrollPx / L.lineH);
  if (startIdx > count)
    startIdx = count;
  double ty = L.contentY0 - r->listScrollPx + (double)startIdx * L.lineH;
  char row[32];
  for (size_t i = startIdx; i < count && ty < L.contentY1;
       i++, ty += L.lineH) {
    if (i == r->listSelectedIdx) {
      double rowTop = ty > L.contentY0 ? ty : L.contentY0;
      double rowBottom = ty + L.lineH < L.contentY1 ? ty + L.lineH : L.contentY1;
      if (rowBottom > rowTop)
        grOverlayPushRect(r, L.x0 + 2.0 * s, rowTop, L.x1 - 6.0 * s, rowBottom,
                          selectedRowColor);
    }
    snprintf(row, sizeof(row), "Vertex %u", ids[i]);
    grOverlayPushTextClipped(r, L.x0 + pad, ty, fontPx, textColor, row,
                             L.contentY0, L.contentY1);
  }

  if (L.maxScrollPx > 0.0) {
    double trackX0 = L.x1 - 4.0 * s, trackX1 = L.x1 - 2.0 * s;
    double trackH = L.contentY1 - L.contentY0;
    double contentH = trackH + L.maxScrollPx;
    grOverlayPushRect(r, trackX0, L.contentY0, trackX1, L.contentY1,
                      scrollTrackColor);
    double thumbH = trackH * (trackH / contentH);
    if (thumbH < 12.0 * s)
      thumbH = 12.0 * s;
    double thumbY0 = L.contentY0 +
                     (trackH - thumbH) * (r->listScrollPx / L.maxScrollPx);
    grOverlayPushRect(r, trackX0, thumbY0, trackX1, thumbY0 + thumbH,
                      scrollThumbColor);
  }
}
