/* Uniform Catmull-Clark refiner + cached stencil tables (displacementAndSubSurf
 * plan, S1 gate). Hand-checked fixtures: cube (smooth rules), creased cube
 * (EDGE_SHARP crease rules + propagation), an open triangle fan (boundary
 * rules, non-quad faces), and a lone pentagon (n-gon -> quad split). Verifies
 * per-level counts, grid tables (Ptex-style per-cage-corner grids), and the
 * core contract: evaluating cached stencil tables on a perturbed cage is
 * bit-identical to directly re-running recursive subdivision on it. */
#include "test_util.h"

#include "mesh/attribute_bool.h"
#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/mesh_shapes.h"
#include "subdiv/subdiv.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>
#include <cstring>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace litestl;
using namespace litestl::math;
using litestl::util::Vector;

static bool near3(const float3 &a, float x, float y, float z, float eps = 1e-6f)
{
  return std::fabs(a[0] - x) <= eps && std::fabs(a[1] - y) <= eps &&
         std::fabs(a[2] - z) <= eps;
}

static int findVert(Mesh &m, float x, float y, float z)
{
  for (int vi : m.v) {
    if (near3(m.v.co[vi], x, y, z, 1e-7f)) {
      return vi;
    }
  }
  return ELEM_NONE;
}

static int countSharpEdges(Mesh &m)
{
  BoolAttrView *sharp = boundary::findBoolEdgeView(&m, boundary::EDGE_SHARP);
  if (!sharp) {
    return 0;
  }
  int n = 0;
  for (int ei : m.e) {
    if ((*sharp)[ei]) {
      n++;
    }
  }
  return n;
}

static int faceVertCount(Mesh &m, int fi)
{
  int c0 = m.l.c[m.f.l[fi]], cc = c0, n = 0;
  do {
    n++;
    cc = m.c.next[cc];
  } while (cc != c0);
  return n;
}

static bool sameBits(const Vector<float3> &a, const Vector<float3> &b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (int i = 0; i < int(a.size()); i++) {
    if (std::memcmp(&a[i], &b[i], sizeof(float3)) != 0) {
      return false;
    }
  }
  return true;
}

/* Fixture builders (deterministic — the bit-exact gate builds each twice). */

static Mesh *buildCube()
{
  return createCube(2, 1.0f);
}

