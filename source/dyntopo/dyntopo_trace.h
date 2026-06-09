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
  int round = 0;
  int splits = 0, collapses = 0, flips = 0, smooths = 0;
  int tri_count = 0;          // frontier triangles measured this round
  int thin_count = 0;         // tris with min interior angle < thin_angle
  float min_angle = 0.0f;     // worst per-tri min interior angle this round
  float mean_min_angle = 0.0f; // mean of the per-tri min angles
};

struct DynTopoTrace {
  /* A triangle whose smallest interior angle is below this (radians) is "thin".
   * 15 degrees by default — a long thin sliver sits well under it. */
  float thin_angle = 0.2617994f;
  litestl::util::Vector<RoundQuality> rounds;
  void clear() { rounds.clear(); }
};

/* Verdict over a completed trace. `swings` counts thin-triangle bursts that
 * appeared and then healed (a rise then fall in thin_count, each leg at least
 * `min_swing` triangles); `oscillated` is swings >= 1. `peak_round`/`peak_thin`
 * mark the worst burst and `worst_min_angle` the global worst angle; `healed` is
 * end thin_count <= the first round's (the region recovered by the end). */
struct OscillationReport {
  bool oscillated = false;
  bool healed = false;
  int swings = 0;
  int peak_round = -1;
  int peak_thin = 0;
  int start_thin = 0;
  int end_thin = 0;
  float worst_min_angle = 0.0f;
};

/* Hysteresis turning-point counter over the thin_count series: a confirmed rise
 * of >= min_swing then a confirmed fall of >= min_swing is one healed burst. */
inline OscillationReport detectOscillation(const DynTopoTrace &t, int min_swing = 1)
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
  return r;
}

/* Dump the per-round trace (debug-app / test diagnostic). */
inline void printTrace(const DynTopoTrace &t, const char *tag)
{
  const float k = 180.0f / 3.14159265358979323846f;
  for (const RoundQuality &q : t.rounds) {
    std::fprintf(stderr,
                 "[%s] r%-2d s=%-3d c=%-3d f=%-3d sm=%-3d  tris=%-4d thin=%-3d "
                 "minAng=%5.1f meanMin=%5.1f\n",
                 tag, q.round, q.splits, q.collapses, q.flips, q.smooths,
                 q.tri_count, q.thin_count, q.min_angle * k, q.mean_min_angle * k);
  }
}

} // namespace sculptcore::dyntopo
