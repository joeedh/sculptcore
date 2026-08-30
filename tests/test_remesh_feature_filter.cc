// Tier 7 test: feature-tag hysteresis (7a) + sharp-chain spur pruning (7b).
//
//  - Tent fixture: a triangulated ridge whose per-edge dihedral is exact by
//    construction (diagonals chosen so each ridge edge's two wedge triangles
//    both key off one row's slope -> dihedral == 2*atan(slope)). A strong /
//    weak / strong slope pattern makes plain tagging leave a gap in the
//    crease; hysteresis must flood the weak segment closed.
//  - All-weak tent: hysteresis without a strong seed must tag nothing
//    (hysteresis is connectivity-gated, not just a lower threshold).
//  - Spur pruning: per-row slope control turns single ridge edges / short
//    runs sharp with dangling or boundary-anchored ends; a triangulated cube
//    (all corners degree-3 junctions) must be a strict no-op.
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"
#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "remesh/field/cross_field.h"
#include "remesh/field/feature_tag.h"

#include <cmath>
#include <cstdio>

test_init;

// The shared test_assert macro has a known retval=0-on-failure bug; use a local
// one that flips retval (mirrors test_remesh_curvature.cc).
#define TASSERT(expr)                                                                    \
  do {                                                                                   \
    if (!(expr)) {                                                                       \
      retval = 1;                                                                        \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                  \
      fflush(stderr);                                                                    \
    }                                                                                    \
  } while (0)

using namespace sculptcore;
using namespace sculptcore::mesh;
using litestl::math::float3;
using litestl::util::Vector;

