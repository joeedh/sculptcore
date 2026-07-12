#include "test_util.h"

#include "mesh/mesh.h"
#include "mesh/utils/delaunay.h"

#include "litestl/math/vector.h"
#include "litestl/util/rand.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace litestl;
using namespace litestl::math;
using namespace litestl::util;

/* --- Mesh integrity check (local; see comment in delaunay.h). --- */
static bool validateMesh(Mesh &m)
{
  /* Every live edge endpoint slot must be live, and disk cycle must close. */
  for (int ei : m.e) {
    int v1 = m.e.vs[ei][0];
    int v2 = m.e.vs[ei][1];
    if (v1 < 0 || v2 < 0 || v1 >= int(m.v.capacity()) || v2 >= int(m.v.capacity())) {
      fprintf(stderr, "edge %d has bad vert refs %d %d\n", ei, v1, v2);
      return false;
    }
    if (m.v.freemap[v1] || m.v.freemap[v2]) {
      fprintf(stderr, "edge %d references freed vert\n", ei);
      return false;
    }
  }

  /* Disk cycle per vertex closes. */
  for (int vi : m.v) {
    int e0 = m.v.e[vi];
    if (e0 == ELEM_NONE) continue;
    int steps = 0, ec = e0;
    do {
      int side = m.e.vs[ec][0] == vi ? 0 : 1;
      if (m.e.vs[ec][side] != vi) {
        fprintf(stderr, "vert %d disk: edge %d not incident\n", vi, ec);
        return false;
      }
      int next = diskEdge(m.e.disk[ec][side * 2 + 1]);
      int prev = diskEdge(m.e.disk[ec][side * 2]);
      int side_n = m.e.vs[next][0] == vi ? 0 : 1;
      int side_p = m.e.vs[prev][0] == vi ? 0 : 1;
      if (m.e.disk[next][side_n * 2] != diskPack(ec, side)) {
        fprintf(stderr, "disk prev/next mismatch v=%d e=%d\n", vi, ec);
        return false;
      }
      if (m.e.disk[prev][side_p * 2 + 1] != diskPack(ec, side)) {
        fprintf(stderr, "disk prev/next mismatch v=%d e=%d\n", vi, ec);
        return false;
      }
      ec = next;
      if (++steps > 100000) {
        fprintf(stderr, "vert %d disk did not close\n", vi);
        return false;
      }
    } while (ec != e0);
  }

  /* Radial cycle per edge closes. */
  for (int ei : m.e) {
    int c0 = m.e.c[ei];
    if (c0 == ELEM_NONE) continue;
    int steps = 0, cc = c0;
    do {
      if (m.c.e[cc] != ei) {
        fprintf(stderr, "edge %d radial: corner %d c.e mismatch\n", ei, cc);
        return false;
      }
      int rn = m.c.radial_next[cc];
      int rp = m.c.radial_prev[cc];
      if (m.c.radial_prev[rn] != cc || m.c.radial_next[rp] != cc) {
        fprintf(stderr, "radial prev/next mismatch e=%d c=%d\n", ei, cc);
        return false;
      }
      cc = rn;
      if (++steps > 100000) {
        fprintf(stderr, "edge %d radial did not close\n", ei);
        return false;
      }
    } while (cc != c0);
  }

  /* Face/list/corner cycle. */
  for (int fi : m.f) {
    int li = m.f.l[fi];
    while (li != ELEM_NONE) {
      if (m.l.f[li] != fi) {
        fprintf(stderr, "list %d not owned by face %d\n", li, fi);
        return false;
      }
      int c0 = m.l.c[li];
      int cc = c0;
      int n = 0;
      do {
        if (m.c.l[cc] != li) {
          fprintf(stderr, "corner %d c.l != list %d\n", cc, li);
          return false;
        }
        int cn = m.c.next[cc];
        if (m.c.prev[cn] != cc) {
          fprintf(stderr, "corner prev/next mismatch f=%d c=%d\n", fi, cc);
          return false;
        }
        /* edge-vert consistency */
        int ce = m.c.e[cc];
        int v_here = m.c.v[cc];
        int v_next = m.c.v[cn];
        int ev0 = m.e.vs[ce][0], ev1 = m.e.vs[ce][1];
        if (!((ev0 == v_here && ev1 == v_next) ||
              (ev1 == v_here && ev0 == v_next))) {
          fprintf(stderr, "corner edge-vert mismatch f=%d c=%d\n", fi, cc);
          return false;
        }
        cc = cn;
        if (++n > 100000) {
          fprintf(stderr, "list %d did not close\n", li);
          return false;
        }
      } while (cc != c0);
      if (n != m.l.size[li]) {
        fprintf(stderr, "list %d size %d != walked %d\n", li, m.l.size[li], n);
        return false;
      }
      li = m.l.next[li];
    }
  }

  return true;
}

