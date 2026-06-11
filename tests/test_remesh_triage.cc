// Tier-1 input-triage test (plans/quad-remeshing-filtering.md). Drives
// remesh::triageMesh on hand-built meshes whose triage outcome is known:
//
//  - Clean grid: NO-OP — nothing welds/drops, and the vert coords are
//    byte-identical before/after (the Tier-1 no-op-on-good-input contract).
//  - Welded doubles: a 2-triangle quad authored with 6 independent verts (two
//    coincident pairs) welds to 4 verts / 2 faces.
//  - Degenerate face: a colinear "triangle" beside a good one is dropped (and
//    its now-faceless edges cleaned as wire), leaving the good face.
//  - Tiny component: a 3-vert island beside a grid is dropped when its share of
//    the verts is below min_component_frac; the grid survives.
//  - Non-manifold: three triangles fanning one edge are DETECTED (radial 3) but
//    NOT repaired — the face count is unchanged.
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/mesh_shapes.h"
#include "mesh/utils/mesh_validate.h"
#include "mesh/utils/triangulate.h"

#include "remesh/triage.h"

#include <cstdio>

test_init;

#define TASSERT(expr)                                                          \
  do {                                                                         \
    if (!(expr)) {                                                             \
      retval = 1;                                                              \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);        \
      fflush(stderr);                                                          \
    }                                                                          \
  } while (0)

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace sculptcore::remesh;
using litestl::math::float3;
using litestl::util::Vector;

