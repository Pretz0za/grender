/**
 * Reingold-Tilford algorithm walkthrough -- a scholastic animation, not a
 * production feature.
 *
 * Consumes gviz::layout::ReingoldTilfordTrace's pure-data event log (see
 * ~/gviz/include/ReingoldTilfordTrace.hpp) for a small, hand-picked,
 * depth-asymmetric tree and stages it as a step-by-step animation: every
 * subtree starts in its own isolated "world" on screen and only slides into
 * proximity with another subtree at the exact moment CombineSubtreeLeft is
 * about to compare them; every contour-depth level of the walk plays as two
 * beats -- measure (highlight the pair just stepped to, report how far
 * apart they are, nothing moves) then correct (apply exactly that level's
 * own shortfall, if any, and nothing more) -- shown one at a time; a
 * contour thread appearing
 * gets a persistent, dashed, distinctly-colored connector, never confused
 * with a real tree edge. A parent-to-child edge is only drawn once that
 * child's own subtree is fully placed *and* the parent has finished
 * incorporating it -- never while the child is still off in its own little
 * world. Small text captions narrate the current step. Root renders at the
 * top, tree flowing downward.
 *
 * Playback is entirely manual: press N (or Space) to advance exactly one
 * step. Nothing animates on a timer -- this is deliberate, so a viewer can
 * actually read each step instead of the animation racing ahead on its own.
 *
 * All of the choreography (which subtree lives where on screen, easing,
 * beat sequencing) is grender-side staging logic -- gviz's trace event log
 * is pure data with no rendering concepts in it at all, by design.
 *
 * Usage: rtTraceDemo [screenshot.ppm] [stepsBeforeScreenshot]
 *   Interactive mode (no screenshot path): a real window, step with N/Space.
 *   Screenshot mode: steps are driven programmatically (the same one-step
 *   advance N triggers) so a screenshot can be taken headlessly at a chosen
 *   point in the walkthrough. stepsBeforeScreenshot < 0 (or omitted): stop
 *   right after the first thread-creation beat settles, a pedagogically
 *   interesting frame.
 *
 * Controls:
 *   N, Space - advance one step
 *   F        - fit view
 *   drag     - pan
 *   scroll   - zoom
 */

#include "Graphs.hpp"
#include "grender/grender.h"

#include "gviz.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using gviz::Graph;
using gviz::Subgraph;
using gviz::layout::ActionPayload;
using gviz::layout::EmbeddedGraph;
using gviz::layout::ReingoldTilfordTrace;
using Kind = ReingoldTilfordTrace::EventKind;

namespace {

// Demo-local placement constants -- independent of ReingoldTilford.cpp's/
// ReingoldTilfordTrace.cpp's own kXSeparation/kYSeparation (500/1000):
// this demo never renders the trace's own Embed() output, only positions
// this file computes itself from the trace's *relative* offsets (see
// BuildBeats), so its own choice of scale is free to differ.
constexpr double kXSeparation = 140.0;
constexpr double kYSeparation = 220.0; // per tree level, negated: see main()
// Not too large relative to kXSeparation: an untouched leaf's "own little
// world" needs to read as clearly separate, but grRendererFitView() frames
// *every* live vertex (including ones still off in an unmerged world), so
// making this too generous washes out the actively-compared cluster to a
// speck in an otherwise-empty frame.
constexpr double kWorldSpacing = kXSeparation * 3.0;
constexpr double kMoveSeconds = 0.7; // per-beat ease duration once triggered

using Vec2 = std::array<double, 2>;

Vec2 Add(Vec2 a, Vec2 b) { return {a[0] + b[0], a[1] + b[1]}; }
Vec2 Lerp(Vec2 a, Vec2 b, double t) { return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t}; }