static Mesh *buildCreasedCube()
{
  Mesh *m = createCube(2, 1.0f);
  /* Crease the top ring: the 4 edges between z == +0.5 verts. */
  float ring[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
  for (int i = 0; i < 4; i++) {
    int a = findVert(*m, ring[i][0], ring[i][1], 0.5f);
    int b = findVert(*m, ring[(i + 1) % 4][0], ring[(i + 1) % 4][1], 0.5f);
    test_assert(a != ELEM_NONE && b != ELEM_NONE);
    int e = m->find_edge(a, b);
    test_assert(e != ELEM_NONE);
    boundary::setEdgeFlag(m, boundary::EDGE_SHARP, e, true);
  }
  return m;
}

static Mesh *buildFan()
{
  Mesh *m = alloc::New<Mesh>("subdiv fan");
  float co[6][3] = {{0, 0, 0},           {1, 0, 0},  {0.75f, 0.75f, 0},
                    {0, 1, 0},           {-0.75f, 0.75f, 0}, {-1, 0, 0}};
  int ids[6];
  for (int i = 0; i < 6; i++) {
    ids[i] = m->make_vertex(float3(co[i][0], co[i][1], co[i][2]));
  }
  for (int i = 0; i < 4; i++) {
    int tri[3] = {ids[0], ids[i + 1], ids[i + 2]};
    m->make_face(std::span<int>(tri, 3));
  }
  return m;
}

static Mesh *buildPentagon()
{
  Mesh *m = alloc::New<Mesh>("subdiv pentagon");
  float co[5][3] = {
      {1, 0, 0}, {0.25f, 1, 0}, {-1, 0.5f, 0}, {-1, -0.5f, 0}, {0.25f, -1, 0}};
  int ids[5];
  for (int i = 0; i < 5; i++) {
    ids[i] = m->make_vertex(float3(co[i][0], co[i][1], co[i][2]));
  }
  m->make_face(std::span<int>(ids, 5));
  return m;
}

/* Grid-table invariants: corner/edge/face-point chains land where the layout
 * doc says, neighbor grids share their boundary verts, and grids cover every
 * vert and every face of each level exactly (faces exactly once). */
static void checkGrids(subdiv::Refiner &r, Mesh &cage)
{
  int nLevels = int(r.levels.size());

  for (int L = 0; L < nLevels; L++) {
    /* Follow a level-1 vert through vertPointOf up to level L. */
    auto chainVert = [&](int v1) {
      for (int k = 1; k <= L; k++) {
        v1 = r.levels[k].vertPointOf[v1];
      }
      return v1;
    };
    subdiv::SubdivLevel &lvl = r.levels[L];
    int S = lvl.gridSide, W = S + 1;
    test_assert(S == (1 << L));
    test_assert(int(lvl.gridVerts.size()) == r.gridCount() * W * W);
    test_assert(int(lvl.gridFaces.size()) == r.gridCount() * S * S);
    test_assert(r.gridCount() * S * S == lvl.mesh->f.count);

    Vector<bool> vertSeen, faceSeen;
    vertSeen.resize(lvl.vertCount);
    faceSeen.resize(lvl.mesh->f.count);
    for (int i = 0; i < lvl.vertCount; i++) {
      vertSeen[i] = false;
    }
    for (int i = 0; i < lvl.mesh->f.count; i++) {
      faceSeen[i] = false;
    }

    for (int gi = 0; gi < r.gridCount() * W * W; gi++) {
      int v1 = lvl.gridVerts[gi];
      test_assert(v1 >= 0 && v1 < lvl.vertCount);
      vertSeen[v1] = true;
    }
    for (int gi = 0; gi < r.gridCount() * S * S; gi++) {
      int f1 = lvl.gridFaces[gi];
      test_assert(f1 >= 0 && f1 < lvl.mesh->f.count);
      test_assert(!faceSeen[f1]); /* each level face is exactly one grid cell */
      faceSeen[f1] = true;
    }
    int missedV = 0;
    for (int i = 0; i < lvl.vertCount; i++) {
      missedV += vertSeen[i] ? 0 : 1;
    }
    test_assert(missedV == 0);

    /* Corner anchors + neighbor sharing, per cage face corner. */
    int g = 0;
    for (int fi : cage.f) {
      int c0 = cage.l.c[cage.f.l[fi]], cc = c0;
      int nCorners = faceVertCount(cage, fi);
      int cornerIdx = 0;
      do {
        const int *gv = &lvl.gridVerts[g * W * W];
        test_assert(gv[0] == chainVert(r.levels[0].vertPointOf[cage.c.v[cc]]));
        test_assert(gv[S * W + S] == chainVert(r.levels[0].facePointOf[fi]));
        test_assert(gv[S] == chainVert(r.levels[0].edgePointOf[cage.c.e[cc]]));
        int cp = cage.c.prev[cc];
        test_assert(gv[S * W] == chainVert(r.levels[0].edgePointOf[cage.c.e[cp]]));

        int gNext = g - cornerIdx + (cornerIdx + 1) % nCorners;
        const int *gvNext = &lvl.gridVerts[gNext * W * W];
        test_assert(gv[S] == gvNext[S * W]); /* grid(c) (S,0) == grid(next c) (0,S) */

        g++;
        cornerIdx++;
        cc = cage.c.next[cc];
      } while (cc != c0);
    }
  }
}

/* The S1 gate: cached stencil tables from one refine reproduce a direct
 * recursive re-subdivision of a (perturbed) cage bit-exactly. `build` must be
 * deterministic — it is called twice to get two identical cages. */
static void checkBitExact(Mesh *(*build)(), int nLevels, const char *tag)
{
  Mesh *cage1 = build();
  subdiv::Refiner ra;
  ra.refine(*cage1, nLevels);

  /* Cached chain vs the refiner's own per-level output on the same cage. */
  Vector<float3> cageCo, evaled, direct;
  subdiv::gatherVertCo(*cage1, cageCo);
  for (int L = 0; L < nLevels; L++) {
    ra.evalFromCage(cageCo, L + 1, evaled);
    subdiv::gatherVertCo(*ra.levels[L].mesh, direct);
    direct.resize(ra.levels[L].vertCount); /* trim page-granular tail */
    test_assert(sameBits(evaled, direct));
  }

  /* Perturb an identically-built cage, subdivide it recursively from scratch,
   * and compare against evaluating the CACHED tables on the perturbed coords. */
  Mesh *cage2 = build();
  for (int vi : cage2->v) {
    cage2->v.co[vi] +=
        float3(float(vi % 5) * 0.0625f, float((vi + 1) % 3) * 0.03125f,
               float((vi + 2) % 7) * 0.015625f);
  }
  subdiv::Refiner rb;
  rb.refine(*cage2, nLevels);

  Vector<float3> cage2Co;
  subdiv::gatherVertCo(*cage2, cage2Co);
  for (int L = 0; L < nLevels; L++) {
    ra.evalFromCage(cage2Co, L + 1, evaled);
    subdiv::gatherVertCo(*rb.levels[L].mesh, direct);
    direct.resize(rb.levels[L].vertCount);
    test_assert(sameBits(evaled, direct));
    fprintf(stderr, "%s: level %d bit-exact (%d verts)\n", tag, L + 1,
            rb.levels[L].vertCount);
  }

  checkGrids(ra, *cage1);

  alloc::Delete(cage1);
  alloc::Delete(cage2);
}

static void checkLevelShape(subdiv::Refiner &r, int L, int expectV, int expectF,
                            const char *tag)
{
  subdiv::SubdivLevel &lvl = r.levels[L];
  test_assert(lvl.vertCount == expectV);
  test_assert(lvl.mesh->v.count == expectV);
  test_assert(lvl.mesh->f.count == expectF);
  for (int fi : lvl.mesh->f) {
    test_assert(faceVertCount(*lvl.mesh, fi) == 4);
  }
  fprintf(stderr, "%s L%d: V=%d F=%d\n", tag, L + 1, lvl.vertCount,
          lvl.mesh->f.count);
}

static void testCube()
{
  Mesh *m = buildCube();
  test_assert(m->v.count == 8 && m->f.count == 6 && m->e.count == 12);

  subdiv::Refiner r;
  r.refine(*m, 3);
  test_assert(r.gridCount() == 24);
  checkLevelShape(r, 0, 26, 24, "cube");
  checkLevelShape(r, 1, 98, 96, "cube");
  checkLevelShape(r, 2, 386, 384, "cube");

  subdiv::SubdivLevel &l1 = r.levels[0];
  Mesh &m1 = *l1.mesh;

  /* Face point of the +z face: its center. */
  int topFace = ELEM_NONE;
  for (int fi : m->f) {
    int c0 = m->l.c[m->f.l[fi]], cc = c0;
    bool top = true;
    do {
      top = top && m->v.co[m->c.v[cc]][2] > 0.0f;
      cc = m->c.next[cc];
    } while (cc != c0);
    if (top) {
      topFace = fi;
      break;
    }
  }
  test_assert(topFace != ELEM_NONE);
  test_assert(near3(m1.v.co[l1.facePointOf[topFace]], 0.0f, 0.0f, 0.5f));

  /* Smooth edge point: edge (-.5,-.5,.5)-(.5,-.5,.5) -> (0, -0.375, 0.375). */
  int ea = findVert(*m, -0.5f, -0.5f, 0.5f), eb = findVert(*m, 0.5f, -0.5f, 0.5f);
  int e1 = m->find_edge(ea, eb);
  test_assert(e1 != ELEM_NONE);
  test_assert(near3(m1.v.co[l1.edgePointOf[e1]], 0.0f, -0.375f, 0.375f));

  /* Smooth vertex point, valence 3: (.5,.5,.5) -> (5/18, 5/18, 5/18). */
  int vc = findVert(*m, 0.5f, 0.5f, 0.5f);
  float w = 5.0f / 18.0f;
  test_assert(near3(m1.v.co[l1.vertPointOf[vc]], w, w, w));

  alloc::Delete(m);
}

static void testCreasedCube()
{
  Mesh *m = buildCreasedCube();
  subdiv::Refiner r;
  r.refine(*m, 2);
  checkLevelShape(r, 0, 26, 24, "creased-cube");
  checkLevelShape(r, 1, 98, 96, "creased-cube");

  subdiv::SubdivLevel &l1 = r.levels[0];
  Mesh &m1 = *l1.mesh;

  /* Creased edge point: the exact midpoint. */
  int a = findVert(*m, -0.5f, -0.5f, 0.5f), b = findVert(*m, 0.5f, -0.5f, 0.5f);
  int e1 = m->find_edge(a, b);
  test_assert(near3(m1.v.co[l1.edgePointOf[e1]], 0.0f, -0.5f, 0.5f, 0.0f));

  /* Crease vertex (two sharp ring edges): 3/4 v + 1/8 (ring neighbors). */
  int vt = findVert(*m, 0.5f, 0.5f, 0.5f);
  test_assert(near3(m1.v.co[l1.vertPointOf[vt]], 0.375f, 0.375f, 0.5f));

  /* Bottom verts are unaffected by the top crease: smooth valence-3 rule. */
  int vb = findVert(*m, 0.5f, 0.5f, -0.5f);
  float w = 5.0f / 18.0f;
  test_assert(near3(m1.v.co[l1.vertPointOf[vb]], w, w, -w));

  /* Smooth edge point whose endpoint is a crease vert is still smooth-rule. */
  int vt2 = findVert(*m, 0.5f, 0.5f, -0.5f);
  int e2 = m->find_edge(vt, vt2);
  test_assert(near3(m1.v.co[l1.edgePointOf[e2]], 0.375f, 0.375f, 0.0f));

  /* Sharp flags propagate to both child edges, every level. */
  test_assert(countSharpEdges(*m) == 4);
  test_assert(countSharpEdges(m1) == 8);
  test_assert(countSharpEdges(*r.levels[1].mesh) == 16);

  /* Level-2 child edge points of level-1 sharp edges are exact midpoints. */
  BoolAttrView *sharp = boundary::findBoolEdgeView(&m1, boundary::EDGE_SHARP);
  test_assert(sharp != nullptr);
  subdiv::SubdivLevel &l2 = r.levels[1];
  for (int ei : m1.e) {
    if (!(*sharp)[ei]) {
      continue;
    }
    int v0 = m1.e.vs[ei][0], v1 = m1.e.vs[ei][1];
    int lo = v0 < v1 ? v0 : v1, hi = v0 < v1 ? v1 : v0;
    /* Mirror StencilTable::eval's op sequence exactly (zero-init, two +=). */
    float3 mid;
    mid += m1.v.co[lo] * 0.5f;
    mid += m1.v.co[hi] * 0.5f;
    float3 got = l2.mesh->v.co[l2.edgePointOf[ei]];
    test_assert(std::memcmp(&mid, &got, sizeof(float3)) == 0);
  }

  alloc::Delete(m);
}

static void testFan()
{
  Mesh *m = buildFan();
  test_assert(m->v.count == 6 && m->e.count == 9 && m->f.count == 4);

  subdiv::Refiner r;
  r.refine(*m, 2);
  test_assert(r.gridCount() == 12);
  checkLevelShape(r, 0, 19, 12, "fan");
  checkLevelShape(r, 1, 61, 48, "fan"); /* V2 = 19 + 30 + 12 */

  subdiv::SubdivLevel &l1 = r.levels[0];
  Mesh &m1 = *l1.mesh;

  /* Fan center: two boundary spokes -> crease rule -> 0.125*(r0 + r4) = 0. */
  int vc = findVert(*m, 0, 0, 0);
  test_assert(near3(m1.v.co[l1.vertPointOf[vc]], 0.0f, 0.0f, 0.0f, 0.0f));

  /* Interior ring vert (0,1,0): crease rule along the boundary ring. */
  int v2 = findVert(*m, 0, 1, 0);
  test_assert(near3(m1.v.co[l1.vertPointOf[v2]], 0.0f, 0.9375f, 0.0f));

  /* Open-end ring vert (1,0,0): crease rule with the center as one neighbor. */
  int v0 = findVert(*m, 1, 0, 0);
  test_assert(near3(m1.v.co[l1.vertPointOf[v0]], 0.84375f, 0.09375f, 0.0f));

  /* Boundary ring edge -> midpoint. */
  int v1 = findVert(*m, 0.75f, 0.75f, 0);
  int eRing = m->find_edge(v0, v1);
  test_assert(near3(m1.v.co[l1.edgePointOf[eRing]], 0.875f, 0.375f, 0.0f));

  /* Interior spoke center-(0,1,0), two triangle faces:
   * 1/4 (c + r2) + 1/12 (c+r1+r2) + 1/12 (c+r2+r3) -> y = 13/24. */
  int eSpoke = m->find_edge(vc, v2);
  test_assert(near3(m1.v.co[l1.edgePointOf[eSpoke]], 0.0f, 13.0f / 24.0f, 0.0f));

  alloc::Delete(m);
}

static void testPentagon()
{
  Mesh *m = buildPentagon();
  subdiv::Refiner r;
  r.refine(*m, 2);
  test_assert(r.gridCount() == 5);
  checkLevelShape(r, 0, 11, 5, "pentagon");
  checkLevelShape(r, 1, 31, 20, "pentagon");

  subdiv::SubdivLevel &l1 = r.levels[0];
  Mesh &m1 = *l1.mesh;

  /* Lone n-gon: face point = centroid (the non-dyadic 1/5 weights path). */
  test_assert(near3(m1.v.co[l1.facePointOf[0]], -0.1f, 0.0f, 0.0f));

  /* All-boundary vert: crease rule. p0 -> 0.75*p0 + 0.125*(p1 + p4). */
  int p0 = findVert(*m, 1, 0, 0);
  test_assert(near3(m1.v.co[l1.vertPointOf[p0]], 0.8125f, 0.0f, 0.0f));

  /* Boundary edge point = midpoint. */
  int p1 = findVert(*m, 0.25f, 1, 0);
  int e1 = m->find_edge(p0, p1);
  test_assert(near3(m1.v.co[l1.edgePointOf[e1]], 0.625f, 0.5f, 0.0f));

  alloc::Delete(m);
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  testCube();
  testCreasedCube();
  testFan();
  testPentagon();

  checkBitExact(buildCube, 3, "cube");
  checkBitExact(buildCreasedCube, 3, "creased-cube");
  checkBitExact(buildFan, 2, "fan");
  checkBitExact(buildPentagon, 2, "pentagon");

  /* Skip test_end(): attr name strings stay live in the alloc tracker
   * (mirrors the other spatial/mesh tests). */
  return retval;
}