namespace {

int tri(Mesh *m, int a, int b, int c)
{
  Vector<int> vs;
  vs.append(a);
  vs.append(b);
  vs.append(c);
  return m->make_face(vs);
}

// Clean input → triage must change nothing. Snapshot every vert's coord and
// confirm it (and the vert/face counts) are byte-identical afterward.
void testNoOpOnClean()
{
  Mesh *g = makeGrid(5, 5, 1.0f); // 25 verts, 16 quads, all manifold
  int v0 = g->v.count, f0 = g->f.count;
  Vector<float3> before;
  for (int v : g->v)
    before.append(g->v.co[v]);

  TriageParams p; // defaults: weld_rel 1e-5, min_component_frac 0
  TriageReport r;
  triageMesh(*g, p, r);

  TASSERT(r.ran);
  TASSERT(r.welded_verts == 0);
  TASSERT(r.removed_degenerate_faces == 0);
  TASSERT(r.removed_duplicate_faces == 0);
  TASSERT(r.removed_wire_edges == 0);
  TASSERT(r.removed_components == 0);
  TASSERT(r.non_manifold_edges == 0);
  TASSERT(r.non_manifold_verts == 0);
  TASSERT(g->v.count == v0);
  TASSERT(g->f.count == f0);

  int i = 0;
  bool identical = true;
  for (int v : g->v) {
    float3 co = g->v.co[v];
    if (i >= int(before.size()) || co[0] != before[i][0] ||
        co[1] != before[i][1] || co[2] != before[i][2])
      identical = false;
    i++;
  }
  TASSERT(identical);
  fprintf(stderr, "[no-op] verts=%d faces=%d identical=%d\n", g->v.count,
          g->f.count, int(identical));
  litestl::alloc::Delete<Mesh>(g);
}

// A quad split into two triangles, authored with 6 independent verts so the two
// shared corners are coincident duplicates. Weld merges the 2 dups → 4 verts.
void testWeldDoubles()
{
  Mesh *m = litestl::alloc::New<Mesh>("test weld");
  int a0 = m->make_vertex(float3(0, 0, 0));
  int b0 = m->make_vertex(float3(1, 0, 0));
  int c0 = m->make_vertex(float3(0, 1, 0));
  int a1 = m->make_vertex(float3(1, 0, 0)); // coincident with b0
  int b1 = m->make_vertex(float3(1, 1, 0));
  int c1 = m->make_vertex(float3(0, 1, 0)); // coincident with c0
  tri(m, a0, b0, c0);
  tri(m, a1, b1, c1);
  m->recalc_normals();
  TASSERT(m->v.count == 6);
  TASSERT(m->f.count == 2);

  TriageParams p;
  TriageReport r;
  triageMesh(*m, p, r);

  fprintf(stderr, "[weld] welded=%d verts=%d faces=%d dupF=%d\n",
          r.welded_verts, m->v.count, m->f.count, r.removed_duplicate_faces);
  TASSERT(r.welded_verts == 2);
  TASSERT(m->v.count == 4);
  TASSERT(m->f.count == 2);
  TASSERT(r.removed_duplicate_faces == 0);
  TASSERT(r.removed_degenerate_faces == 0);
  litestl::alloc::Delete<Mesh>(m);
}

// A good triangle plus a colinear (zero-area) "triangle": the degenerate face is
// dropped and its faceless edges cleaned, leaving exactly one face.
void testDegenerateFace()
{
  Mesh *m = litestl::alloc::New<Mesh>("test degen");
  int g0 = m->make_vertex(float3(0, 0, 0));
  int g1 = m->make_vertex(float3(1, 0, 0));
  int g2 = m->make_vertex(float3(0, 1, 0));
  int d0 = m->make_vertex(float3(0, 0, 5));
  int d1 = m->make_vertex(float3(1, 0, 5));
  int d2 = m->make_vertex(float3(2, 0, 5)); // colinear with d0,d1
  tri(m, g0, g1, g2);
  tri(m, d0, d1, d2);
  m->recalc_normals();
  TASSERT(m->f.count == 2);

  TriageParams p;
  TriageReport r;
  triageMesh(*m, p, r);

  fprintf(stderr, "[degen] degen=%d wire=%d faces=%d\n",
          r.removed_degenerate_faces, r.removed_wire_edges, m->f.count);
  TASSERT(r.removed_degenerate_faces == 1);
  TASSERT(m->f.count == 1);
  TASSERT(r.welded_verts == 0);
  litestl::alloc::Delete<Mesh>(m);
}

// A 5x5 grid plus a far-away 3-vert island. With min_component_frac high enough
// the island (3 of 28 verts) is dropped; the grid survives.
void testTinyComponent()
{
  Mesh *m = makeGrid(5, 5, 1.0f); // 25 verts, one component
  int gridVerts = m->v.count, gridFaces = m->f.count;
  int t0 = m->make_vertex(float3(100, 0, 0));
  int t1 = m->make_vertex(float3(101, 0, 0));
  int t2 = m->make_vertex(float3(100, 1, 0));
  tri(m, t0, t1, t2);
  m->recalc_normals();

  TriageParams p;
  p.min_component_frac = 0.2f; // thresh = floor(0.2*28) = 5 > 3
  TriageReport r;
  triageMesh(*m, p, r);

  fprintf(stderr, "[tiny-comp] comps=%d compVerts=%d verts=%d faces=%d\n",
          r.removed_components, r.removed_component_verts, m->v.count,
          m->f.count);
  TASSERT(r.removed_components == 1);
  TASSERT(r.removed_component_verts == 3);
  TASSERT(m->v.count == gridVerts);
  TASSERT(m->f.count == gridFaces);
  litestl::alloc::Delete<Mesh>(m);
}

// Three triangles sharing edge (s0,s1): a non-manifold edge (radial 3). Triage
// detects it (count > 0) but does not repair — all three faces remain.
void testNonManifoldDetect()
{
  Mesh *m = litestl::alloc::New<Mesh>("test nonmanifold");
  int s0 = m->make_vertex(float3(0, 0, 0));
  int s1 = m->make_vertex(float3(1, 0, 0));
  int w0 = m->make_vertex(float3(0, 1, 0));
  int w1 = m->make_vertex(float3(0, -1, 0));
  int w2 = m->make_vertex(float3(0, 0, 1));
  tri(m, s0, s1, w0);
  tri(m, s0, s1, w1);
  tri(m, s0, s1, w2);
  m->recalc_normals();

  TriageParams p;
  TriageReport r;
  triageMesh(*m, p, r);

  fprintf(stderr, "[nonmanifold] nmEdges=%d faces=%d degen=%d\n",
          r.non_manifold_edges, m->f.count, r.removed_degenerate_faces);
  TASSERT(r.non_manifold_edges >= 1);
  TASSERT(m->f.count == 3); // detect-only: nothing removed
  TASSERT(r.removed_degenerate_faces == 0);
  litestl::alloc::Delete<Mesh>(m);
}

// Polygon-soup stress: rebuild a triangulated sphere with every face authored
// from its own 3 verts, so weld merges ~2/3 of all verts and the rebuild path
// (kill-all faces/verts + make_face survivors) runs at scale.
void testWeldStressSoup()
{
  Mesh *src = makeUVSphere(96, 96, 1.0f);
  (void)triangulateMesh(*src);
  int srcV = src->v.count, srcF = src->f.count;

  Mesh *m = litestl::alloc::New<Mesh>("test soup");
  for (int f : src->f) {
    Vector<int> vs;
    int c0 = src->l.c[src->f.l[f]], cc = c0;
    do {
      vs.append(m->make_vertex(src->v.co[src->c.v[cc]]));
      cc = src->c.next[cc];
    } while (cc != c0);
    m->make_face(vs);
  }
  m->recalc_normals();
  litestl::alloc::Delete<Mesh>(src);
  int soupV = m->v.count;
  fprintf(stderr, "[soup] authored verts=%d faces=%d (target %d verts)\n",
          m->v.count, m->f.count, srcV);

  TriageParams p;
  TriageReport r;
  triageMesh(*m, p, r);

  fprintf(stderr, "[soup] welded=%d verts=%d faces=%d degen=%d dup=%d\n",
          r.welded_verts, m->v.count, m->f.count, r.removed_degenerate_faces,
          r.removed_duplicate_faces);
  TASSERT(r.welded_verts == soupV - srcV);
  TASSERT(m->v.count == srcV);
  TASSERT(m->f.count == srcF);
  RemeshReport health = remeshValidate(*m);
  TASSERT(health.manifold);
  TASSERT(health.non_manifold_edges == 0);
  litestl::alloc::Delete<Mesh>(m);
}

// Tier 6.3 input hole policy: a triangulated plane with one tiny hole (1 cell,
// rim 4) and one large hole (3x3 cells, rim 12), outer boundary 32. With
// max_frac between them (thresh 0.15*48 = 7.2) the tiny hole is filled by a
// centroid fan; the large hole and the outer boundary are preserved.
void testHoleFill()
{
  Mesh *m = makeGrid(9, 9, 8.0f); // unit cells over [-4,4]^2
  (void)triangulateMesh(*m);

  // Punch the holes: kill faces whose centroid lands in either box, then clean
  // up like triage would (orphan interior verts, wire edges).
  auto inBox = [&](const float3 &p, float x0, float y0, float x1, float y1) {
    return p[0] > x0 && p[0] < x1 && p[1] > y0 && p[1] < y1;
  };
  Vector<int> kill;
  for (int f : m->f) {
    float3 cen(0, 0, 0);
    int c0 = m->l.c[m->f.l[f]], cc = c0, n = 0;
    do {
      cen += m->v.co[m->c.v[cc]];
      n++;
      cc = m->c.next[cc];
    } while (cc != c0);
    cen *= 1.0f / float(n);
    if (inBox(cen, -3, -3, -2, -2) || inBox(cen, 0, 0, 3, 3))
      kill.append(f);
  }
  TASSERT(int(kill.size()) == 2 + 18);
  for (int f : kill)
    m->kill_face(f);
  Vector<int> orphans;
  for (int v : m->v) {
    if (inBox(m->v.co[v], 0.5f, 0.5f, 2.5f, 2.5f))
      orphans.append(v);
  }
  TASSERT(int(orphans.size()) == 4);
  for (int v : orphans)
    m->kill_vertex(v);
  Vector<int> wire;
  for (int e : m->e) {
    if (m->e.c[e] == ELEM_NONE)
      wire.append(e);
  }
  for (int e : wire)
    m->kill_edge(e);
  m->recalc_normals();

  auto boundaryEdges = [&]() {
    int n = 0;
    for (int e : m->e) {
      int c = m->e.c[e];
      if (c != ELEM_NONE && m->c.radial_next[c] == c)
        n++;
    }
    return n;
  };
  TASSERT(boundaryEdges() == 4 + 12 + 32);
  int v0 = m->v.count, f0 = m->f.count;

  TriageReport r;
  fillInputHoles(*m, 0.15f, r);

  fprintf(stderr, "[hole-fill] filled=%d kept=%d fillFaces=%d bnd=%d\n",
          r.input_holes_filled, r.input_holes_kept, r.input_hole_fill_faces,
          boundaryEdges());
  TASSERT(r.input_holes_filled == 1);
  TASSERT(r.input_holes_kept == 2);
  TASSERT(r.input_hole_fill_faces == 4); // rim 4 -> centroid fan of 4 tris
  TASSERT(m->v.count == v0 + 1);
  TASSERT(m->f.count == f0 + 4);
  TASSERT(boundaryEdges() == 12 + 32); // large hole + outer border preserved

  RemeshReport health = remeshValidate(*m);
  TASSERT(health.manifold);
  TASSERT(health.consistent_winding);
  TASSERT(health.boundary_loop_count == 2);

  // Idempotent: nothing left under the threshold.
  TriageReport r2;
  fillInputHoles(*m, 0.15f, r2);
  TASSERT(r2.input_holes_filled == 0);
  TASSERT(r2.input_holes_kept == 2);
  litestl::alloc::Delete<Mesh>(m);
}

} // namespace

int main()
{
  testNoOpOnClean();
  testWeldDoubles();
  testDegenerateFace();
  testTinyComponent();
  testNonManifoldDetect();
  testWeldStressSoup();
  testHoleFill();
  return retval;
}
