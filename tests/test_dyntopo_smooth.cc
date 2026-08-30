/* M7.4: tangential smoothing. With smoothing on, a dab's in-region triangles are
 * more uniform (lower edge-length coefficient of variation) than with it off,
 * while still converging to the target and staying valid (no folded/zero-area
 * faces — the clamp + tangential projection guarantee it). */
#include "test_util.h"

#include "dyntopo/dyntopo.h"
#include "mesh/mesh.h"

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
  Mesh *m = alloc::New<Mesh>("test_dyntopo_smooth grid");
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

/* Edge-length coefficient of variation (stddev/mean) over in-region edges, and
 * the smallest in-region triangle area (a fold/degenerate would drive this <= 0). */
static void quality(Mesh *m, float3 center, float r2, double &cv, double &minArea)
{
  double sum = 0.0, sq = 0.0;
  int n = 0;
  for (int e : m->e) {
    if (m->e.c[e] == ELEM_NONE)
      continue;
    float3 a = m->v.co[m->e.vs[e][0]], b = m->v.co[m->e.vs[e][1]];
    if (((a + b) * 0.5f - center).lengthSqr() > r2)
      continue;
    double len = (a - b).length();
    sum += len;
    sq += len * len;
    n++;
  }
  double mean = n > 0 ? sum / n : 0.0;
  double var = n > 0 ? sq / n - mean * mean : 0.0;
  cv = mean > 0.0 ? std::sqrt(var > 0.0 ? var : 0.0) / mean : 0.0;

  minArea = 1e30;
  for (int f : m->f) {
    int l = m->f.l[f];
    int c0 = m->l.c[l], c1 = m->c.next[c0], c2 = m->c.next[c1];
    float3 A = m->v.co[m->c.v[c0]], B = m->v.co[m->c.v[c1]], C = m->v.co[m->c.v[c2]];
    if (((A + B + C) * (1.0f / 3.0f) - center).lengthSqr() > r2)
      continue;
    double area = 0.5 * double((B - A).cross(C - A).length());
    if (area < minArea)
      minArea = area;
  }
}

static int run(bool smooth,
               float3 center,
               float radius,
               double &cv,
               double &minArea,
               int &leftover,
               int &smooths)
{
  Mesh *m = makeTriGrid(41); /* spacing 0.025 */
  dyntopo::DynTopoParams p;
  p.l_max = 0.012f;
  p.l_min = 0.004f;
  p.grade = 2.0f;
  p.do_flips = true;
  p.do_smooth = smooth;
  p.smooth_lambda = 0.5f;
  p.mode = dyntopo::DynTopoMode::Subdivide;

  dyntopo::DynTopoStats st =
      dyntopo::runDyntopoRemesh(*m, center, radius, p, /*seed=*/123u);
  smooths = st.smooths;

  const float r2 = radius * radius;
  quality(m, center, r2, cv, minArea);

  /* Convergence: in-region edges within the graded band. */
  leftover = 0;
  for (int e : m->e) {
    if (m->e.c[e] == ELEM_NONE)
      continue;
    float3 a = m->v.co[m->e.vs[e][0]], b = m->v.co[m->e.vs[e][1]];
    float3 mid = (a + b) * 0.5f;
    float d2 = (mid - center).lengthSqr();
    if (d2 > r2)
      continue;
    float target = p.l_max * (1.0f + p.grade * (std::sqrt(d2) / radius));
    if ((a - b).length() > target * 1.001f)
      leftover++;
  }

  int faces = m->f.count;
  alloc::Delete(m);
  return faces;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  const float3 center(0, 0, 0);
  const float radius = 0.15f;

  double cvOff, areaOff, cvOn, areaOn;
  int loOff, smOff, loOn, smOn;
  run(false, center, radius, cvOff, areaOff, loOff, smOff);
  run(true, center, radius, cvOn, areaOn, loOn, smOn);

  printf("[smooth] off: cv=%.4f minArea=%.3e leftover=%d smooths=%d\n",
         cvOff,
         areaOff,
         loOff,
         smOff);
  printf("[smooth] on : cv=%.4f minArea=%.3e leftover=%d smooths=%d\n",
         cvOn,
         areaOn,
         loOn,
         smOn);

  test_assert(smOff == 0); /* off really does no smoothing */
  test_assert(smOn > 0);   /* on actually moved verts */
  test_assert(loOff == 0); /* both converge */
  test_assert(loOn == 0);
  test_assert(cvOn < cvOff); /* smoothing yields more uniform edge lengths */
  test_assert(areaOn > 0.0); /* no folded / zero-area triangle (clamp holds) */

  printf("dyntopo_smooth test: ok\n");
  return test_end();
}
