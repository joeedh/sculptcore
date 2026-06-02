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

static int runDab(float grade, int &leftover, int &maxVal, int &fAfter)
{
  Mesh *m = makeTriGrid(41); /* spacing 0.025 */
  const float3 center(0, 0, 0);
  const float radius = 0.15f;

  dyntopo::DynTopoParams p;
  p.l_max = 0.012f;
  p.l_min = 0.004f;
  p.grade = grade;
  p.mode = dyntopo::DynTopoMode::Subdivide;

  dyntopo::DynTopoStats st = dyntopo::applyBrushDab(*m, center, radius, p, /*seed=*/123u);
  measure(m, center, radius, p, leftover, maxVal);
  fAfter = m->f.count;

  alloc::Delete(m);
  return st.splits;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  int lo0, mv0, fa0;
  int splits0 = runDab(0.0f, lo0, mv0, fa0);
  int lo2, mv2, fa2;
  int splits2 = runDab(2.0f, lo2, mv2, fa2);

  printf("[cascade] grade0: splits=%d leftover=%d maxVal=%d faces=%d\n", splits0, lo0,
         mv0, fa0);
  printf("[cascade] grade2: splits=%d leftover=%d maxVal=%d faces=%d\n", splits2, lo2,
         mv2, fa2);

  /* (a) The graded (recommended) config fully converges to its target. */
  test_assert(lo2 == 0);

  /* (b) Graded split count + max valence stay bounded — deterministic given the
   * seed, with ~1.5x headroom so a real cascade/valence regression trips it
   * while incidental drift does not. (Measured: splits=308, maxVal=18.) */
  test_assert(splits2 > 0 && splits2 < 500);
  test_assert(mv2 < 24);

  /* (c) Grading is doing its job: uniform refinement here genuinely cascades
   * (doesn't even converge in max_rounds — leftover>0, valence ~57), and
   * grading cuts the split count many-fold and removes the high-valence hubs.
   * If grading silently no-ops, these trip. */
  test_assert(lo0 > 0);
  test_assert(splits2 * 4 < splits0);
  test_assert(mv2 < mv0);
  test_assert(mv0 < 80);

  printf("dyntopo_cascade test: ok\n");
  return test_end();
}
