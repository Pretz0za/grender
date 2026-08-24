/**
 * Stats overlay: renders the gviz::layout::StatSeries recorded on the
 * attached embedded graph as ImPlot line charts inside a single ImGui
 * window ("Stats"), stacked top to bottom -- one chart per non-empty,
 * currently-shown series.
 *
 * This file only decides what to draw; the data and chart kind (linear vs.
 * log y axis) come from the embedder through the public
 * EmbeddedGraph::StatSeries* API. It's called once per frame while
 * r->statsVisible (see buildOverlayWindows in grRenderer.cpp) -- there's no
 * revision-cache dirty tracking the way the old bitmap-font renderer needed,
 * since rebuilding an ImGui/ImPlot window every frame it's open is exactly
 * what those libraries are designed for (they rebuild their own draw data
 * from scratch every frame regardless).
 *
 * The window is docked to the right edge of the viewport (position/size
 * re-pinned every frame with ImGuiCond_Always, not just on first use) rather
 * than left as a free-floating window a user could drag on top of the graph.
 * Each plot only shows the most recent GR_STATS_WINDOW_SAMPLES samples of its
 * series -- StatSeries samples accumulate for the lifetime of the embedded
 * graph (e.g. thousands of force-tick heat values), and charting the entire
 * history both makes long-running demos increasingly illegible (recent
 * detail squashed flat against old data) and does needless work re-uploading
 * an ever-growing vertex buffer to ImPlot every frame.
 */

#include "grInternal.h"

/** How many of a series' most recent samples to chart; older samples still
 *  live in StatSeries::samples (gviz keeps the full history) but scroll out
 *  of view here. */
static constexpr int GR_STATS_WINDOW_SAMPLES = 200;

/** Right-side dock width, in the same units as ImGui window coordinates. */
static constexpr float GR_STATS_PANEL_WIDTH = 340.0f;

void grStatsOverlayBuild(grRenderer *r) {
  if (!r->graph)
    return;

  size_t total = r->graph->StatSeriesCount();
  if (total == 0)
    return;

  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - GR_STATS_PANEL_WIDTH,
             viewport->WorkPos.y),
      ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(GR_STATS_PANEL_WIDTH, viewport->WorkSize.y),
                           ImGuiCond_Always);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
  if (!ImGui::Begin("Stats", nullptr, flags)) {
    ImGui::End();
    return;
  }

  // Reused across series/frames so windowing the chart doesn't allocate.
  static std::vector<double> xsBuf;

  for (size_t i = 0; i < total; i++) {
    const gviz::layout::StatSeries *series = r->graph->StatSeriesAt(i);
    if (!series || series->samples.empty())
      continue;
    if (i < r->statsSeriesVisibleCount && !r->statsSeriesVisible[i])
      continue;

    size_t count = series->samples.size();
    size_t start = count > (size_t)GR_STATS_WINDOW_SAMPLES
                       ? count - (size_t)GR_STATS_WINDOW_SAMPLES
                       : 0;
    int n = (int)(count - start);
    xsBuf.resize((size_t)n);
    for (int j = 0; j < n; j++)
      xsBuf[(size_t)j] = (double)(start + (size_t)j);

    ImGui::PushID((int)i);
    if (ImPlot::BeginPlot(series->name, ImVec2(-1, 160),
                          ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
      ImPlot::SetupAxes("sample", "value", ImPlotAxisFlags_None,
                        ImPlotAxisFlags_AutoFit);
      ImPlot::SetupAxisLimits(ImAxis_X1, (double)start,
                              (double)(start + (size_t)(n > 1 ? n - 1 : 1)),
                              ImGuiCond_Always);
      if (series->kind == gviz::layout::StatChartKind::LineLog)
        ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
      ImPlot::PlotLine(series->name, xsBuf.data(),
                       series->samples.data() + start, n);
      ImPlot::EndPlot();
    }
    ImGui::PopID();
  }

  ImGui::End();
}