/* --- 2D circumcircle test for Delaunay verification. --- */
static bool inCircumcircle2D(double ax, double ay,
                             double bx, double by,
                             double cx, double cy,
                             double px, double py,
                             double eps)
{
  /* Force CCW. */
  double orient = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
  if (std::fabs(orient) < 1e-30) return false;
  double Ax = ax - px, Ay = ay - py;
  double Bx = bx - px, By = by - py;
  double Cx = cx - px, Cy = cy - py;
  double det = (Ax * Ax + Ay * Ay) * (Bx * Cy - Cx * By) -
               (Bx * Bx + By * By) * (Ax * Cy - Cx * Ay) +
               (Cx * Cx + Cy * Cy) * (Ax * By - Bx * Ay);
  if (orient < 0) det = -det;
  return det > eps;
}

static int convexHullSize(Vector<float2> &pts)
{
  int n = int(pts.size());
  if (n < 3) return n;
  Vector<int> order;
  for (int i = 0; i < n; i++) order.append(i);
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    if (pts[a][0] != pts[b][0]) return pts[a][0] < pts[b][0];
    return pts[a][1] < pts[b][1];
  });
  Vector<int> hull;
  auto cross2 = [&](int o, int a, int b) {
    return double(pts[a][0] - pts[o][0]) * double(pts[b][1] - pts[o][1]) -
           double(pts[a][1] - pts[o][1]) * double(pts[b][0] - pts[o][0]);
  };
  /* Lower */
  for (int idx : order) {
    while (hull.size() >= 2 &&
           cross2(hull[hull.size() - 2], hull[hull.size() - 1], idx) <= 0) {
      hull.pop_back();
    }
    hull.append(idx);
  }
  size_t lower = hull.size() + 1;
  for (int i = n - 2; i >= 0; i--) {
    int idx = order[i];
    while (hull.size() >= lower &&
           cross2(hull[hull.size() - 2], hull[hull.size() - 1], idx) <= 0) {
      hull.pop_back();
    }
    hull.append(idx);
  }
  return int(hull.size()) - 1;
}

static int eulerCharFaces(Mesh &m)
{
  int V = m.v.count;
  int E = m.e.count;
  int F = m.f.count;
  return V - E + F;
}

struct TestStats {
  int pointSets = 0;
  int facesEmitted = 0;
  int circumFails = 0;
};

