/* M7.5 CI gate: a deterministic regression guard on the dyntopo cascade. A
 * seeded dab on a fixed grid must (a) fully converge to its (graded) target,
 * (b) keep the split count under a ceiling — the cascade ratio can't silently
 * blow up — and (c) keep the max in-region valence bounded (no high-valence
 * hub explosion). The graded knob (M7.1a) is what holds (b) and (c) down; this
 * test fails loudly if a future change regresses either. Split counts are
 * deterministic given the seed, so the ceilings are stable, not perf-timed. */
#include "test_util.h"

#include "dyntopo/dyntopo.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace litestl;
using namespace litestl::math;

static Mesh *makeTriGrid(int n)
{
  Mesh *m = alloc::New<Mesh>("test_dyntopo_cascade grid");
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

static int valence(Mesh *m, int v)
{
  if (m->v.e[v] == ELEM_NONE) {
    return 0;
  }
  int n = 0;
  for (int e : EdgeOfVertIter(m, v, m->v.e[v])) {
    (void)e;
    if (++n > 100000) {
      break;
    }
  }
  return n;
}

/* Edges still outside the (graded) target band, max valence, both over the dab
 * region. leftover==0 proves the dab converged; maxVal bounds the hubs. */
static void measure(Mesh *m, float3 center, float radius, const dyntopo::DynTopoParams &p,
                    int &leftover, int &maxVal)
{
  const float r2 = radius * radius;
  leftover = 0;
  maxVal = 0;
  for (int e : m->e) {
    if (m->e.c[e] == ELEM_NONE) {
      continue;
    }
    float3 mid = (m->v.co[m->e.vs[e][0]] + m->v.co[m->e.vs[e][1]]) * 0.5f;
    float d2 = (mid - center).lengthSqr();
    if (d2 > r2) {
      continue;
    }
    float scale = (p.grade > 0.0f && radius > 0.0f)
                      ? 1.0f + p.grade * (std::sqrt(d2) / radius)
                      : 1.0f;
    float L = (m->v.co[m->e.vs[e][0]] - m->v.co[m->e.vs[e][1]]).length();
    if (L > p.l_max * scale * 1.001f) {
      leftover++;
    }
  }
  for (int v : m->v) {
    if ((m->v.co[v] - center).lengthSqr() > r2) {
      continue;
    }
    int val = valence(m, v);
    if (val > maxVal) {
      maxVal = val;
    }
  }
}

static int runDab(float grade, bool flips, int &leftover, int &maxVal, int &flipCount)
{
  Mesh *m = makeTriGrid(41); /* spacing 0.025 */
  const float3 center(0, 0, 0);
  const float radius = 0.15f;

  dyntopo::DynTopoParams p;
  p.l_max = 0.012f; /* ~0.48x base spacing: deep enough to expose the cascade */
  p.l_min = 0.004f;
  p.grade = grade;
  p.do_flips = flips;
  p.mode = dyntopo::DynTopoMode::Subdivide;

  dyntopo::DynTopoStats st = dyntopo::applyBrushDab(*m, center, radius, p, /*seed=*/123u);
  measure(m, center, radius, p, leftover, maxVal);
  flipCount = st.flips;

  alloc::Delete(m);
  return st.splits;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  /* Recommended config (grade + geometric flips), grade-only, and the bare
   * baseline (neither). The dab target is ~0.5x the base spacing, deep enough
   * that the spoke cascade runs away without the flip sweep. */
  int loR, mvR, flR;
  int splitsR = runDab(2.0f, true, loR, mvR, flR);
  int loG, mvG, flG;
  int splitsG = runDab(2.0f, false, loG, mvG, flG);
  int loB, mvB, flB;
  int splitsB = runDab(0.0f, false, loB, mvB, flB);

  printf("[cascade] grade+flips: splits=%d flips=%d leftover=%d maxVal=%d\n", splitsR,
         flR, loR, mvR);
  printf("[cascade] grade only : splits=%d flips=%d leftover=%d maxVal=%d\n", splitsG,
         flG, loG, mvG);
  printf("[cascade] baseline   : splits=%d flips=%d leftover=%d maxVal=%d\n", splitsB,
         flB, loB, mvB);

  /* (a) The recommended config fully converges and stays well-shaped: it should
   * reach the target (leftover 0) with near-regular valence — the cascade is
   * gone. Deterministic given the seed; ~1.5x headroom on the ceilings. */
  test_assert(loR == 0);
  test_assert(flR > 0);          /* flips actually fired */
  test_assert(splitsR > 0 && splitsR < 1000);
  test_assert(mvR < 14);         /* near-regular: no high-valence hubs */

  /* (b) The flip sweep is load-bearing: without it (grade only) the cascade is
   * markedly worse — more splits and a much higher max valence. If flips
   * silently no-op, these trip. */
  test_assert(splitsR < splitsG);
  test_assert(mvR < mvG);

  /* (c) The bare baseline (no grade, no flips) cascades hardest — its valence
   * blows up far past the flip-tamed result. */
  test_assert(mvB > 2 * mvR);

  printf("dyntopo_cascade test: ok\n");
  return test_end();
}
