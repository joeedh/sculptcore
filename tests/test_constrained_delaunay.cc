#include "test_util.h"

#include "mesh/mesh.h"
#include "mesh/utils/triangulate.h"

#include "litestl/math/vector.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace litestl;
using namespace litestl::math;
using namespace litestl::util;

/* --- 2D helpers (float-tolerant; no exact equality). --- */
static float2 proj2(const float3 &co, const float3 &ub, const float3 &vb)
{
  return float2(co.dot(ub), co.dot(vb));
}

static double shoelace(const Vector<float2> &poly)
{
  double a = 0.0;
  int n = int(poly.size());
  for (int i = 0; i < n; i++) {
    float2 p = poly[i], q = poly[(i + 1) % n];
    a += double(p[0]) * q[1] - double(q[0]) * p[1];
  }
  return a * 0.5;
}

static double triArea2(float2 a, float2 b, float2 c)
{
  return 0.5 *
         (double(b[0] - a[0]) * (c[1] - a[1]) - double(b[1] - a[1]) * (c[0] - a[0]));
}

/* Even-odd ray test; eps-free (robust for the axis-aligned test polygons). */
static bool pointInPoly2(float2 pt, const Vector<float2> &poly)
{
  bool in = false;
  int n = int(poly.size());
  for (int i = 0, j = n - 1; i < n; j = i++) {
    float2 a = poly[i], b = poly[j];
    if (((a[1] > pt[1]) != (b[1] > pt[1])) &&
        (pt[0] < (double(b[0] - a[0]) * (pt[1] - a[1]) / (b[1] - a[1])) + a[0]))
    {
      in = !in;
    }
  }
  return in;
}

/* Triangulate a single-loop face and assert the result tiles the polygon:
 * exactly n-2 triangles, all using face verts/corners, consistent winding,
 * total area == polygon area, every centroid inside, and the mesh untouched. */
static bool runFaceCase(const char *label, Vector<float3> &verts3, float3 normal)
{
  Mesh m;
  Vector<int> vs;
  for (const float3 &p : verts3) {
    vs.append(m.make_vertex(p));
  }
  int fi = m.make_face(std::span<int>(vs.data(), vs.size()));

  int vc = m.v.count, ec = m.e.count, fc = m.f.count, cc = m.c.count, lc = m.l.count;

  /* Project the face loop the same way triangulateFace does. */
  Vector<float3> outer3;
  for (int v : vs)
    outer3.append(m.v.co[v]);
  float3 nrm = detail_delaunay::fitPlaneNormal(
      util::span<const float3>(outer3.data(), outer3.size()));
  if (normal.length() > 1e-6f)
    nrm = normal;
  if (nrm.length() < 1e-6f)
    nrm = float3(0, 0, 1);
  else
    nrm.normalize();
  float3 ub, vb;
  detail_delaunay::planeBasis(nrm, ub, vb);

  Vector<float2> poly;
  {
    int li = m.f.l[fi], c0 = m.l.c[li], c = c0;
    do {
      poly.append(proj2(m.v.co[m.c.v[c]], ub, vb));
      c = m.c.next[c];
    } while (c != c0);
  }
  double polyArea = std::fabs(shoelace(poly));

  Vector<Tri> tris;
  auto ok = triangulateFace(m, fi, tris);
  test_assert(bool(ok));

  /* Non-mutation: triangulateFace must not change the mesh. */
  bool unchanged = m.v.count == vc && m.e.count == ec && m.f.count == fc &&
                   m.c.count == cc && m.l.count == lc;
  if (!unchanged) {
    fprintf(stderr, "[%s] mesh mutated by triangulateFace\n", label);
    return false;
  }

  int n = int(vs.size());
  if (int(tris.size()) != n - 2) {
    fprintf(stderr, "[%s] tri count %d != expected %d\n", label, int(tris.size()), n - 2);
    return false;
  }

  Set<int> faceVerts;
  for (int v : vs)
    faceVerts.add(v);

  double sumArea = 0.0;
  bool signSet = false, sign = false;
  for (const Tri &t : tris) {
    for (int k = 0; k < 3; k++) {
      if (!faceVerts.contains(t.v[k])) {
        fprintf(stderr, "[%s] tri vert %d not a face vert\n", label, t.v[k]);
        return false;
      }
      if (m.c.v[t.c[k]] != t.v[k]) {
        fprintf(stderr, "[%s] corner/vert mismatch c=%d v=%d\n", label, t.c[k], t.v[k]);
        return false;
      }
    }
    float2 A = proj2(m.v.co[t.v[0]], ub, vb);
    float2 B = proj2(m.v.co[t.v[1]], ub, vb);
    float2 C = proj2(m.v.co[t.v[2]], ub, vb);
    double sa = triArea2(A, B, C);
    sumArea += std::fabs(sa);
    bool s = sa > 0.0;
    if (!signSet) {
      sign = s;
      signSet = true;
    } else if (s != sign) {
      fprintf(stderr, "[%s] inconsistent triangle winding\n", label);
      return false;
    }
    float2 cent((A[0] + B[0] + C[0]) / 3.0f, (A[1] + B[1] + C[1]) / 3.0f);
    if (!pointInPoly2(cent, poly)) {
      fprintf(stderr, "[%s] triangle centroid outside polygon\n", label);
      return false;
    }
  }

  if (std::fabs(sumArea - polyArea) > 1e-4 * polyArea + 1e-6) {
    fprintf(stderr, "[%s] area %.6f != polygon area %.6f\n", label, sumArea, polyArea);
    return false;
  }
  return true;
}

