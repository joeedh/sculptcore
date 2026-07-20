/* UV-chart boundary preservation in dyntopo (P11 E5). A grid carrying two UV
 * charts split along its middle row derives EDGE_UVCHART on that row; a heavy
 * refining dab with preserve_features=true must keep the chart boundary a
 * single connected simple path (splits refine it, nothing tears it), and the
 * incremental recompute after the dab must agree with a from-scratch
 * (markAllDirty) re-derivation — i.e. the derived flags stay in sync with the
 * actual UV discontinuity through split/collapse interpolation. */
#include "test_util.h"

#include "dyntopo/dyntopo.h"
#include "mesh/attribute.h"
#include "mesh/boundary.h"
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
namespace bnd = sculptcore::mesh::boundary;

static Mesh *makeTriGrid(int n, util::Vector<int> &grid)
{
  Mesh *m = alloc::New<Mesh>("test_dyntopo_uvchart grid");
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

/* Two charts: uv = vertex xy, +5 in u for faces above the middle row. */
static void assignChartUVs(Mesh *m)
{
  AttrRef &ref = m->c.attrs.ensure(AttrType::FLOAT2, "uv", /*materialize=*/true);
  ref.use = AttrUse::UV;
  auto *uv = ref.get_data<float2>();
  for (int c : m->c) {
    const int f = m->l.f[m->c.l[c]];
    // Face centroid y decides the chart (no face straddles the middle row).
    float cy = 0.0f;
    int cnt = 0;
    int l = m->f.l[f];
    int c0 = m->l.c[l], cc = c0;
    do {
      cy += m->v.co[m->c.v[cc]][1];
      cnt++;
      cc = m->c.next[cc];
    } while (cc != c0);
    cy /= float(cnt);
    const float3 co = m->v.co[m->c.v[c]];
    uv->materialize(c);
    (*uv)[c] = float2(co[0] + (cy > 0.0f ? 5.0f : 0.0f), co[1]);
  }
}

static int chartDegree(Mesh *m, int v, BoolAttrView *flag)
{
  if (m->v.e[v] == ELEM_NONE) return 0;
  int n = 0;
  for (int e : EdgeOfVertIter(m, v, m->v.e[v])) {
    if (flag->get(e)) n++;
  }
  return n;
}

static int walkChain(Mesh *m, int start, BoolAttrView *flag)
{
  int prev = ELEM_NONE, cur = start;
  for (int guard = 0; guard < 1000000; guard++) {
    int next = ELEM_NONE, deg = 0;
    for (int e : EdgeOfVertIter(m, cur, m->v.e[cur])) {
      if (!flag->get(e)) continue;
      deg++;
      int o = (m->e.vs[e][0] == cur) ? m->e.vs[e][1] : m->e.vs[e][0];
      if (o != prev) next = o;
    }
    if (deg > 2) return ELEM_NONE; /* branch: torn */
    if (next == ELEM_NONE) return cur;
    prev = cur;
    cur = next;
  }
  return ELEM_NONE;
}

/* Snapshot the flagged live-edge set as sorted (vmin,vmax) pairs. */
static void flagSet(Mesh *m, BoolAttrView *flag, util::Vector<int64_t> &out)
{
  out.clear();
  for (int e : m->e) {
    if (flag && flag->get(e)) {
      int a = m->e.vs[e][0], b = m->e.vs[e][1];
      if (a > b) std::swap(a, b);
      out.append((int64_t(a) << 32) | int64_t(b));
    }
  }
  std::sort(out.begin(), out.end());
}

static void runTest()
{
  const int n = 41;
  util::Vector<int> grid;
  Mesh *m = makeTriGrid(n, grid);
  assignChartUVs(m);

  const int midY = n / 2;
  const int leftEnd = grid[midY * n + 0];
  const int rightEnd = grid[midY * n + (n - 1)];

  bnd::markAllDirty(m);
  bnd::recomputeDirty(m);

  // The derived chart boundary must be exactly the middle row.
  BoolAttrView *uvchart = bnd::findBoolEdgeView(m, bnd::EDGE_UVCHART);
  test_assert(uvchart != nullptr);
  int pre = 0;
  for (int e : m->e) {
    if (uvchart->get(e)) pre++;
  }
  test_assert(pre == n - 1);
  test_assert((bnd::vertClass(m, grid[midY * n + 5]) & bnd::BC_UVCHART) != 0);

  dyntopo::DynTopoParams p;
  p.l_max = 0.012f;
  p.l_min = 0.004f;
  p.grade = 2.0f;
  p.do_flips = true;
  p.do_smooth = true;
  p.preserve_features = true;
  p.mode = dyntopo::DynTopoMode::Both;

  dyntopo::DynTopoStats st = dyntopo::runDyntopoRemesh(*m, float3(0, 0, 0), 0.2f, p, 7u);
  bnd::recomputeDirty(m); /* the executors' end-of-stroke incremental fold */

  printf("[uvchart] splits=%d collapses=%d flips=%d\n", st.splits, st.collapses,
         st.flips);
  test_assert(st.splits > 0);
  test_assert(!m->v.freemap[leftEnd] && !m->v.freemap[rightEnd]);

  // The chart boundary survives as one connected simple chain.
  uvchart = bnd::findBoolEdgeView(m, bnd::EDGE_UVCHART);
  test_assert(uvchart != nullptr);
  bool simple = true;
  int ends = 0;
  for (int v : m->v) {
    int d = chartDegree(m, v, uvchart);
    if (d == 0) continue;
    if (d == 1) ends++;
    else if (d != 2) simple = false;
  }
  printf("[uvchart] ends=%d simple=%d\n", ends, int(simple));
  test_assert(simple && ends == 2);
  test_assert(walkChain(m, leftEnd, uvchart) == rightEnd);

  // Incremental recompute == from-scratch re-derivation: the propagated /
  // interpolated flags match the actual UV discontinuity everywhere.
  util::Vector<int64_t> incremental, full;
  flagSet(m, uvchart, incremental);
  bnd::markAllDirty(m);
  bnd::recomputeDirty(m);
  flagSet(m, bnd::findBoolEdgeView(m, bnd::EDGE_UVCHART), full);
  printf("[uvchart] incremental=%d full=%d\n", int(incremental.size()),
         int(full.size()));
  test_assert(incremental.size() == full.size());
  for (int i = 0; i < int(incremental.size()); i++) {
    test_assert(incremental[i] == full[i]);
  }

  alloc::Delete(m);
  printf("dyntopo_uvchart test: ok\n");
}

/* Max |uv - co.xy| over all corners: 0 on a flat grid whose uv = vertex xy iff
 * every topological/smoothing op kept UVs anchored to positions. */
static float uvDrift(Mesh *m)
{
  AttrRef ref = m->c.attrs.find_attribute(AttrType::FLOAT2, "uv");
  auto *uv = ref.get_data<float2>();
  float worst = 0.0f;
  for (int c : m->c) {
    const float3 co = m->v.co[m->c.v[c]];
    const float2 d = uv->safe_get(c) - float2(co[0], co[1]);
    worst = std::max(worst, d.lengthSqr());
  }
  return std::sqrt(worst);
}

/* Dyntopo tangential smooth with reproject_uvs: the slid verts' UVs must stay
 * anchored (uv == xy on the flat single-chart grid); without it they drift. */
static float runSmoothDab(bool reproject)
{
  const int n = 41;
  util::Vector<int> grid;
  Mesh *m = makeTriGrid(n, grid);
  /* Single chart: uv = vertex xy. */
  AttrRef &ref = m->c.attrs.ensure(AttrType::FLOAT2, "uv", /*materialize=*/true);
  ref.use = AttrUse::UV;
  auto *uv = ref.get_data<float2>();
  for (int c : m->c) {
    const float3 co = m->v.co[m->c.v[c]];
    uv->materialize(c);
    (*uv)[c] = float2(co[0], co[1]);
  }

  dyntopo::DynTopoParams p;
  p.l_max = 0.012f;
  p.l_min = 0.004f;
  p.grade = 2.0f;
  p.do_flips = true;
  p.do_smooth = true;
  p.smooth_lambda = 0.8f;
  p.reproject_uvs = reproject;
  p.mode = dyntopo::DynTopoMode::Both;
  dyntopo::DynTopoStats st = dyntopo::runDyntopoRemesh(*m, float3(0, 0, 0), 0.2f, p, 7u);
  test_assert(st.smooths > 0);

  const float drift = uvDrift(m);
  alloc::Delete(m);
  return drift;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  runTest();

  const float driftOn = runSmoothDab(true);
  const float driftOff = runSmoothDab(false);
  printf("[uvchart] smooth uv drift: reproject=on %g, off %g\n", double(driftOn),
         double(driftOff));
  test_assert(driftOff > 1e-4f); /* smoothing really slides UVs when off */
  test_assert(driftOn < driftOff * 0.05f);

  return test_end();
}
