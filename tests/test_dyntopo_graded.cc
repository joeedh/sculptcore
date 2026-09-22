#include "test_util.h"

#include "dyntopo/dyntopo.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace sculptcore::dyntopo;
using namespace litestl;
using namespace litestl::math;
using namespace litestl::util;

/** Every face a triangle, every edge referencing live verts. The full topology
 * audit lives in test_dyntopo.cc; this is the cheap sanity check that the
 * graded walk did not hand the operators something they could not chew. */
static bool validateTris(Mesh &m, const char *tag)
{
  for (int ei : m.e) {
    int v1 = m.e.vs[ei][0], v2 = m.e.vs[ei][1];
    if (v1 == v2 || m.v.freemap[v1] || m.v.freemap[v2]) {
      fprintf(stderr, "[%s] edge %d degenerate or dangling\n", tag, ei);
      return false;
    }
  }
  for (int fi : m.f) {
    if (m.f.list_count[fi] != 1 || m.l.size[m.f.l[fi]] != 3) {
      fprintf(stderr, "[%s] face %d is not a triangle\n", tag, fi);
      return false;
    }
  }
  return true;
}

// One large triangle. Every edge midpoint sits far outside a small dab, which
// is the case Sphere cannot see and GradedRecursive can.
static Mesh *makeBigTri()
{
  Mesh *m = alloc::New<Mesh>("test_dyntopo_graded big tri");
  int a = m->make_vertex(float3(-2.0f, -2.0f, 0.0f));
  int b = m->make_vertex(float3(3.0f, -2.0f, 0.0f));
  int c = m->make_vertex(float3(-2.0f, 3.0f, 0.0f));
  int tri[3] = {a, b, c};
  m->make_face(std::span<int>(tri, 3));
  return m;
}