namespace {

constexpr float DEG = 3.14159265f / 180.0f;

// Triangulated tent: ridge along y at x=0, wings dropping as z = -s_j*|x|.
// Diagonals run (j,i)->(j+1,i+1) on the +x wing and (j,i+1)->(j+1,i) on the
// -x wing, so both triangles wedged on ridge edge j->j+1 have their third
// vertex on row j+1: the edge's dihedral is exactly 2*atan(slope[j+1]).
Mesh *makeTent(const float *slope, int ny, int nxHalf, float cell)
{
  Mesh *m = litestl::alloc::New<Mesh>("Tent");
  const int nx = 2 * nxHalf + 1;

  Vector<int> verts;
  verts.resize(ny * nx);
  for (int j = 0; j < ny; j++) {
    for (int i = 0; i < nx; i++) {
      float x = float(i - nxHalf) * cell;
      float3 co(x, float(j) * cell, -slope[j] * std::fabs(x));
      verts[j * nx + i] = m->make_vertex(co);
    }
  }

  Vector<int> vs;
  auto tri = [&](int a, int b, int c) {
    vs.clear();
    vs.append(verts[a]);
    vs.append(verts[b]);
    vs.append(verts[c]);
    m->make_face(vs);
  };
  for (int j = 0; j < ny - 1; j++) {
    for (int i = 0; i < nx - 1; i++) {
      int A = j * nx + i, B = j * nx + i + 1;
      int C = (j + 1) * nx + i + 1, D = (j + 1) * nx + i;
      if (i >= nxHalf) { // +x wing: diagonal A-C
        tri(A, B, C);
        tri(A, C, D);
      } else { // -x wing: diagonal B-D
        tri(A, B, D);
        tri(B, C, D);
      }
    }
  }

  m->recalc_normals();
  return m;
}

// Collect ridge edge ids (both endpoints at x==0), ordered by min y.
void ridgeEdges(Mesh &m, Vector<int> &out)
{
  out.clear();
  for (int e : m.e) {
    float3 a = m.v.co[m.e.vs[e][0]], b = m.v.co[m.e.vs[e][1]];
    if (std::fabs(a[0]) < 1e-6f && std::fabs(b[0]) < 1e-6f) {
      out.append(e);
    }
  }
  for (int i = 0; i < int(out.size()); i++) { // insertion sort by min y
    for (int k = i + 1; k < int(out.size()); k++) {
      auto miny = [&](int e) {
        return std::fmin(m.v.co[m.e.vs[e][0]][1], m.v.co[m.e.vs[e][1]][1]);
      };
      if (miny(out[k]) < miny(out[i])) {
        int t = out[i];
        out[i] = out[k];
        out[k] = t;
      }
    }
  }
}

int countSharp(Mesh &m)
{
  BuiltinAttr<bool, ".remesh.e.is_sharp"> is_sharp;
  is_sharp.ensure(m.e.attrs);
  int n = 0;
  for (int e : m.e) {
    n += is_sharp[e] ? 1 : 0;
  }
  return n;
}

void testHysteresis()
{
  const float SHARP = 45.0f * DEG, HYST = 15.0f * DEG;
  const float sStrong = std::tan(27.5f * DEG); // dihedral 55 deg (> 45)
  const float sWeak = std::tan(17.5f * DEG);   // dihedral 35 deg (in (30, 45))

  // Rows 1-3 strong, 4-6 weak, 7-9 strong (row 0's slope feeds no ridge edge).
  const int NY = 10;
  float slope[NY];
  for (int j = 0; j < NY; j++) {
    slope[j] = (j >= 4 && j <= 6) ? sWeak : sStrong;
  }

  Mesh *tent = makeTent(slope, NY, 3, 0.1f);
  Vector<int> ridge;
  ridgeEdges(*tent, ridge);
  TASSERT(int(ridge.size()) == NY - 1);

  BuiltinAttr<bool, ".remesh.e.is_sharp"> is_sharp;
  is_sharp.ensure(tent->e.attrs);

  // Plain tagging: only the 6 strong ridge edges (gap at rows 4-6).
  remesh::computeFeatureTags(*tent, SHARP);
  int nPlain = countSharp(*tent);
  int nRidgePlain = 0;
  for (int i = 0; i < int(ridge.size()); i++) {
    bool want = !(i >= 3 && i <= 5); // edge i has dihedral of row i+1
    TASSERT(is_sharp[ridge[i]] == want);
    nRidgePlain += is_sharp[ridge[i]] ? 1 : 0;
  }
  fprintf(stderr, "[hyst/plain] sharp=%d ridge_sharp=%d\n", nPlain, nRidgePlain);
  TASSERT(nRidgePlain == 6);
  TASSERT(nPlain == 6); // nothing tagged off-ridge

  // Hysteresis: the weak segment is strong-connected along the ridge -> whole
  // crease tagged; still nothing off-ridge.
  remesh::computeFeatureTags(*tent, SHARP, HYST);
  int nHyst = countSharp(*tent);
  int nRidgeHyst = 0;
  for (int i = 0; i < int(ridge.size()); i++) {
    TASSERT(is_sharp[ridge[i]]);
    nRidgeHyst += is_sharp[ridge[i]] ? 1 : 0;
  }
  fprintf(stderr, "[hyst/on] sharp=%d ridge_sharp=%d\n", nHyst, nRidgeHyst);
  TASSERT(nRidgeHyst == NY - 1);
  TASSERT(nHyst == NY - 1);

  litestl::alloc::Delete<Mesh>(tent);

  // An all-weak tent has no strong seed, so hysteresis must tag nothing.
  // Hysteresis gates on connectivity, not on a lowered threshold.
  float weakSlope[NY];
  for (int j = 0; j < NY; j++) {
    weakSlope[j] = sWeak;
  }
  Mesh *weakTent = makeTent(weakSlope, NY, 3, 0.1f);
  remesh::computeFeatureTags(*weakTent, SHARP, HYST);
  int nWeak = countSharp(*weakTent);
  fprintf(stderr, "[hyst/all-weak] sharp=%d\n", nWeak);
  TASSERT(nWeak == 0);
  litestl::alloc::Delete<Mesh>(weakTent);
}

// Cube with triangulated faces: 12 true crease edges, every corner a
// degree-3 junction, face diagonals flat (coplanar halves).
Mesh *makeCube()
{
  Mesh *m = litestl::alloc::New<Mesh>("Cube");
  static const float C[8][3] = {{0, 0, 0},
                                {1, 0, 0},
                                {1, 1, 0},
                                {0, 1, 0},
                                {0, 0, 1},
                                {1, 0, 1},
                                {1, 1, 1},
                                {0, 1, 1}};
  int v[8];
  for (int i = 0; i < 8; i++) {
    v[i] = m->make_vertex(float3(C[i][0], C[i][1], C[i][2]));
  }
  static const int Q[6][4] = {
      {0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
  Vector<int> vs;
  auto tri = [&](int a, int b, int c) {
    vs.clear();
    vs.append(v[a]);
    vs.append(v[b]);
    vs.append(v[c]);
    m->make_face(vs);
  };
  for (int q = 0; q < 6; q++) {
    tri(Q[q][0], Q[q][1], Q[q][2]);
    tri(Q[q][0], Q[q][2], Q[q][3]);
  }
  m->recalc_normals();
  return m;
}

void testSpurPruning()
{
  const float SHARP = 45.0f * DEG, HYST = 15.0f * DEG;
  const float sStrong = std::tan(27.5f * DEG);
  const float sWeak = std::tan(17.5f * DEG);
  const int NY = 10;
  // nxHalf=1: a slope change between rows kinks the wing surface along that
  // row's horizontal edges, and the kink grows with |x| (~49deg at the third
  // column). One column per wing keeps every wing dihedral ~10deg, so only
  // ridge edges can tag sharp and chain expectations stay exact.
  const int NXH = 1;
  float slope[NY];

  // Single interior sharp edge (both ends dangling): kept with pruning off,
  // dropped at min_chain=2.
  for (int j = 0; j < NY; j++) {
    slope[j] = sWeak;
  }
  slope[5] = sStrong; // ridge edge 4 only
  Mesh *spur = makeTent(slope, NY, NXH, 0.1f);
  remesh::computeFeatureTags(*spur, SHARP, 0.0f, 0);
  int n = countSharp(*spur);
  fprintf(stderr, "[prune/spur off] sharp=%d\n", n);
  TASSERT(n == 1);
  remesh::computeFeatureTags(*spur, SHARP, 0.0f, 2);
  n = countSharp(*spur);
  fprintf(stderr, "[prune/spur on] sharp=%d\n", n);
  TASSERT(n == 0);
  litestl::alloc::Delete<Mesh>(spur);

  // 4-edge dangling chain (ridge edges 2..5): survives min_chain=4, dropped
  // at min_chain=5.
  for (int j = 0; j < NY; j++) {
    slope[j] = (j >= 3 && j <= 6) ? sStrong : sWeak;
  }
  Mesh *chain4 = makeTent(slope, NY, NXH, 0.1f);
  remesh::computeFeatureTags(*chain4, SHARP, 0.0f, 4);
  n = countSharp(*chain4);
  fprintf(stderr, "[prune/chain4 min4] sharp=%d\n", n);
  TASSERT(n == 4);
  remesh::computeFeatureTags(*chain4, SHARP, 0.0f, 5);
  n = countSharp(*chain4);
  fprintf(stderr, "[prune/chain4 min5] sharp=%d\n", n);
  TASSERT(n == 0);
  litestl::alloc::Delete<Mesh>(chain4);

  // One end boundary-anchored, other dangling: still a spur, dropped.
  for (int j = 0; j < NY; j++) {
    slope[j] = sWeak;
  }
  slope[NY - 1] = sStrong; // ridge edge 8: row 9 = boundary row, row 8 interior
  Mesh *half = makeTent(slope, NY, NXH, 0.1f);
  remesh::computeFeatureTags(*half, SHARP, 0.0f, 2);
  n = countSharp(*half);
  fprintf(stderr, "[prune/half-anchor] sharp=%d\n", n);
  TASSERT(n == 0);
  litestl::alloc::Delete<Mesh>(half);

  // Full ridge anchored at both boundary rows: kept at any min_chain.
  for (int j = 0; j < NY; j++) {
    slope[j] = sStrong;
  }
  Mesh *ridge = makeTent(slope, NY, NXH, 0.1f);
  remesh::computeFeatureTags(*ridge, SHARP, 0.0f, 20);
  n = countSharp(*ridge);
  fprintf(stderr, "[prune/anchored] sharp=%d\n", n);
  TASSERT(n == NY - 1);
  litestl::alloc::Delete<Mesh>(ridge);

  // Cube no-op: all 12 crease edges junction-anchored, diagonals untagged,
  // even with hysteresis + aggressive pruning.
  Mesh *cube = makeCube();
  remesh::computeFeatureTags(*cube, SHARP, 0.0f, 0);
  n = countSharp(*cube);
  fprintf(stderr, "[prune/cube plain] sharp=%d\n", n);
  TASSERT(n == 12);
  remesh::computeFeatureTags(*cube, SHARP, HYST, 10);
  n = countSharp(*cube);
  fprintf(stderr, "[prune/cube filtered] sharp=%d\n", n);
  TASSERT(n == 12);
  litestl::alloc::Delete<Mesh>(cube);
}

// Flat n x n vert grid, every quad split along the (j,i)->(j+1,i+1) diagonal,
// with the two verts of the quad (jA,iA) diagonal lifted by h. That diagonal's
// dihedral is acos((1-2h^2)/(1+2h^2)) (60 deg at h=sqrt(1/6)); every edge
// around the lift stays under ~31 deg, so the tagger sees one isolated sharp
// edge at 45 deg to the axis-aligned rim - a worst-case noise spur.
Mesh *makeSpurGrid(int n, int jA, int iA, float h)
{
  Mesh *m = litestl::alloc::New<Mesh>("SpurGrid");
  Vector<int> verts;
  verts.resize(n * n);
  for (int j = 0; j < n; j++) {
    for (int i = 0; i < n; i++) {
      bool lifted = (j == jA && i == iA) || (j == jA + 1 && i == iA + 1);
      verts[j * n + i] = m->make_vertex(float3(float(i), float(j), lifted ? h : 0.0f));
    }
  }
  Vector<int> vs;
  auto tri = [&](int a, int b, int c) {
    vs.clear();
    vs.append(verts[a]);
    vs.append(verts[b]);
    vs.append(verts[c]);
    m->make_face(vs);
  };
  for (int j = 0; j < n - 1; j++) {
    for (int i = 0; i < n - 1; i++) {
      int A = j * n + i, B = j * n + i + 1;
      int C = (j + 1) * n + i + 1, D = (j + 1) * n + i;
      tri(A, B, C);
      tri(A, C, D);
    }
  }
  m->recalc_normals();
  return m;
}

// E2E gate assert: the unfiltered noise spur hard-pins two faces 45 deg off
// the rim-pinned field and forces singularities; pruning it (min_chain=2)
// leaves a uniform field with none.
void testSpurSingularities()
{
  const float SHARP = 45.0f * DEG;
  const float H = std::sqrt(1.0f / 6.0f); // lifted-diagonal dihedral = 60 deg
  Mesh *grid = makeSpurGrid(13, 6, 6, H);

  remesh::computeFeatureTags(*grid, SHARP);
  int n = countSharp(*grid);
  fprintf(stderr, "[sing/tags] sharp=%d\n", n);
  TASSERT(n == 1); // fixture self-check: exactly the isolated spur

  remesh::CrossFieldParams cp;
  cp.use_curvature = false; // no flat-face gate: hard pins apply everywhere
  cp.use_sharp_features = true;
  cp.feature_min_chain = 0;
  remesh::CrossFieldStats s0 = remesh::computeCrossField(*grid, cp);
  cp.feature_min_chain = 2;
  remesh::CrossFieldStats s1 = remesh::computeCrossField(*grid, cp);
  fprintf(stderr,
          "[sing/field] unfiltered=%d filtered=%d\n",
          s0.num_singularities,
          s1.num_singularities);
  TASSERT(s0.num_singularities > 0);
  TASSERT(s1.num_singularities == 0);
  litestl::alloc::Delete<Mesh>(grid);
}

} // namespace

int main()
{
  testHysteresis();
  testSpurPruning();
  testSpurSingularities();
  return retval;
}