double SmoothStep(double t) {
  t = std::clamp(t, 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

const uint32_t kCompareHighlight = GR_RGBA8(120, 200, 255, 255); // whole subtrees being compared
const uint32_t kContourMeasureHighlight = GR_RGBA8(255, 110, 200, 255); // pair just measured, no correction yet
const uint32_t kContourCorrectHighlight = GR_RGBA8(255, 40, 140, 255); // same pair, deeper/more saturated: the correcting moment
const uint32_t kThreadHighlight = GR_RGBA8(255, 176, 92, 255);
const uint32_t kBaseEdgeColor = GR_RGBA8(140, 158, 184, 165);
const uint32_t kThreadEdgeColor = GR_RGBA8(255, 176, 92, 220);

/**
 * One animated beat of the demo -- deliberately much coarser-grained,
 * screen/color-aware staging state than ReingoldTilfordTrace::Event.
 * Several trace events collapse into one beat's fields (e.g. a
 * ContourMeasure's or ContourCorrect's two contour vertices become a
 * highlight pair); this struct itself is entirely grender-side, never
 * touched by gviz.
 */
struct Beat {
  std::string caption;
  bool hasHighlight = false;
  size_t highlightA = 0, highlightB = 0;
  uint32_t highlightColor = 0;
  std::vector<std::pair<size_t, Vec2>> moves; // vertex -> target world position
  bool addThreadEdge = false;
  size_t threadA = 0, threadB = 0;
  // Parent-to-child edges that become real (visible) as of this beat -- see
  // BuildBeats' class comment: never before the child's own relative
  // offset is fully settled.
  std::vector<std::pair<size_t, size_t>> newEdges;
  double moveSeconds = kMoveSeconds; // 0 = apply instantly, no ease
};

/**
 * Replays a ReingoldTilfordTrace event log into a sequence of Beats. The
 * core staging idea is a tiny union-find over "which vertices currently
 * move together as one rigid group":
 *
 *   - A leaf gets a fresh, far-away "home" world slot the moment it's
 *     isolated (SubtreeIsolated).
 *   - An internal vertex is isolated exactly co-located with its first
 *     child (mirroring CombineSubtreeLeft(root, 0)'s real no-op -- nothing
 *     to merge against yet) and stays its own single-vertex group; each
 *     child subtree remains its own independent, un-merged group.
 *   - ContourCompareBegin slides the *new* child's whole group next to the
 *     bounding box of everything already merged under the same parent --
 *     "only when about to compare do two subtrees animate into proximity."
 *     Deliberately makes them fully overlap (not just close), matching
 *     ReingoldTilfordTrace's own contract: the root pair starts measured at
 *     exactly 0 apart (see ContourMeasure below), not assumed pre-separated.
 *   - Every contour-depth level plays as two beats, mirroring
 *     SeparateAlongContours' own measure-then-correct cycle exactly: a
 *     ContourMeasure event (pure observation -- highlights the pair the
 *     walk just stepped to and reports how far apart they currently are,
 *     nothing moves) immediately followed by its one paired ContourCorrect
 *     event (applies exactly that level's own shortfall, if any -- never
 *     more, never a backlog carried over from an earlier level; see
 *     ReingoldTilfordTrace.hpp's class comment on why summing independent
 *     per-level shortfalls this way still equals the true total). The real
 *     while loop only ever accumulates corrections into a per-gap array
 *     (`newSeparations`, replicated here as curSeparationsRaw, indexed by
 *     Event::ancestor -- see ReingoldTilfordTrace.hpp's doc comment on why
 *     a correction isn't always attributed to the subtree just compared).
 *     But unlike the real algorithm (which only converts that array to
 *     actual coordinates once, after the whole loop finishes), *this*
 *     function re-derives and applies "what the algorithm's answer would
 *     be if the walk stopped right here" -- computeOffsetUnits/
 *     applyCurrentSeparation -- in the very same ContourCorrect beat that
 *     changed the accumulator. This is still an exact computation
 *     (computeOffsetUnits is a pure transcription of SeparateAlongContours'
 *     post-loop math -- prefix-sum, fold in gaps between previously-merged
 *     children, convert to absolute offsets -- run on whatever the
 *     accumulator currently holds, never an approximation of it), just
 *     evaluated incrementally instead of once at the end, specifically so
 *     a beat's caption ("correction of X added") and what moves on screen
 *     in that same beat can never disagree. Every merge's very first
 *     measure/correct cycle is the root pair itself (ContourMeasure always
 *     reports 0 apart there, ContourCorrect always fires), so there is no
 *     "0-iteration walk owes an un-applied baseline separation" case left
 *     to special-case here -- every merge, even one whose while loop never
 *     runs at all, gets at least that one real correction applied through
 *     the same ordinary path as every deeper level. By the time
 *     ExtremesUpdated/ThreadCreated are processed below, every child [0,
 *     childIndex] is already at its true final position -- exactly
 *     matching real execution order (CombineSubtreeLeft calls
 *     SeparateAlongContours to completion, *then* UpdateExtremes, *then*
 *     CreateThreads): extremes and threads are never computed against a
 *     still-overlapping pair.
 *   - SubtreeMerged's own move is therefore normally a no-op (every child's
 *     position is already exact) -- kept anyway as a cheap safety net
 *     using the trace's own authoritative offsets, and as the trigger for
 *     folding finished children into their parent's group (see
 *     finalizeVertex) once a vertex's last merge completes.
 *
 * Y is simpler: set once per vertex from the trace's own (real, see
 * ReingoldTilfordTrace's class comment) depth and never touched again --
 * exactly mirroring how Embed() computes Y independently of the X-offset
 * bookkeeping.
 */
std::vector<Beat> BuildBeats(const Graph &tree,
                              const std::vector<ReingoldTilfordTrace::Event> &events) {
  size_t n = tree.Size();
  std::vector<Vec2> pos(n, Vec2{0.0, 0.0});
  std::vector<size_t> blobRoot(n);
  for (size_t i = 0; i < n; i++)
    blobRoot[i] = i;
  std::vector<int> mergesRemaining(n, 0);
  size_t nextSlot = 0;
  std::vector<Beat> beats; // declared early: finalizeVertex (below) pushes into it directly

  auto find = [&](size_t x) {
    while (blobRoot[x] != x)
      x = blobRoot[x];
    return x;
  };
  auto groupMembers = [&](size_t root) {
    std::vector<size_t> members;
    for (size_t x = 0; x < n; x++)
      if (find(x) == root)
        members.push_back(x);
    return members;
  };
  auto translateGroup = [&](size_t groupRoot, Vec2 delta,
                             std::vector<std::pair<size_t, Vec2>> &moves) {
    if (std::fabs(delta[0]) < 1e-9 && std::fabs(delta[1]) < 1e-9)
      return;
    for (size_t x : groupMembers(groupRoot)) {
      pos[x] = Add(pos[x], delta);
      moves.push_back({x, pos[x]});
    }
  };
  // Folds every child 0..degree-1 of v into v's own group, and pushes a
  // dedicated follow-up beat that reveals v's parent-child edges -- called
  // exactly once per internal vertex, at the moment its *last* merge
  // finishes (or immediately, for a single-child vertex that never gets a
  // CombineSubtreeLeft call at all).
  //
  // Deliberately its own beat, not folded into whichever beat triggered it
  // (a real, separate acceptance requirement, not just tidiness): by the
  // time a parent's edges are drawn, every child must already be sitting
  // at its final, fully-settled relative position -- nothing may still be
  // easing toward it. Bundling the edge reveal into the *same* beat as the
  // position-settling move would show the edges connect while the
  // children are still visibly sliding into place; pushing it as its own
  // no-motion beat *after* that move's beat has already been advanced past
  // (see AdvanceOneStep -- a beat only becomes "current" once the previous
  // one's easing has fully finished) guarantees the children are at rest
  // first.
  auto finalizeVertex = [&](size_t v) {
    size_t degree = tree.Degree(v);
    Beat edgeBeat;
    edgeBeat.moveSeconds = 0.0;
    edgeBeat.caption = "All of " + std::to_string(v) +
                       "'s children are fully placed relative to it -- drawing its edges.";
    for (size_t k = 0; k < degree; k++) {
      size_t ck = tree.Neighbor(v, k);
      blobRoot[find(ck)] = v;
      edgeBeat.newEdges.push_back({v, ck});
    }
    beats.push_back(std::move(edgeBeat));
  };

  // Running state for whichever merge's ContourCompareBegin..SubtreeMerged
  // block is currently being processed (these always form one contiguous
  // run in the event log -- see CombineSubtreeLeft's structure).
  // curSeparationsRaw is a faithful, live copy of SeparateAlongContours' own
  // `newSeparations` array *before* its post-loop prefix-sum conversion:
  // index k accumulates the raw correction for the gap right after child k,
  // exactly as Event::ancestor identifies it. Never mutated in place by the
  // conversion below -- computeOffsetUnits always works off a copy -- so it
  // can be recomputed from after every single correction, not just once at
  // the end. No lookahead (e.g. a step-count-per-merge map) is needed to
  // drive this anymore: every merge's ContourCompareBegin is now
  // unconditionally followed by at least one ContourMeasure/ContourCorrect
  // cycle (the root-level one, which always fires -- see
  // ReingoldTilfordTrace.hpp's class comment), so the old "0-iteration walk
  // still owes its baseline separation, apply it as a special case" branches
  // this file used to need are gone; the baseline separation is now just an
  // ordinary correction like any other, applied the same way in the same
  // place as every deeper level's.
  std::vector<double> curSeparationsRaw;

  // Exact transcription of SeparateAlongContours' post-loop math (see
  // ReingoldTilfordTrace.cpp): prefix-sum the raw per-gap corrections, fold
  // in whatever spacing already exists between previously-merged children,
  // then convert to absolute per-child offsets. i is the merge's
  // childIndex (== curSeparationsRaw.size()). Pure -- doesn't touch
  // curSeparationsRaw -- so it can be called after *every* accumulator
  // update to get "what the algorithm's answer would be if the walk
  // stopped right here," not just once at the very end.
  auto computeOffsetUnits = [&](size_t v, size_t i) {
    std::vector<double> work = curSeparationsRaw;
    double acc = 0.0;
    for (size_t k = 0; k < i; k++) {
      acc += work[k];
      work[k] = acc;
    }
    // Fold in whatever spacing already exists between previously-merged
    // children -- recovered from their current world positions, which the
    // pos[child] == pos[v] + offsetUnits*kXSeparation invariant (maintained
    // by every translateGroup call in this function) makes exact. These
    // children's positions never change during this merge's own walk, so
    // this is safe to recompute on every call.
    if (i > 1) {
      for (size_t k = 0; k + 1 < i; k++) {
        double gapUnits = std::fabs((pos[tree.Neighbor(v, k + 1)][0] - pos[tree.Neighbor(v, k)][0]) /
                                     kXSeparation);
        work[k] += gapUnits;
      }
    }
    double total = 0.0;
    for (size_t k = 0; k < i; k++)
      total += work[k];
    std::vector<double> offsetUnits(i + 1);
    offsetUnits[0] = -total / 2.0;
    for (size_t k = 1; k <= i; k++)
      offsetUnits[k] = offsetUnits[k - 1] + work[k - 1];
    return offsetUnits;
  };

  // Applies computeOffsetUnits' *current* answer to every child [0, i] of
  // v, into whichever beat is currently being built -- called inline, in
  // the very same beat whose caption reports the correction that just
  // changed curSeparationsRaw, so what the text says and what moves on
  // screen can never disagree (they're computed from the same state, in
  // the same step).
  auto applyCurrentSeparation = [&](size_t v, size_t i, Beat &b) {
    std::vector<double> offsetUnits = computeOffsetUnits(v, i);
    for (size_t k = 0; k <= i; k++) {
      size_t ck = tree.Neighbor(v, k);
      double targetX = pos[v][0] + offsetUnits[k] * kXSeparation;
      Vec2 delta = {targetX - pos[ck][0], 0.0};
      translateGroup(find(ck), delta, b.moves);
    }
  };

  for (const auto &e : events) {
    switch (e.kind) {
    case Kind::SubtreeIsolated: {
      size_t v = e.root;
      size_t degree = tree.Degree(v);
      Beat b;
      bool singleChildDone = false;
      if (degree == 0) {
        pos[v] = {static_cast<double>(nextSlot) * kWorldSpacing,
                   -static_cast<double>(e.level) * kYSeparation};
        nextSlot++;
        b.caption = "Vertex " + std::to_string(v) + " is a leaf -- its own little world, for now.";
      } else {
        size_t child0 = tree.Neighbor(v, 0);
        pos[v] = {pos[child0][0], -static_cast<double>(e.level) * kYSeparation};
        mergesRemaining[v] = static_cast<int>(degree) - 1;
        b.caption = "Vertex " + std::to_string(v) + " isolated above its first child (" +
                    std::to_string(child0) + "); " + std::to_string(degree - 1) +
                    (degree - 1 == 1 ? " sibling still to merge in." : " siblings still to merge in.");
        singleChildDone = (degree == 1); // no CombineSubtreeLeft(v, i>0) will ever fire
      }
      b.moves.push_back({v, pos[v]});
      beats.push_back(std::move(b));
      // finalizeVertex pushes its own, separate, no-motion beat -- only
      // after this SubtreeIsolated beat has already been fully advanced
      // past, so the single child is already at rest when v's edge to it
      // is drawn (see finalizeVertex's comment).
      if (singleChildDone)
        finalizeVertex(v);
      break;
    }
    case Kind::ContourCompareBegin: {
      size_t v = e.root, i = e.childIndex;

      // Start fully overlapping vertexA (the left blob's current
      // comparison point) -- the classic teaching framing: assume no
      // separation is needed yet, then let the contour walk reveal and
      // correct any collision, one step at a time.
      Vec2 delta = {pos[e.vertexA][0] - pos[e.vertexB][0], 0.0};

      Beat b;
      translateGroup(find(e.vertexB), delta, b.moves);
      b.caption = "Comparing subtree " + std::to_string(e.vertexB) +
                  " against the blob merged so far under " + std::to_string(v) +
                  " -- starting overlapped with " + std::to_string(e.vertexA) + ".";
      b.hasHighlight = true;
      b.highlightA = e.vertexA;
      b.highlightB = e.vertexB;
      b.highlightColor = kCompareHighlight;
      beats.push_back(std::move(b));

      // Fresh accumulator for this merge -- all zero. Unlike an earlier
      // version of this file, nothing is pre-seeded here: the root-level
      // pair (lrContour/rlContour, exactly e.vertexA/e.vertexB above) gets
      // its own ordinary ContourMeasure/ContourCorrect cycle next in the
      // event log, exactly like every deeper level, and that correction is
      // what supplies curSeparationsRaw[i - 1]'s value -- pre-seeding it
      // here too would double it.
      curSeparationsRaw.assign(i, 0.0);
      break;
    }
    case Kind::ContourMeasure: {
      // Pure observation beat: the contour walk just stepped one level
      // deeper (blob's right contour rightward, new subtree's left
      // contour leftward) and landed on vertexA/vertexB -- highlight that
      // pair and report how far apart they currently sit, in the same
      // offset units applyCurrentSeparation works in. Nothing moves this
      // beat; the immediately-following ContourCorrect decides whether
      // anything needs to.
      Beat b;
      char buf[224];
      snprintf(buf, sizeof(buf),
               "Measuring: right contour at %zu, left contour at %zu -- currently %.2f apart "
               "(need at least 1.00).",
               e.vertexA, e.vertexB, static_cast<double>(e.measuredSeparation));
      b.caption = buf;
      b.hasHighlight = true;
      b.highlightA = e.vertexA;
      b.highlightB = e.vertexB;
      b.highlightColor = kContourMeasureHighlight;
      b.moveSeconds = 0.0;
      beats.push_back(std::move(b));
      break;
    }
    case Kind::ContourCorrect: {
      // The correction (if any) for the pair the immediately-preceding
      // ContourMeasure just reported on. Every ContourMeasure gets exactly
      // one ContourCorrect -- this beat never accumulates across several
      // levels or defers a shortfall for a later beat to absorb; whatever
      // curSeparationsRaw gains here is exactly this one level's own
      // requirement (see ReingoldTilfordTrace.hpp's class comment).
      // Applying it immediately, in this same beat, keeps the caption and
      // what moves on screen from ever disagreeing (same rationale as
      // before this event was split out of the old combined ContourStep).
      size_t v = e.root, i = e.childIndex;
      Beat b;

      // Known, investigated consequence of applying each level's
      // correction immediately as its own beat, still worth flagging here:
      // a single top-level gap can need more than one ContourCorrect (one
      // per contour depth) before it's fully resolved -- e.g. two matched-
      // depth "bushy" blobs merging under a shared root first correct at
      // the blob-root level, then again at their deepest matched
      // grandchild level. Applying a still-partial correction to the
      // *whole* rigid blob (translateGroup moves every descendant
      // together, mirroring how Embed() itself derives a descendant's
      // absolute position from its ancestor chain of offsets) is a
      // mathematically exact snapshot of that intermediate algorithm
      // state, not an approximation -- but it can transiently show deep
      // descendants on opposite sides of the merge crossing on screen
      // until the next level's ContourCorrect (to the *same* gap)
      // finishes the job. Confirmed via gviz's ReingoldTilfordTraceTests.cpp
      // that the underlying event log is not at fault: each correction is
      // independently and correctly computed and attributed, and the
      // final positions (once the merge's last ContourCorrect lands) are
      // always properly ordered with no crossing.
      if (e.correctionFired) {
        curSeparationsRaw[e.ancestor] += e.correctionAmount;
        applyCurrentSeparation(v, i, b);
        char buf[224];
        snprintf(buf, sizeof(buf),
                 "Correcting: added %.2f to the gap after child %zu -- %zu and %zu are now exactly "
                 "1.00 apart.",
                 static_cast<double>(e.correctionAmount), e.ancestor, e.vertexA, e.vertexB);
        b.caption = buf;
      } else {
        b.caption = "Correcting: " + std::to_string(e.vertexA) + " and " + std::to_string(e.vertexB) +
                    " are already at least 1.00 apart -- nothing to add.";
      }
      b.hasHighlight = true;
      b.highlightA = e.vertexA;
      b.highlightB = e.vertexB;
      b.highlightColor = kContourCorrectHighlight;
      beats.push_back(std::move(b));
      break;
    }
    case Kind::ThreadCreated: {
      Beat b;
      b.caption = "Thread created: " + std::to_string(e.vertexA) + " now jumps straight to " +
                  std::to_string(e.vertexB) +
                  " for future contour walks (the two sides had unequal depth).";
      b.hasHighlight = true;
      b.highlightA = e.vertexA;
      b.highlightB = e.vertexB;
      b.highlightColor = kThreadHighlight;
      b.addThreadEdge = true;
      b.threadA = e.vertexA;
      b.threadB = e.vertexB;
      b.moveSeconds = 0.0;
      beats.push_back(std::move(b));
      break;
    }
    case Kind::ExtremesUpdated: {
      Beat b;
      b.caption = "Blob extremes updated: leftmost is now " + std::to_string(e.vertexA) +
                  ", rightmost " + std::to_string(e.vertexB) + ".";
      b.moveSeconds = 0.0;
      beats.push_back(std::move(b));
      break;
    }
    case Kind::SubtreeMerged: {
      size_t v = e.root, i = e.childIndex;
      Beat b;
      // applyCurrentSeparation already put every child [0, i] at its true
      // final position, incrementally, as each correction fired above --
      // this loop re-targets against the trace's own authoritative offsets
      // as a cheap safety net (should always be a no-op; translateGroup
      // skips near-zero deltas), not as where the actual separation
      // happens.
      for (size_t k = 0; k <= i; k++) {
        size_t ck = tree.Neighbor(v, k);
        double targetX = pos[v][0] + static_cast<double>(e.offsets[k]) * kXSeparation;
        Vec2 delta = {targetX - pos[ck][0], 0.0};
        translateGroup(find(ck), delta, b.moves);
      }
      mergesRemaining[v]--;
      bool allChildrenPlaced = mergesRemaining[v] == 0;
      b.caption = "Merge complete: child " + std::to_string(i) + " of " + std::to_string(v) +
                  " folded in.";
      b.moveSeconds = 0.0;
      beats.push_back(std::move(b));
      // Again, finalizeVertex's edge-reveal beat only gets pushed *after*
      // this merge's own beat -- so it only becomes "current" (see
      // AdvanceOneStep) once this move has fully eased and every child of
      // v is genuinely at rest, never mid-slide.
      if (allChildrenPlaced)
        finalizeVertex(v);
      break;
    }
    case Kind::EmbedStep:
      // Collapsed into one closing beat below, appended by the caller --
      // this demo already renders the correct positions incrementally as
      // CalculateOffsets' own events play out, so replaying Embed's
      // redundant per-vertex assignment one more time would just repeat
      // beats already shown.
      break;
    }
  }

  Beat finalBeat;
  finalBeat.caption =
      "Layout complete -- " + std::to_string(n) + " vertices placed, root at the top.";
  finalBeat.moveSeconds = 0.0;
  beats.push_back(std::move(finalBeat));

  return beats;
}

/**
 * A small, hand-picked, depth-asymmetric tree, empirically verified (see
 * the task exploration) to exercise everything the brief asks for:
 *
 *   0 (root)
 *   +- 1 -> {4, 5}              (two leaf children)
 *   +- 2 -> {7, 8, 12}          (three leaf children)
 *   +- 3 -> {9, 10}
 *           9 -> {11, 6}        (two leaf children)
 *           10                  (leaf)
 *
 * Root has three children (a real 3-way merge sequence: CombineSubtreeLeft
 * folds in child 1, then child 2, i.e. tree-vertex 2, then child 3, i.e.
 * tree-vertex 3). Merging {1} with {2} (root's first merge) and merging
 * {1,2} with {3} (root's second merge) each walk the contours down exactly
 * one level before one side hits a leaf -- each SeparateAlongContours call
 * in this tree resolves in a single ContourMeasure/ContourCorrect pair.
 * Merging 3's own children
 * ({9}'s subtree vs leaf 10) triggers exactly one contour thread
 * (immediately, since 10 is a leaf from the start). Height 3 (0 -> 3 -> 9
 * -> 11/6).
 */
Graph BuildDemoTree() {
  Graph g(/*directed=*/true, 13);
  for (int i = 0; i < 13; i++)
    g.AddVertex();
  g.AddEdge(0, 1, 1.0);
  g.AddEdge(0, 2, 1.0);
  g.AddEdge(0, 3, 1.0);
  g.AddEdge(1, 4, 1.0);
  g.AddEdge(1, 5, 1.0);
  g.AddEdge(2, 7, 1.0);
  g.AddEdge(2, 8, 1.0);
  g.AddEdge(2, 12, 1.0);
  g.AddEdge(3, 9, 1.0);
  g.AddEdge(3, 10, 1.0);
  g.AddEdge(9, 11, 1.0);
  g.AddEdge(9, 6, 1.0);
  return g;
}

/** All mutable playback state, threaded through the plain-function-pointer
 *  ActionHandler via userData (see EmbeddedGraph::AddAction's doc comment
 *  on why it's a function pointer, not a capturing std::function). */
struct DemoState {
  const std::vector<Beat> *beats = nullptr;
  Graph *display = nullptr;
  EmbeddedGraph *stage = nullptr;
  grRenderer *r = nullptr;
  std::vector<Vec2> *live = nullptr;
  std::vector<Vec2> beatStart;
  std::vector<std::pair<size_t, size_t>> threadEdges;

  int currentBeat = -1; // -1: nothing applied yet
  double elapsed = 0.0;
  bool animating = false;

  // grRendererGraphStructureChanged only *marks* the topology dirty --
  // grRendererFrame's own lazy rebuild (uploadTopology) doesn't run until
  // later in that same frame, well after actions are dispatched (see
  // processInput's call site relative to the r->topoDirty check in
  // grRendererFrame). Calling grRendererSetEdgeColors/SetEdgeDashed
  // synchronously from inside the action handler -- as this file used to
  // -- reads/writes against the *stale* pre-change edge-buffer order, and
  // the real rebuild then discards that upload as stale too (correctly:
  // uploadTopology resets hasEdgeColors/hasEdgeDashed on every structural
  // change). So: just remember a recolor is owed, and do it from the main
  // loop *after* grRendererFrame() returns, once the rebuild has actually
  // happened.
  bool pendingRecolor = false;
};

/**
 * Recomputes every edge's persistent color and dash flag from scratch, in
 * the graph's *current* edge-buffer order -- must re-run after any
 * structural change (grRendererGraphStructureChanged invalidates both
 * arrays automatically, per grRendererSetEdgeColors/SetEdgeDashed's
 * documented contract).
 */
void RecolorEdges(grRenderer *r, const std::vector<std::pair<size_t, size_t>> &threadEdges) {
  size_t count = grRendererEdgeCount(r);
  std::vector<uint32_t> ids(count * 2);
  grRendererGetEdges(r, ids.data());
  std::vector<uint32_t> colors(count, kBaseEdgeColor);
  std::vector<uint32_t> dashed(count, 0);
  for (size_t i = 0; i < count; i++) {
    uint32_t u = ids[2 * i], v = ids[2 * i + 1];
    for (auto &te : threadEdges) {
      if ((te.first == u && te.second == v) || (te.first == v && te.second == u)) {
        colors[i] = kThreadEdgeColor;
        dashed[i] = 1;
      }
    }
  }
  grRendererSetEdgeColors(r, colors.data(), colors.size());
  grRendererSetEdgeDashed(r, dashed.data(), dashed.size());
}

/** Applies beat s.currentBeat's discrete side effects (new edges, thread
 *  edge, highlight, caption) and either snaps instantly (moveSeconds == 0)
 *  or starts easing toward its target positions. Called once per step,
 *  whether that step was triggered by a key press or (in screenshot mode)
 *  driven programmatically -- see main(). */
void ApplyCurrentBeat(DemoState &s) {
  const Beat &b = (*s.beats)[s.currentBeat];
  s.beatStart = *s.live;

  bool structureChanged = false;
  for (auto &edge : b.newEdges) {
    if (!s.display->EdgeExists(edge.first, edge.second)) {
      s.display->AddEdge(edge.first, edge.second, 1.0);
      structureChanged = true;
    }
  }
  if (b.addThreadEdge && !s.display->EdgeExists(b.threadA, b.threadB)) {
    s.display->AddEdge(b.threadA, b.threadB, 1.0);
    s.threadEdges.push_back({b.threadA, b.threadB});
    structureChanged = true;
  }
  if (structureChanged) {
    grRendererGraphStructureChanged(s.r);
    s.pendingRecolor = true; // see DemoState::pendingRecolor
  }

  if (b.hasHighlight) {
    Subgraph hl = Subgraph::CreateVertexInduced(*s.display);
    hl.ShowVertex(b.highlightA);
    hl.ShowVertex(b.highlightB);
    grRendererSetHighlight(s.r, hl, b.highlightColor, b.highlightColor);
  } else {
    grRendererClearHighlight(s.r);
  }
  grRendererSetCaption(s.r, b.caption.c_str());

  if (b.moveSeconds <= 0.0) {
    for (auto &mv : b.moves)
      (*s.live)[mv.first] = mv.second;
    for (size_t v = 0; v < s.live->size(); v++)
      s.stage->SetVPosition(v, (*s.live)[v].data());
    s.animating = false;
  } else {
    s.animating = true;
    s.elapsed = 0.0;
  }
}

/** Advances exactly one step, if the current beat isn't still easing --
 *  mid-animation presses are coalesced (ignored) rather than queued, so
 *  "one press = one step" holds even for an impatient double-press. */
void AdvanceOneStep(DemoState &s) {
  if (s.animating)
    return;
  if (s.currentBeat + 1 >= static_cast<int>(s.beats->size()))
    return;
  s.currentBeat++;
  ApplyCurrentBeat(s);
}

void ActionNextStep(EmbeddedGraph &embedding, void *userData, const ActionPayload &payload) {
  (void)embedding;
  (void)payload;
  AdvanceOneStep(*static_cast<DemoState *>(userData));
}



} // namespace


static size_t karyTreeVertexCount(size_t branching, size_t depth) {
  if (branching <= 1)
    return depth + 1;

  size_t count = 0;
  size_t levelSize = 1;
  for (size_t d = 0; d <= depth; d++) {
    count += levelSize;
    levelSize *= branching;
  }
  return count;
}

static void addKarySubtree(gviz::Graph &g, size_t parent, size_t branching,
                           size_t remainingDepth) {
  if (remainingDepth == 0)
    return;

  for (size_t i = 0; i < branching; i++) {
    size_t child = g.Size();
    g.AddVertex();
    g.AddEdge(parent, child, 1.0);
    addKarySubtree(g, child, branching, remainingDepth - 1);
  }
}

static gviz::Graph buildKaryTree(size_t branching, size_t depth) {
  size_t n = karyTreeVertexCount(branching, depth);
  gviz::Graph g(true, n);
  g.AddVertex();
  addKarySubtree(g, 0, branching, depth);
  return g;
}

int main(int argc, char **argv) {
  const char *screenshotPath = argc > 1 ? argv[1] : nullptr;
  int stepsBeforeScreenshot = argc > 2 ? atoi(argv[2]) : -1;

  Graph tree = gviz::graphs::BuildRandomConnectedGraph(50, 0, time(NULL), true);
  tree.BuildLayout();

  std::optional<ReingoldTilfordTrace> trace;
  try {
    trace.emplace(tree, 0);
  } catch (const std::exception &e) {
    fprintf(stderr, "trace construction failed: %s\n", e.what());
    return 1;
  }
  trace->CalculateOffsets(0, 0);
  double origin[2] = {0.0, 0.0};
  // Only to populate EmbedStep events for completeness/parity with
  // ReingoldTilford's own workflow -- this demo never renders these
  // positions directly, see BuildBeats' class comment.
  trace->Embed(0, origin);

  std::vector<Beat> beats = BuildBeats(tree, trace->Events());
  fprintf(stderr, "%zu-vertex tree, %zu steps, height %zu\n", tree.Size(), beats.size(),
          trace->Height());

  if (stepsBeforeScreenshot < 0) {
    stepsBeforeScreenshot = static_cast<int>(beats.size()); // default: run to the end
    for (size_t i = 0; i < beats.size(); i++) {
      if (beats[i].addThreadEdge) {
        stepsBeforeScreenshot = static_cast<int>(i) + 1;
        break;
      }
    }
  }

  // Separate, undirected "display" graph: starts with every vertex but NO
  // edges at all (see BuildBeats' class comment -- a parent-child edge
  // only becomes real once that child's own subtree is fully placed), and
  // additionally grows extra edges over the course of playback to
  // visualize contour threads. This is grender's own answer to Subgraph
  // never exposing its parent Graph& (see this repo's grInternal.h
  // backingGraph convention): anything needing raw parent-graph mutation
  // holds its own Graph& alongside what it hands the renderer.
  Graph display(/*directed=*/false, tree.Size());
  for (size_t i = 0; i < tree.Size(); i++)
    display.AddVertex();

  // gviz::layout::EmbeddedGraph (bare, no algorithm) no longer takes a
  // Subgraph at all -- it's now a plain vertexCount+dimension buffer,
  // entirely decoupled from any structure (see EmbeddedGraph.hpp's class
  // doc). `sg` is kept alive separately and handed to grRendererSetGraph so
  // grender can still draw edges from `display`'s live structure.
  Subgraph sg = Subgraph::CreateVertexInduced(display);
  for (size_t v = 0; v < display.Size(); v++)
    sg.ShowVertex(v);
  EmbeddedGraph stage(display.Size(), 2);

  grRendererDesc desc;
  grRendererDescInit(&desc);
  desc.title = "grender - Reingold-Tilford walkthrough (N/Space = step)";
  desc.nodeStyle.radius = 16.0f;
  // desc.nodeStyle.strokeColor = GR_COLOR(0.08f, 0.10f, 0.14f, 1.0f);
  desc.nodeStyle.strokeWidth = 2.5f;
  // desc.edgeStyle.color = GR_COLOR(0.55f, 0.62f, 0.72f, 0.65f);
  desc.edgeStyle.width = 2.0f;

  grRenderer *r = grRendererCreate(&desc);
  if (!r) {
    fprintf(stderr, "renderer creation failed\n");
    return 1;
  }
  if (grRendererSetGraph(r, sg, stage, &display) < 0) {
    fprintf(stderr, "graph attach failed\n");
    grRendererDestroy(r);
    return 1;
  }

  std::vector<Vec2> live(display.Size(), Vec2{0.0, 0.0});
  for (size_t v = 0; v < display.Size(); v++)
    stage.SetVPosition(v, live[v].data());
  grRendererFitView(r);

  DemoState state;
  state.beats = &beats;
  state.display = &display;
  state.stage = &stage;
  state.r = r;
  state.live = &live;

  stage.AddAction("rtTraceDemo.next", ActionNextStep, &state);
  grRendererBindKey(r, 'N', "rtTraceDemo.next");
  grRendererBindKey(r, GR_KEY_SPACE, "rtTraceDemo.next");
  grRendererSetCaption(r, "Press N or Space to begin the Reingold-Tilford walkthrough.");

  int stepsRemaining = screenshotPath ? stepsBeforeScreenshot : 0;
  bool screenshotTaken = false;

  while (grRendererFrame(r)) {
    double dt = grRendererDeltaTime(r);

    // grRendererFrame() has already run this frame's lazy topology rebuild
    // (if any structural change was requested during action dispatch, still
    // within this same call) by the time it returns -- see
    // DemoState::pendingRecolor's comment for why recoloring must wait
    // until here rather than happening inline in the action handler.
    if (state.pendingRecolor) {
      RecolorEdges(r, state.threadEdges);
      state.pendingRecolor = false;
    }

    if (state.animating) {
      const Beat &b = beats[state.currentBeat];
      state.elapsed += dt;
      double t = SmoothStep(state.elapsed / b.moveSeconds);
      for (auto &mv : b.moves)
        live[mv.first] = Lerp(state.beatStart[mv.first], mv.second, t);
      for (size_t v = 0; v < live.size(); v++)
        stage.SetVPosition(v, live[v].data());
      if (state.elapsed >= b.moveSeconds)
        state.animating = false;
    }

    // Screenshot mode drives the exact same one-step-at-a-time advance a
    // real N/Space press would, just programmatically and without waiting
    // on a human: step once whenever settled, until stepsRemaining reaches
    // 0 and the last step's ease has finished.
    if (screenshotPath && !screenshotTaken) {
      if (stepsRemaining > 0 && !state.animating) {
        AdvanceOneStep(state);
        stepsRemaining--;
      } else if (stepsRemaining == 0 && !state.animating) {
        // The demo never auto-refits the camera during playback (so a real
        // user's own F/drag/scroll framing is never fought) -- but for a
        // headless verification screenshot there's no user to press F, so
        // frame the current state once right before capturing it.
        grRendererFitView(r);
        if (grRendererSaveScreenshot(r, screenshotPath) == 0)
          printf("screenshot saved to %s (step %d/%zu)\n", screenshotPath, state.currentBeat + 1,
                 beats.size());
        else
          fprintf(stderr, "screenshot failed\n");
        screenshotTaken = true;
        grRendererRequestClose(r);
      }
    }
  }

  grRendererDestroy(r);
  return 0;
}