/** Triangulated NxN grid in z=0, spanning [-0.5, 0.5]^2 (test_dyntopo.cc's). */
static Mesh *makeTriGrid(int n)
{
  Mesh *m = alloc::New<Mesh>("test_dyntopo_graded grid");
  Vector<int> grid;
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

/** Furthest any edge shorter than `pristine` sits from center, measured at its
 * midpoint. Every edge of a fresh grid is at least `pristine` long, so this is
 * how far out the dab actually reached. -1 when it reached nowhere. */
static float touchedReach(Mesh &m, float3 center, float pristine)
{
  float worst = -1.0f;
  for (int e : m.e) {
    if ((m.v.co[m.e.vs[e][0]] - m.v.co[m.e.vs[e][1]]).length() >= pristine) {
      continue;
    }
    float3 mid = (m.v.co[m.e.vs[e][0]] + m.v.co[m.e.vs[e][1]]) * 0.5f;
    float d = (mid - center).length();
    worst = d > worst ? d : worst;
  }
  return worst;
}

// Mean edge length over the edges whose midpoint falls in [lo, hi) from center.
// Returns -1 when the shell holds no edges.
static float shellMeanLen(Mesh &m, float3 center, float lo, float hi, int *countOut)
{
  double sum = 0.0;
  int n = 0;
  for (int e : m.e) {
    float3 mid = (m.v.co[m.e.vs[e][0]] + m.v.co[m.e.vs[e][1]]) * 0.5f;
    float d = (mid - center).length();
    if (d >= lo && d < hi) {
      sum += (m.v.co[m.e.vs[e][0]] - m.v.co[m.e.vs[e][1]]).length();
      n++;
    }
  }
  if (countOut) {
    *countOut = n;
  }
  return n > 0 ? float(sum / n) : -1.0f;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  // Sphere stays the default; nothing shipped changes behaviour by this commit.
  {
    DynTopoParams defaults;
    test_assert(defaults.region == DynTopoRegion::Sphere);
  }

  /* --- A triangle far larger than the brush. Sphere tests edge midpoints, all
   *     of which lie outside a small dab, so it finds nothing to do however
   *     coarse the triangle is. GradedRecursive tests the face instead, so the
   *     dab lands inside it and the walk grades outward from its edges. --- */
  {
    const float3 center(-1.0f / 3.0f, -1.0f / 3.0f, 0.0f); // the triangle's centroid
    const float radius = 0.2f;

    DynTopoParams p;
    p.l_max = 0.1f;
    p.l_min = 0.0f; // collapse never triggers
    p.mode = DynTopoMode::Subdivide;

    Mesh *ms = makeBigTri();
    DynTopoStats sphere = runDyntopoRemesh(*ms, center, radius, p, 1234u);

    p.region = DynTopoRegion::GradedRecursive;
    Mesh *mg = makeBigTri();
    DynTopoStats graded = runDyntopoRemesh(*mg, center, radius, p, 1234u);

    printf("big tri: sphere splits=%d verts=%d | graded splits=%d verts=%d hops=%d\n",
           sphere.splits,
           ms->v.count,
           graded.splits,
           mg->v.count,
           graded.graded_hops);

    test_assert(sphere.splits == 0); // the gap this mode exists to close
    test_assert(ms->v.count == 3);
    test_assert(graded.splits > 0);
    test_assert(mg->v.count > 3);
    test_assert(validateTris(*mg, "bigtri-graded"));

    alloc::Delete<Mesh>(ms);
    alloc::Delete<Mesh>(mg);
  }

  /* --- Density gradation. On a coarse grid both modes refine under the dab,
   *     but only GradedRecursive touches anything beyond the rim, what it
   *     leaves there gets coarser with distance, and the walk still stops. --- */
  {
    const float3 center(0, 0, 0);
    const float radius = 0.1f;

    DynTopoParams p;
    p.l_max = 0.02f;
    p.l_min = 0.0f;
    p.mode = DynTopoMode::Subdivide;
    p.do_flips = false; // measure the candidate rule, not the flip sweep

    Mesh *ms = makeTriGrid(17); // spacing 0.0625, so one quad spans the dab
    runDyntopoRemesh(*ms, center, radius, p, 7u);

    p.region = DynTopoRegion::GradedRecursive;
    Mesh *mg = makeTriGrid(17);
    DynTopoStats graded = runDyntopoRemesh(*mg, center, radius, p, 7u);

    // Two shells outward from the rim, plus the reach of each mode. The base
    // spacing is 0.0625 and the goal grows 1.6x a hop from 0.02, so the walk
    // runs out of reach a few hops out, well inside the grid.
    int nNear = 0, nMid = 0, nsOut = 0;
    float sOut = shellMeanLen(*ms, center, radius, 0.2f, &nsOut);
    float gNear = shellMeanLen(*mg, center, radius, 0.2f, &nNear);
    float gMid = shellMeanLen(*mg, center, 0.2f, 0.32f, &nMid);
    float sReach = touchedReach(*ms, center, 0.06f);
    float gReach = touchedReach(*mg, center, 0.06f);

    printf("grid: sphere outside=%.4f (n=%d) reach=%.3f | graded near=%.4f (n=%d) "
           "mid=%.4f (n=%d) reach=%.3f hops=%d\n",
           sOut,
           nsOut,
           sReach,
           gNear,
           nNear,
           gMid,
           nMid,
           gReach,
           graded.graded_hops);

    test_assert(graded.graded_hops > 0); // the walk actually left the seed faces
    test_assert(nNear > 0 && nMid > 0);
    test_assert(gNear < sOut); // graded refined past the rim, sphere did not
    test_assert(gNear < gMid); // and the refinement thins out with distance
    // Sphere stops at the rim (allowing the half-edge its midpoint test buys).
    test_assert(sReach < radius + 0.04f);
    // Graded reaches further, but still terminates well inside the grid, whose
    // furthest edge midpoint sits at about 0.69.
    test_assert(gReach > sReach);
    test_assert(gReach < 0.55f);
    test_assert(validateTris(*mg, "grid-graded"));

    alloc::Delete<Mesh>(ms);
    alloc::Delete<Mesh>(mg);
  }

  /* --- Grading outward exists because refining only inside the rim fans the
   *     apex of whatever coarse triangle straddles it. So check that a graded
   *     dab leaves max valence no worse than a Sphere dab does. The relief knob
   *     is printed rather than asserted; it does not dominate this case, and
   *     which way it moves is what the A/B exists to answer. --- */
  {
    const float3 center(0, 0, 0);
    const float radius = 0.1f;

    DynTopoParams p;
    p.l_max = 0.02f;
    p.l_min = 0.0f;
    p.mode = DynTopoMode::Subdivide; // flips left on: the shipped configuration

    auto maxValence = [](Mesh &m) {
      int worst = 0;
      for (int v : m.v) {
        if (m.v.e[v] == ELEM_NONE) {
          continue;
        }
        int n = 0;
        for (int e : EdgeOfVertIter(&m, v, m.v.e[v])) {
          (void)e;
          n++;
        }
        worst = n > worst ? n : worst;
      }
      return worst;
    };

    Mesh *ms = makeTriGrid(17);
    runDyntopoRemesh(*ms, center, radius, p, 99u);

    p.region = DynTopoRegion::GradedRecursive;
    Mesh *mOn = makeTriGrid(17);
    runDyntopoRemesh(*mOn, center, radius, p, 99u);

    p.graded_valence_relief = 0;
    Mesh *mOff = makeTriGrid(17);
    runDyntopoRemesh(*mOff, center, radius, p, 99u);

    int vs = maxValence(*ms), vOn = maxValence(*mOn), vOff = maxValence(*mOff);
    printf("max valence: sphere=%d graded=%d graded-no-relief=%d\n", vs, vOn, vOff);
    test_assert(vOn <= vs);

    alloc::Delete<Mesh>(ms);
    alloc::Delete<Mesh>(mOn);
    alloc::Delete<Mesh>(mOff);
  }

  /* --- Determinism. The walk uses an unordered memo and a stack, so pin that
   *     the same seed still reproduces the same dab. --- */
  {
    const float3 center(0, 0, 0);
    const float radius = 0.2f;

    DynTopoParams p;
    p.l_max = 0.04f;
    p.l_min = 0.01f;
    p.region = DynTopoRegion::GradedRecursive;

    Mesh *a = makeTriGrid(9);
    Mesh *b = makeTriGrid(9);
    DynTopoStats sa = runDyntopoRemesh(*a, center, radius, p, 4242u);
    DynTopoStats sb = runDyntopoRemesh(*b, center, radius, p, 4242u);

    test_assert(sa.splits == sb.splits);
    test_assert(sa.collapses == sb.collapses);
    test_assert(sa.flips == sb.flips);
    test_assert(sa.rounds == sb.rounds);
    test_assert(sa.graded_hops == sb.graded_hops);
    test_assert(a->v.count == b->v.count);
    test_assert(a->e.count == b->e.count);
    test_assert(a->f.count == b->f.count);

    alloc::Delete<Mesh>(a);
    alloc::Delete<Mesh>(b);
  }

  /* --- Convergence: a second graded dab on an already-graded region is a
   *     no-op, so the outward walk does not chase its own tail. --- */
  {
    const float3 center(0, 0, 0);
    const float radius = 0.2f;

    DynTopoParams p;
    p.l_max = 0.04f;
    p.l_min = 0.0f;
    p.mode = DynTopoMode::Subdivide;
    p.region = DynTopoRegion::GradedRecursive;

    Mesh *m = makeTriGrid(9);
    DynTopoStats first = runDyntopoRemesh(*m, center, radius, p, 5u);
    int V1 = m->v.count;
    DynTopoStats second = runDyntopoRemesh(*m, center, radius, p, 5u);

    printf("convergence: first splits=%d capped=%d | second splits=%d\n",
           first.splits,
           int(first.capped),
           second.splits);

    test_assert(!first.capped);
    test_assert(second.splits == 0);
    test_assert(m->v.count == V1);

    alloc::Delete<Mesh>(m);
  }

  printf("dyntopo graded test: ok\n");
  return test_end();
}