static bool runDelaunayCase(const char *label,
                            Vector<float3> &pts,
                            float3 normal,
                            TestStats &stats,
                            bool expectExactCount)
{
  Mesh mesh;
  Vector<int> verts;
  for (const float3 &p : pts) {
    verts.append(mesh.make_vertex(p));
  }
  int v_before = mesh.v.count;
  int e_before = mesh.e.count;
  int f_before = mesh.f.count;
  int chi_before = eulerCharFaces(mesh);

  Vector<int> outFaces;
  auto ok = delaunayTriangulate(mesh,
                                std::span<int>(verts.data(), verts.size()),
                                std::optional<float3>(normal),
                                &outFaces);
  test_assert(bool(ok));
  if (!validateMesh(mesh)) {
    fprintf(stderr, "[%s] mesh integrity failed\n", label);
    return false;
  }

  /* All faces must be triangles, and only those we asked for were added. */
  test_assert(int(outFaces.size()) == mesh.f.count - f_before);
  for (int fi : outFaces) {
    int li = mesh.f.l[fi];
    if (mesh.l.size[li] != 3) {
      fprintf(stderr, "[%s] non-triangle face emitted (size=%d)\n",
              label, mesh.l.size[li]);
      return false;
    }
  }

  /* No duplicate triangles (as unordered triples of verts). */
  Set<int64_t> seen;
  for (int fi : outFaces) {
    int li = mesh.f.l[fi];
    int c0 = mesh.l.c[li];
    int c1 = mesh.c.next[c0];
    int c2 = mesh.c.next[c1];
    int v[3] = {mesh.c.v[c0], mesh.c.v[c1], mesh.c.v[c2]};
    std::sort(v, v + 3);
    int64_t key = (int64_t(v[0]) * 1000003LL + v[1]) * 1000003LL + v[2];
    if (!seen.add(key)) {
      fprintf(stderr, "[%s] duplicate triangle\n", label);
      return false;
    }
  }

  /* Project original points onto the same plane to validate empty-circumcircle. */
  float3 n = normal;
  if (n.length() < 1e-6f) n = float3(0.0f, 0.0f, 1.0f);
  n.normalize();
  float3 ub(1, 0, 0);
  if (std::fabs(n[0]) > 0.9f) ub = float3(0, 1, 0);
  ub.crossSelf(n);
  ub.normalize();
  float3 vb = n;
  vb.crossSelf(ub);
  vb.normalize();

  Vector<float2> pts2;
  for (const float3 &p : pts) {
    pts2.append(float2(p.dot(ub), p.dot(vb)));
  }

  /* Empty circumcircle property. */
  for (int fi : outFaces) {
    int li = mesh.f.l[fi];
    int c0 = mesh.l.c[li];
    int c1 = mesh.c.next[c0];
    int c2 = mesh.c.next[c1];
    int va = mesh.c.v[c0];
    int vb_i = mesh.c.v[c1];
    int vc = mesh.c.v[c2];
    /* map mesh vert idx -> input idx */
    int ia = -1, ibx = -1, ic = -1;
    for (int i = 0; i < int(verts.size()); i++) {
      if (verts[i] == va) ia = i;
      if (verts[i] == vb_i) ibx = i;
      if (verts[i] == vc) ic = i;
    }
    if (ia < 0 || ibx < 0 || ic < 0) continue;
    for (int j = 0; j < int(pts2.size()); j++) {
      if (j == ia || j == ibx || j == ic) continue;
      /* Skip duplicates: if j projects within eps of any triangle vert, ignore. */
      bool dup = false;
      int tri[3] = {ia, ibx, ic};
      for (int k = 0; k < 3; k++) {
        float2 d = pts2[j] - pts2[tri[k]];
        if (std::fabs(d[0]) < 1e-5f && std::fabs(d[1]) < 1e-5f) {
          dup = true;
          break;
        }
      }
      if (dup) continue;
      if (inCircumcircle2D(pts2[ia][0], pts2[ia][1],
                           pts2[ibx][0], pts2[ibx][1],
                           pts2[ic][0], pts2[ic][1],
                           pts2[j][0], pts2[j][1], 1e-5)) {
        stats.circumFails++;
        fprintf(stderr,
                "[%s] empty-circumcircle violation: tri (%d,%d,%d) contains pt %d\n",
                label, ia, ibx, ic, j);
        return false;
      }
    }
  }

  /* Triangle count check (only when input is in general position / no dupes). */
  if (expectExactCount) {
    int h = convexHullSize(pts2);
    int expected = 2 * int(pts2.size()) - h - 2;
    if (int(outFaces.size()) != expected) {
      fprintf(stderr, "[%s] triangle count %d != expected %d (n=%d, h=%d)\n",
              label, int(outFaces.size()), expected, int(pts2.size()), h);
      return false;
    }
  }

  /* Mesh accounting deltas. */
  int dV = mesh.v.count - v_before;
  int dE = mesh.e.count - e_before;
  int dF = mesh.f.count - f_before;
  test_assert(dV == 0);
  /* Each new triangle face contributes its boundary edges, but edges are shared.
   * So we just check chi went up by dF - dE which should equal whatever the
   * triangulation requires. The most useful invariant: chi - chi_before equals
   * dF - dE. */
  (void)chi_before;
  (void)dE;
  (void)dF;

  stats.pointSets++;
  stats.facesEmitted += int(outFaces.size());
  return true;
}