/* Direct (mesh-free) CDT of a polygon-with-holes. Returns the triangle count and
 * fills out_tris; the caller asserts geometry. */
static int
runDirectCdt(Vector<float2> &pts, Vector<detail_delaunay::DEdge> &segs, Vector<int> &out)
{
  auto ok = constrainedDelaunay2D(
      util::span<const float2>(pts.data(), pts.size()),
      util::span<const detail_delaunay::DEdge>(segs.data(), segs.size()),
      out,
      true);
  test_assert(bool(ok));
  return int(out.size()) / 3;
}

static void closeRing(Vector<detail_delaunay::DEdge> &segs, int base, int n)
{
  for (int i = 0; i < n; i++)
    segs.append({base + i, base + (i + 1) % n});
}

int main()
{
  /* --- Single-loop faces through triangulateFace. --- */

  /* Convex square -> fan path, 2 tris. */
  {
    Vector<float3> p;
    p.append(float3(0, 0, 0));
    p.append(float3(1, 0, 0));
    p.append(float3(1, 1, 0));
    p.append(float3(0, 1, 0));
    test_assert(runFaceCase("convex-square", p, float3(0, 0, 0)));
  }

  /* Convex hexagon -> fan path, 4 tris. */
  {
    Vector<float3> p;
    for (int i = 0; i < 6; i++) {
      float a = float(i) / 6.0f * 6.2831853f;
      p.append(float3(std::cos(a), std::sin(a), 0));
    }
    test_assert(runFaceCase("convex-hexagon", p, float3(0, 0, 0)));
  }

  /* Concave U (non-star-shaped: a fan from v0 would exit the notch). CDT path. */
  {
    Vector<float3> p;
    float coords[8][2] = {{0, 0}, {3, 0}, {3, 3}, {2, 3}, {2, 1}, {1, 1}, {1, 3}, {0, 3}};
    for (auto &c : coords)
      p.append(float3(c[0], c[1], 0));
    test_assert(runFaceCase("concave-U", p, float3(0, 0, 0)));
  }

  /* Five-point star (5 reflex vertices). CDT path. */
  {
    Vector<float3> p;
    for (int i = 0; i < 10; i++) {
      float a = -1.5707963f + float(i) / 10.0f * 6.2831853f;
      float rad = (i % 2 == 0) ? 2.0f : 0.8f;
      p.append(float3(rad * std::cos(a), rad * std::sin(a), 0));
    }
    test_assert(runFaceCase("star", p, float3(0, 0, 0)));
  }

  /* Concave U mapped onto a tilted plane -> exercises fitPlaneNormal/planeBasis. */
  {
    float3 nrm(1, 2, 3);
    nrm.normalize();
    float3 ub, vb;
    detail_delaunay::planeBasis(nrm, ub, vb);
    Vector<float3> p;
    float coords[8][2] = {{0, 0}, {3, 0}, {3, 3}, {2, 3}, {2, 1}, {1, 1}, {1, 3}, {0, 3}};
    for (auto &c : coords)
      p.append(ub * c[0] + vb * c[1]);
    test_assert(runFaceCase("concave-U-tilted", p, nrm));
  }

  /* --- Direct CDT: polygon with a hole. --- */

  /* Square (area 16) with a centered square hole (area 4) -> 8 tris, area 12. */
  {
    Vector<float2> outer;
    outer.append(float2(0, 0));
    outer.append(float2(4, 0));
    outer.append(float2(4, 4));
    outer.append(float2(0, 4));
    Vector<float2> hole;
    hole.append(float2(1, 1));
    hole.append(float2(3, 1));
    hole.append(float2(3, 3));
    hole.append(float2(1, 3));

    /* Build twice: hole wound same as outer, then reversed -> identical result. */
    for (int rev = 0; rev < 2; rev++) {
      Vector<float2> pts;
      for (auto &o : outer)
        pts.append(o);
      if (!rev) {
        for (auto &h : hole)
          pts.append(h);
      } else {
        for (int i = 3; i >= 0; i--)
          pts.append(hole[i]);
      }
      Vector<detail_delaunay::DEdge> segs;
      closeRing(segs, 0, 4);
      closeRing(segs, 4, 4);

      Vector<int> out;
      int ntris = runDirectCdt(pts, segs, out);
      const char *lbl = rev ? "hole-reversed" : "hole";
      test_assert(ntris == 8);

      double area = 0.0;
      for (int i = 0; i + 2 < int(out.size()); i += 3) {
        float2 A = pts[out[i]], B = pts[out[i + 1]], C = pts[out[i + 2]];
        double sa = triArea2(A, B, C);
        if (sa <= 0.0) {
          fprintf(stderr, "[%s] non-CCW triangle\n", lbl);
          return 1;
        }
        area += sa;
        float2 cent((A[0] + B[0] + C[0]) / 3.0f, (A[1] + B[1] + C[1]) / 3.0f);
        if (pointInPoly2(cent, hole)) {
          fprintf(stderr, "[%s] triangle centroid inside hole\n", lbl);
          return 1;
        }
        if (!pointInPoly2(cent, outer)) {
          fprintf(stderr, "[%s] triangle centroid outside outer\n", lbl);
          return 1;
        }
      }
      if (std::fabs(area - 12.0) > 1e-4) {
        fprintf(stderr, "[%s] area %.6f != 12\n", lbl, area);
        return 1;
      }
    }
  }

  /* --- Direct CDT: degenerate inputs return empty gracefully. --- */
  {
    Vector<float2> pts;
    pts.append(float2(0, 0));
    pts.append(float2(1, 0));
    Vector<detail_delaunay::DEdge> segs;
    Vector<int> out;
    test_assert(runDirectCdt(pts, segs, out) == 0); /* <3 points */
  }
  {
    Vector<float2> pts;
    for (int i = 0; i < 5; i++)
      pts.append(float2(float(i), 0)); /* collinear */
    Vector<detail_delaunay::DEdge> segs;
    closeRing(segs, 0, 5);
    Vector<int> out;
    test_assert(runDirectCdt(pts, segs, out) == 0);
  }

  printf("constrained delaunay test done\n");
  return test_end();
}
