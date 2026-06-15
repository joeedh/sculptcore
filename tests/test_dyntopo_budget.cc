/* M7.2 safety valve: the per-dab split budget. A budgeted dab must apply at most
 * max_splits and flag budget_hit; the still-out-of-band edges are finished by
 * subsequent dabs (a moving brush re-touches the region), so a heavy refine is
 * spread across frames with bounded per-dab work and the region still fully
 * converges. The same refine with no budget converges in a single dab. */
#include "test_util.h"

#include "dyntopo/dyntopo.h"
#include "mesh/mesh.h"

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
  Mesh *m = alloc::New<Mesh>("test_dyntopo_budget grid");
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

static dyntopo::DynTopoParams params(int budget)
{
  dyntopo::DynTopoParams p;
  p.l_max = 0.012f;
  p.l_min = 0.004f;
  p.grade = 2.0f;
  p.do_flips = true;
  p.mode = dyntopo::DynTopoMode::Subdivide;
  p.max_splits = budget;
  return p;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  const float3 center(0, 0, 0);
  const float radius = 0.15f;
  const uint32_t seed = 123u;

  /* Reference: one unbudgeted dab refines the region fully. */
  Mesh *ref = makeTriGrid(41);
  dyntopo::DynTopoStats r = dyntopo::runDyntopoRemesh(*ref, center, radius, params(0), seed);
  test_assert(!r.budget_hit);
  test_assert(r.splits > 0);
  int refFaces = ref->f.count;
  alloc::Delete(ref);

  /* Budgeted: the same refine, capped at a small budget per dab. */
  const int budget = 50;
  Mesh *m = makeTriGrid(41);
  int dabs = 0, total = 0, maxInDab = 0;
  bool firstHit = false, converged = false;
  for (int i = 0; i < 1000; i++) {
    dyntopo::DynTopoStats st =
        dyntopo::runDyntopoRemesh(*m, center, radius, params(budget), seed);
    dabs++;
    test_assert(st.splits <= budget); /* never overshoot the budget */
    if (st.splits > maxInDab) {
      maxInDab = st.splits;
    }
    if (i == 0) {
      firstHit = st.budget_hit;
    }
    total += st.splits;
    if (st.splits == 0 && !st.budget_hit) {
      converged = true; /* nothing left out of band */
      break;
    }
  }

  printf("[budget] ref: splits=%d faces=%d | budgeted(%d): dabs=%d total=%d "
         "maxInDab=%d faces=%d\n",
         r.splits, refFaces, budget, dabs, total, maxInDab, m->f.count);

  test_assert(firstHit);          /* the heavy first dab hit the cap */
  test_assert(maxInDab <= budget);
  test_assert(dabs > 1);          /* work was genuinely deferred across dabs */
  test_assert(converged);         /* the region still fully refines */
  /* The budget only spreads the work — the converged mesh matches the one-shot
   * refine closely (ordering differs across dabs, so allow a small margin). */
  test_assert(m->f.count >= refFaces * 9 / 10);

  alloc::Delete(m);

  printf("dyntopo_budget test: ok\n");
  return test_end();
}
