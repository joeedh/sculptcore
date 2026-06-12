#pragma once

#include "litestl/util/vector.h"

#include <cstdio>

/* Granular split-sliver oscillation detection for the dyntopo round loop.
 *
 * Default-off diagnostic: when DynTopoParams::trace is set, applyBrushDab
 * appends one RoundQuality per round, measuring the triangle quality of the
 * geometry that round perturbed (its frontier faces inside the dab). A healthy
 * dab improves the worst angle monotonically; the split bug instead drives the
 * region into thin "sliver" triangles for a few rounds and then heals back out
 * — sometimes oscillating in and out repeatedly. That transient is invisible to
 * a final-state survey but is both numerically unstable and a perf loss (work
 * spent splitting/flipping geometry that is undone), so we detect it per round.
 * See documentation/dynamic-topology.md.
 */

namespace sculptcore::dyntopo {

/* One round's triangle-quality snapshot over the frontier faces it touched.
 * Angles are radians; the op counts are the ops *applied that round* (deltas,
 * not cumulative), so a sliver burst can be correlated with the split spike. */
struct RoundQuality {
  int iter = 0;  // outer pre-pass iteration (0 for a bare dab); stamped by the caller
  int round = 0; // dab-local round index (resets each dab / outer iter)
  int splits = 0, collapses = 0, flips = 0, smooths = 0;
  int tri_count = 0;          // frontier triangles measured this round
  int thin_count = 0;         // tris with min interior angle < thin_angle
  float min_angle = 0.0f;     // worst per-tri min interior angle this round
  float mean_min_angle = 0.0f; // mean of the per-tri min angles
  /* Band pressure: the candidates queued this round (before independent-set
   * filtering) and how far the worst one sits outside its own graded band. A
   * healthy dab's counts decay geometrically toward 0; pathological splitting
   * shows up as split candidates feeding collapse candidates round after round
   * (max_over pinned near l_max/l_min·0.5, the band-overlap signature). */
  int split_cands = 0, collapse_cands = 0;
  float max_over = 0.0f;  // worst split overshoot, edge_len/tmax (0 = none)
  float min_under = 0.0f; // worst collapse undershoot, edge_len/tmin (0 = none)
};

struct DynTopoTrace {
  /* A triangle whose smallest interior angle is below this (radians) is "thin".
   * 15 degrees by default — a long thin sliver sits well under it. */
  float thin_angle = 0.2617994f;
  litestl::util::Vector<RoundQuality> rounds;
  void clear() { rounds.clear(); }
};

/* Verdict over a completed trace, covering the two oscillation modes the split bug
 * produces:
 *
 *  - thin-triangle (sliver) oscillation: `swings` counts thin-triangle bursts that
 *    appeared and then healed (a rise then fall in thin_count, each leg at least
 *    `min_swing` triangles); `oscillated` is swings >= 1. `peak_round`/`peak_thin`
 *    mark the worst burst and `worst_min_angle` the global worst angle; `healed` is
 *    end thin_count <= the first round's (the region recovered by the end).
 *
 *  - split<->collapse limit cycle (wasted work): `churn_run` is the longest run of
 *    consecutive "churn" rounds within one dab — rounds doing only a few
 *    vertex-count-changing ops (split+collapse <= churn_op_max) so they make no net
 *    topological progress. A healthy dab nibbles briefly then terminates (short
 *    run); a dab stuck ping-ponging one edge churns until the round cap (long run).
 *    `churn_iter` is the outer pre-pass iter that run occurred in. */
struct OscillationReport {
  bool oscillated = false;
  bool healed = false;
  int swings = 0;
  int peak_round = -1;
  int peak_thin = 0;
  int start_thin = 0;
  int end_thin = 0;
  float worst_min_angle = 0.0f;
  int churn_run = 0;   // longest run of low-op churn rounds within one dab
  int churn_iter = -1; // outer iter that run fell in (0 for a bare dab)
};

/* Hysteresis turning-point counter over the thin_count series: a confirmed rise
 * of >= min_swing then a confirmed fall of >= min_swing is one healed burst. Also
 * scans for the split<->collapse limit cycle: the longest run of consecutive rounds
 * doing <= churn_op_max split+collapse ops (no net topological progress) within one
 * dab — dabs are delimited by round==0, so a churn tail never spans dab/iter ends. */
inline OscillationReport detectOscillation(const DynTopoTrace &t, int min_swing = 1,
                                           int churn_op_max = 2)
{
  OscillationReport r;
  int n = int(t.rounds.size());
  if (n == 0) {
    return r;
  }
  r.start_thin = t.rounds[0].thin_count;
  r.end_thin = t.rounds[n - 1].thin_count;
  r.peak_thin = t.rounds[0].thin_count;
  r.peak_round = t.rounds[0].round;
  r.worst_min_angle = t.rounds[0].min_angle;

  int dir = 0;                       // 0 flat, +1 rising, -1 falling
  int ext = t.rounds[0].thin_count;  // running extreme since the last turn
  for (int i = 0; i < n; i++) {
    const RoundQuality &q = t.rounds[i];
    if (q.thin_count > r.peak_thin) {
      r.peak_thin = q.thin_count;
      r.peak_round = q.round;
    }
    if (q.min_angle < r.worst_min_angle) {
      r.worst_min_angle = q.min_angle;
    }
    if (dir == 1) {
      if (q.thin_count > ext) {
        ext = q.thin_count; // keep climbing; track the peak
      } else if (ext - q.thin_count >= min_swing) {
        r.swings++;         // fell back from a peak: one burst healed
        dir = -1;
        ext = q.thin_count;
      }
    } else if (dir == -1) {
      if (q.thin_count < ext) {
        ext = q.thin_count; // keep falling; track the valley
      } else if (q.thin_count - ext >= min_swing) {
        dir = 1;            // rose back out of a valley: a new burst begins
        ext = q.thin_count;
      }
    } else {
      if (q.thin_count - ext >= min_swing) {
        dir = 1;
        ext = q.thin_count;
      } else if (ext - q.thin_count >= min_swing) {
        dir = -1;
        ext = q.thin_count;
      }
    }
  }
  r.oscillated = r.swings >= 1;
  r.healed = r.end_thin <= r.start_thin;

  // Limit-cycle scan: longest run of consecutive churn rounds, reset at each dab
  // boundary (round==0). A round is churn when it moves few verts (split+collapse
  // <= churn_op_max) — productive decimation/refinement rounds do many more.
  int run = 0;
  for (int i = 0; i < n; i++) {
    const RoundQuality &q = t.rounds[i];
    if (q.round == 0) {
      run = 0; // new dab / outer iter: don't bridge a churn tail across the break
    }
    run = (q.splits + q.collapses) <= churn_op_max ? run + 1 : 0;
    if (run > r.churn_run) {
      r.churn_run = run;
      r.churn_iter = q.iter;
    }
  }
  return r;
}

/* Per-outer-iter rollup of a trace + the oscillation verdict (the Tier-0d
 * convergence stat the CLI/debug app print; printTrace below is the per-round
 * dump). A healthy run decays rounds/ops geometrically across iters. */
inline void printTraceSummary(const DynTopoTrace &t, const char *tag)
{
  const float k = 180.0f / 3.14159265358979323846f;
  int n = int(t.rounds.size());
  for (int i = 0; i < n;) {
    int iter = t.rounds[i].iter;
    int rounds = 0, s = 0, c = 0, fl = 0, thin_peak = 0;
    float worstOver = 0.0f, worstUnder = 1e30f, worstAng = 1e30f;
    const RoundQuality *last = nullptr;
    for (; i < n && t.rounds[i].iter == iter; i++) {
      const RoundQuality &q = t.rounds[i];
      rounds++;
      s += q.splits;
      c += q.collapses;
      fl += q.flips;
      if (q.thin_count > thin_peak) thin_peak = q.thin_count;
      if (q.max_over > worstOver) worstOver = q.max_over;
      if (q.min_under > 0.0f && q.min_under < worstUnder) worstUnder = q.min_under;
      // rounds that measured no frontier tris leave min_angle at 0 — skip them
      if (q.tri_count > 0 && q.min_angle < worstAng) worstAng = q.min_angle;
      last = &q;
    }
    std::fprintf(stderr,
                 "[%s] iter=%-2d rounds=%-3d splits=%-5d collapses=%-5d "
                 "flips=%-5d thinPeak=%-3d worstAng=%5.1f over=%.2f under=%.2f "
                 "endCands=%d/%d\n",
                 tag, iter, rounds, s, c, fl, thin_peak,
                 worstAng < 1e29f ? worstAng * k : 0.0f, worstOver,
                 worstUnder < 1e29f ? worstUnder : 0.0f,
                 last ? last->split_cands : 0, last ? last->collapse_cands : 0);
  }
  OscillationReport r = detectOscillation(t);
  std::fprintf(stderr,
               "[%s] oscillated=%d healed=%d swings=%d peakThin=%d(r%d) "
               "worstAng=%.1f churnRun=%d(it%d)\n",
               tag, int(r.oscillated), int(r.healed), r.swings, r.peak_thin,
               r.peak_round, r.worst_min_angle * k, r.churn_run, r.churn_iter);
}

/* Dump the per-round trace (debug-app / test diagnostic). */
inline void printTrace(const DynTopoTrace &t, const char *tag)
{
  const float k = 180.0f / 3.14159265358979323846f;
  for (const RoundQuality &q : t.rounds) {
    std::fprintf(stderr,
                 "[%s] it%-2d r%-2d s=%-3d c=%-3d f=%-3d sm=%-3d  tris=%-4d "
                 "thin=%-3d minAng=%5.1f meanMin=%5.1f  sc=%-4d cc=%-4d "
                 "over=%5.2f under=%5.2f\n",
                 tag, q.iter, q.round, q.splits, q.collapses, q.flips, q.smooths,
                 q.tri_count, q.thin_count, q.min_angle * k, q.mean_min_angle * k,
                 q.split_cands, q.collapse_cands, q.max_over, q.min_under);
  }
}

} // namespace sculptcore::dyntopo
