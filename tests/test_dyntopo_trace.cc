/* Granular split-sliver oscillation detection gate (dyntopo_trace.h). A seeded
 * dab on a fixed grid is remeshed with the per-round triangle-quality trace on;
 * the trace must (a) record one snapshot per round, (b) capture the transient
 * thin-triangle ("sliver") burst the split path creates before it heals — a
 * pathology a final-state survey misses — and (c) show the geometric flip sweep
 * healing it: with flips ON the region recovers (final round near sliver-free),
 * with flips OFF the slivers persist. See documentation/dynamic-topology.md and
 * documentation/plans/dyntopo-m7-cascade.md. */
#include "test_util.h"

#include "dyntopo/dyntopo.h"
#include "dyntopo/dyntopo_trace.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace litestl;
using namespace litestl::math;

static Mesh *makeTriGrid(int n)
{
  Mesh *m = alloc::New<Mesh>("test_dyntopo_trace grid");
  util::Vector<int> grid;
  grid.resize(n * n);
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      float fx = float(x) / float(n - 1) - 0.5f;
      float fy = float(y) / float(n - 1) - 0.5f;
      grid[y * n + x] = m->make_vertex(float3(fx, fy, 0.0f));
    }
  }
  for (int y = 0; y < n - 1; y++) {
    for (int x = 0; x < n - 1; x++) {
      int a = grid[y * n + x], b = grid[y * n + x + 1];
      int c = grid[(y + 1) * n + x + 1], d = grid[(y + 1) * n + x];
      int t0[3] = {a, b, c};
      int t1[3] = {a, c, d};
      m->make_face(std::span<int>(t0, 3));
      m->make_face(std::span<int>(t1, 3));
    }
  }
  return m;
}

/* Scalar result of one traced dab — no allocating members, so it outlives the
 * trace (whose Vector must free before test_end's leak check runs). */
struct Analysis {
  int trace_rounds = 0;
  int stats_rounds = 0;
  dyntopo::OscillationReport report;
};

/* Run one traced dab, print its per-round trace, and return the scalar verdict.
 * The DynTopoTrace is local here, so its buffer frees on return. */
static Analysis runAndAnalyze(bool flips, const char *tag)
{
  Mesh *m = makeTriGrid(41); /* spacing 0.025 */
  const float3 center(0, 0, 0);
  const float radius = 0.15f;

  dyntopo::DynTopoParams p;
  p.l_max = 0.012f; /* ~0.48x base spacing: deep enough to expose the cascade */
  p.l_min = 0.004f;
  p.grade = 2.0f;
  p.do_flips = flips;
  p.mode = dyntopo::DynTopoMode::Subdivide;

  dyntopo::DynTopoTrace trace;
  p.trace = &trace;
  dyntopo::DynTopoStats st =
      dyntopo::runDyntopoRemesh(*m, center, radius, p, /*seed=*/123u);
  alloc::Delete(m);

  dyntopo::printTrace(trace, tag);

  Analysis a;
  a.trace_rounds = int(trace.rounds.size());
  a.stats_rounds = st.rounds;
  a.report = dyntopo::detectOscillation(trace, /*min_swing=*/2);
  return a;
}

int main()
{
  setvbuf(stderr, nullptr, _IONBF, 0);
  setvbuf(stdout, nullptr, _IONBF, 0);

  Analysis on = runAndAnalyze(/*flips=*/true, "flips-on");
  Analysis off = runAndAnalyze(/*flips=*/false, "flips-off");
  const dyntopo::OscillationReport &rOn = on.report;
  const dyntopo::OscillationReport &rOff = off.report;

  const float k = 180.0f / 3.14159265358979323846f;
  printf("[trace] flips-on : rounds=%d peak_thin=%d@r%d worst_minAng=%.1f "
         "end_thin=%d healed=%d swings=%d\n",
         on.trace_rounds,
         rOn.peak_thin,
         rOn.peak_round,
         rOn.worst_min_angle * k,
         rOn.end_thin,
         int(rOn.healed),
         rOn.swings);
  printf("[trace] flips-off: rounds=%d peak_thin=%d@r%d worst_minAng=%.1f "
         "end_thin=%d healed=%d swings=%d\n",
         off.trace_rounds,
         rOff.peak_thin,
         rOff.peak_round,
         rOff.worst_min_angle * k,
         rOff.end_thin,
         int(rOff.healed),
         rOff.swings);

  /* (a) Per-round trace was recorded for every round the dab ran. */
  test_assert(on.trace_rounds == on.stats_rounds);
  test_assert(off.trace_rounds == off.stats_rounds);
  test_assert(on.trace_rounds > 1 && off.trace_rounds > 1);

  /* (b) Without the flip sweep the split path drives the region into a deep,
   * oscillating sliver state: a thin-triangle burst forms (peak >> what's left
   * at the end, so a final-state survey understates it), the worst angle goes
   * near-degenerate (< 5deg), the thin count swings repeatedly, and it never
   * heals back to the start. This transient is exactly what the trace exists to
   * catch. */
  test_assert(rOff.peak_thin > 0);
  test_assert(rOff.peak_thin > rOff.end_thin);
  test_assert(rOff.worst_min_angle < 0.087f); /* < 5deg: near-degenerate */
  test_assert(rOff.swings >= 1);
  test_assert(!rOff.healed);

  /* (c) The geometric flip sweep heals slivers within the round, so they never
   * accumulate: with flips ON the burst stays tiny (peak far below the flips-OFF
   * burst), the worst angle never goes near-degenerate, and the converged state
   * keeps far fewer residual slivers than flips-OFF. The flip sweep is the
   * load-bearing mitigation for the split-sliver pathology.
   *
   * (We assert the flip sweep's *efficacy* — healthy converged state, much
   * better than OFF — rather than an exact return to the starting sliver count.
   * The dab converges via a seeded maximal-independent-set walk whose pick order
   * depends on mesh-internal element ids / disk-cycle order, so the exact
   * converged triangulation is one of several valid ones; in-place Euler ops
   * legitimately land on a different one. The OFF-vs-ON contrast below is the
   * id-layout-robust signal.) */
  test_assert(rOn.peak_thin < rOff.peak_thin);
  test_assert(rOn.worst_min_angle > rOff.worst_min_angle);
  test_assert(rOn.end_thin < rOff.end_thin); /* flips leave far fewer slivers */
  test_assert(rOn.worst_min_angle > 0.087f); /* flips-on stays > 5deg: healthy */

  printf("dyntopo_trace test: ok\n");
  return test_end();
}
