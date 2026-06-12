// Tier 7 test: feature-tag hysteresis (7a).
//
//  - Tent fixture: a triangulated ridge whose per-edge dihedral is exact by
//    construction (diagonals chosen so each ridge edge's two wedge triangles
//    both key off one row's slope -> dihedral == 2*atan(slope)). A strong /
//    weak / strong slope pattern makes plain tagging leave a gap in the
//    crease; hysteresis must flood the weak segment closed.
//  - All-weak tent: hysteresis without a strong seed must tag nothing
//    (hysteresis is connectivity-gated, not just a lower threshold).
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"
#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "remesh/field/feature_tag.h"

#include <cmath>
#include <cstdio>

test_init;

// The shared test_assert macro has a known retval=0-on-failure bug; use a local
// one that flips retval (mirrors test_remesh_curvature.cc).
#define TASSERT(expr)                                                                     \
  do {                                                                                    \
    if (!(expr)) {                                                                        \
      retval = 1;                                                                         \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                   \
      fflush(stderr);                                                                     \
    }                                                                                     \
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

  // All-weak tent: no strong seed, so hysteresis must tag nothing (it is a
  // connectivity gate, not a lowered threshold).
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

} // namespace

int main()
{
  testHysteresis();
  return retval;
}
