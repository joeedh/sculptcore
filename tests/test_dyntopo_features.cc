/* Feature preservation in dyntopo (boundary-conditions integration). A seam line
 * marked straight across a grid must survive a heavy refining dab with
 * preserve_features=true: it stays a single connected simple path of seam edges
 * (split refines it, collinear collapse may coarsen it, but flips/collapses never
 * tear or branch it), and its endpoints are never collapsed away. The dab still
 * subdivides the surrounding region (features don't block ordinary remeshing). */
#include "test_util.h"

#include "dyntopo/dyntopo.h"
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
  Mesh *m = alloc::New<Mesh>("test_dyntopo_features grid");
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

static int seamDegree(Mesh *m, int v, BoolAttrView *seam)
{
  if (m->v.e[v] == ELEM_NONE)
    return 0;
  int n = 0;
  for (int e : EdgeOfVertIter(m, v, m->v.e[v])) {
    if (seam->get(e))
      n++;
  }
  return n;
}

/* Walk the seam-edge chain from `start`; return the far endpoint reached (or
 * ELEM_NONE if it branches / dead-ends before a degree-1 vert). */
static int walkSeam(Mesh *m, int start, BoolAttrView *seam)
{
  int prev = ELEM_NONE, cur = start;
  for (int guard = 0; guard < 1000000; guard++) {
    int next = ELEM_NONE, deg = 0;
    for (int e : EdgeOfVertIter(m, cur, m->v.e[cur])) {
      if (!seam->get(e))
        continue;
      deg++;
      int o = (m->e.vs[e][0] == cur) ? m->e.vs[e][1] : m->e.vs[e][0];
      if (o != prev)
        next = o;
    }
    if (deg > 2)
      return ELEM_NONE; /* branch: torn */
    if (next == ELEM_NONE)
      return cur; /* reached an endpoint */
    prev = cur;
    cur = next;
  }
  return ELEM_NONE;
}

struct Result {
  int splits, collapses, flips;
  int seamCount;
  bool connected;
  bool simple;
  bool endsAlive;
};

static Result runDab(bool preserve)
{
  const int n = 41;
  util::Vector<int> grid;
  Mesh *m = makeTriGrid(n, grid);

  const int midY = n / 2;
  int leftEnd = grid[midY * n + 0];
  int rightEnd = grid[midY * n + (n - 1)];

  // Mark the whole middle row of edges as a seam.
  for (int x = 0; x < n - 1; x++) {
    int e = m->find_edge(grid[midY * n + x], grid[midY * n + x + 1]);
    test_assert(e != ELEM_NONE);
    bnd::setEdgeFlag(m, bnd::EDGE_SEAM, e, true);
  }
  bnd::recomputeDirty(m);

  dyntopo::DynTopoParams p;
  p.l_max = 0.012f;
  p.l_min = 0.004f;
  p.grade = 2.0f;
  p.do_flips = true;
  p.do_smooth = true;
  p.preserve_features = preserve;
  p.mode = dyntopo::DynTopoMode::Both;

  dyntopo::DynTopoStats st = dyntopo::runDyntopoRemesh(*m, float3(0, 0, 0), 0.2f, p, 7u);
  bnd::recomputeDirty(m); /* fold in the dab's boundary marks */

  BoolAttrView *seam = bnd::findBoolEdgeView(m, bnd::EDGE_SEAM);

  Result r;
  r.splits = st.splits;
  r.collapses = st.collapses;
  r.flips = st.flips;
  r.endsAlive = !m->v.freemap[leftEnd] && !m->v.freemap[rightEnd];

  int seamCount = 0;
  for (int e : m->e) {
    if (seam && seam->get(e))
      seamCount++;
  }
  r.seamCount = seamCount;

  // Simple-path check: exactly two seam verts of degree 1 (the ends), the rest
  // degree 2, none higher.
  bool simple = true;
  int ends = 0;
  if (seam) {
    for (int v : m->v) {
      int d = seamDegree(m, v, seam);
      if (d == 0)
        continue;
      if (d == 1)
        ends++;
      else if (d != 2)
        simple = false;
    }
  }
  r.simple = simple && ends == 2;

  r.connected = r.endsAlive && seam && walkSeam(m, leftEnd, seam) == rightEnd;

  alloc::Delete(m);
  return r;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  Result on = runDab(true);
  Result off = runDab(false);

  printf("[features] preserve=on : splits=%d collapses=%d flips=%d seam=%d "
         "simple=%d connected=%d endsAlive=%d\n",
         on.splits,
         on.collapses,
         on.flips,
         on.seamCount,
         on.simple,
         on.connected,
         on.endsAlive);
  printf("[features] preserve=off: splits=%d collapses=%d flips=%d seam=%d "
         "simple=%d connected=%d endsAlive=%d\n",
         off.splits,
         off.collapses,
         off.flips,
         off.seamCount,
         off.simple,
         off.connected,
         off.endsAlive);

  // With preservation: the dab still refines, and the seam survives intact — a
  // single connected simple path whose endpoints are never collapsed.
  test_assert(on.splits > 0);
  test_assert(on.endsAlive);
  test_assert(on.simple);
  test_assert(on.connected);

  // Differential: without preservation the flip sweep + collapses tear or branch
  // the seam, so it is no longer a clean connected simple path. (If this ever
  // passes trivially, preservation isn't actually doing anything.)
  test_assert(!(off.simple && off.connected));

  printf("dyntopo_features test: ok\n");
  return test_end();
}