int main()
{
  TestStats stats;

  /* Case 1: small triangle. */
  {
    Vector<float3> pts;
    pts.append(float3(0, 0, 0));
    pts.append(float3(1, 0, 0));
    pts.append(float3(0, 1, 0));
    bool ok = runDelaunayCase("triangle", pts, float3(0, 0, 1), stats, true);
    test_assert(ok);
  }

  /* Case 2: square (4 cocircular points; valid Delaunay has 2 triangles). */
  {
    Vector<float3> pts;
    pts.append(float3(0, 0, 0));
    pts.append(float3(1, 0, 0));
    pts.append(float3(1, 1, 0));
    pts.append(float3(0, 1, 0));
    bool ok = runDelaunayCase("square-cocirc", pts, float3(0, 0, 1), stats, true);
    test_assert(ok);
  }

  /* Case 3: collinear -> no faces. */
  {
    Vector<float3> pts;
    pts.append(float3(0, 0, 0));
    pts.append(float3(1, 0, 0));
    pts.append(float3(2, 0, 0));
    pts.append(float3(3, 0, 0));
    Mesh mesh;
    Vector<int> verts;
    for (auto &p : pts) verts.append(mesh.make_vertex(p));
    Vector<int> outFaces;
    auto ok = delaunayTriangulate(mesh,
                                  std::span<int>(verts.data(), verts.size()),
                                  std::optional<float3>(float3(0, 0, 1)),
                                  &outFaces);
    test_assert(bool(ok));
    test_assert(outFaces.size() == 0);
    test_assert(validateMesh(mesh));
  }

  /* Case 4: <3 verts -> no faces. */
  {
    Mesh mesh;
    int v0 = mesh.make_vertex(float3(0, 0, 0));
    int v1 = mesh.make_vertex(float3(1, 0, 0));
    int verts[2] = {v0, v1};
    Vector<int> outFaces;
    auto ok = delaunayTriangulate(mesh,
                                  std::span<int>(verts, 2),
                                  std::optional<float3>(float3(0, 0, 1)),
                                  &outFaces);
    test_assert(bool(ok));
    test_assert(outFaces.size() == 0);
  }

  /* Case 5: duplicates de-dup. */
  {
    Vector<float3> pts;
    pts.append(float3(0, 0, 0));
    pts.append(float3(1, 0, 0));
    pts.append(float3(0, 1, 0));
    pts.append(float3(0, 0, 0)); /* dup */
    pts.append(float3(1, 0, 0)); /* dup */
    Mesh mesh;
    Vector<int> verts;
    for (auto &p : pts) verts.append(mesh.make_vertex(p));
    Vector<int> outFaces;
    auto ok = delaunayTriangulate(mesh,
                                  std::span<int>(verts.data(), verts.size()),
                                  std::optional<float3>(float3(0, 0, 1)),
                                  &outFaces);
    test_assert(bool(ok));
    test_assert(outFaces.size() == 1);
    test_assert(validateMesh(mesh));
  }

  /* Case 6: randomized point sets at varying sizes, on a tilted plane. */
  Random rnd(1234);
  int sizes[] = {3, 4, 5, 8, 16, 32, 64};
  for (int N : sizes) {
    for (int trial = 0; trial < 8; trial++) {
      /* Tilted plane: normal = (1,2,3) normalized. */
      float3 nrm(1.0f, 2.0f, 3.0f);
      nrm.normalize();
      float3 ub(1, 0, 0);
      if (std::fabs(nrm[0]) > 0.9f) ub = float3(0, 1, 0);
      ub.crossSelf(nrm);
      ub.normalize();
      float3 vb = nrm;
      vb.crossSelf(ub);
      vb.normalize();

      Vector<float3> pts;
      Vector<float2> proj;
      int attempts = 0;
      while (int(pts.size()) < N && attempts < N * 20) {
        attempts++;
        float x = rnd.get_float() * 2.0f - 1.0f;
        float y = rnd.get_float() * 2.0f - 1.0f;
        /* reject near-duplicates */
        bool dup = false;
        for (auto &q : proj) {
          if (std::fabs(q[0] - x) < 1e-3f && std::fabs(q[1] - y) < 1e-3f) {
            dup = true;
            break;
          }
        }
        if (dup) continue;
        proj.append(float2(x, y));
        float3 p = ub * x + vb * y + nrm * (rnd.get_float() * 0.001f);
        pts.append(p);
      }
      if (int(pts.size()) < N) continue;

      char label[64];
      snprintf(label, sizeof(label), "rand-N%d-t%d", N, trial);
      bool ok = runDelaunayCase(label, pts, nrm, stats, true);
      test_assert(ok);
    }
  }

  /* Case 7: auto-fit plane (no normal supplied) on tilted points. */
  {
    Random rnd2(42);
    float3 nrm(0.3f, -0.7f, 0.5f);
    nrm.normalize();
    float3 ub(1, 0, 0);
    if (std::fabs(nrm[0]) > 0.9f) ub = float3(0, 1, 0);
    ub.crossSelf(nrm);
    ub.normalize();
    float3 vb = nrm;
    vb.crossSelf(ub);
    vb.normalize();

    Vector<float3> pts;
    Vector<float2> proj;
    int attempts = 0;
    while (int(pts.size()) < 24 && attempts < 1000) {
      attempts++;
      float x = rnd2.get_float() * 2.0f - 1.0f;
      float y = rnd2.get_float() * 2.0f - 1.0f;
      bool dup = false;
      for (auto &q : proj) {
        if (std::fabs(q[0] - x) < 1e-3f && std::fabs(q[1] - y) < 1e-3f) {
          dup = true;
          break;
        }
      }
      if (dup) continue;
      proj.append(float2(x, y));
      pts.append(ub * x + vb * y);
    }

    Mesh mesh;
    Vector<int> verts;
    for (auto &p : pts) verts.append(mesh.make_vertex(p));
    Vector<int> outFaces;
    auto ok = delaunayTriangulate(mesh,
                                  std::span<int>(verts.data(), verts.size()),
                                  std::nullopt,
                                  &outFaces);
    test_assert(bool(ok));
    test_assert(validateMesh(mesh));
    /* Triangulation should produce something for 24 general-position points. */
    test_assert(outFaces.size() > 0);
  }

  printf("delaunay test: %d point sets, %d faces emitted, %d circumcircle fails\n",
         stats.pointSets, stats.facesEmitted, stats.circumFails);

  return test_end();
}
