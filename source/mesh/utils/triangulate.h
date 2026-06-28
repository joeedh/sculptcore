#pragma once

#include "../mesh.h"
#include "../mesh_base.h"
#include "../mesh_proxy.h"
#include "attr_interp.h"
#include "delaunay.h"
#include "litestl/util/error.h"
#include "litestl/util/index_range.h"
#include "litestl/util/vector.h"
#include <cmath>
#include <ranges>
#include <span>

namespace sculptcore::mesh {

/* Fan-triangulate a single n-gon (single outer loop) in place, threading the
 * MeshCallbacks so the spatial tree + meshlog stay in sync, and carrying the
 * face's attrs (`group`, …) + per-corner attrs (`uv`, …) onto each triangle.
 * Perimeter edges are reused by make_face (not killed), so their boundary flags
 * survive; the new diagonals are plain interior edges. No-op for triangles /
 * multi-loop faces. Used by dyntopo to triangulate faces it encounters. */
static inline void triangulateFaceFanCb(Mesh &m, int f, MeshCallbacks *cb = nullptr)
{
  if (f < 0 || f >= int(m.f.capacity()) || m.f.freemap[f] ||
      m.f.list_count[f] != 1) {
    return;
  }
  int li = m.f.l[f];
  if (m.l.size[li] == 3) {
    return;
  }

  AttrRowSnapshot faceSnap;
  snapshotAttrRow(m.f.attrs, f, faceSnap);

  litestl::util::Vector<int, 16> vs;
  litestl::util::Vector<AttrRowSnapshot, 16> csnaps;
  int c0 = m.l.c[li], cc = c0;
  do {
    vs.append(m.c.v[cc]);
    AttrRowSnapshot cs;
    snapshotAttrRow(m.c.attrs, cc, cs);
    csnaps.append(std::move(cs));
    cc = m.c.next[cc];
  } while (cc != c0);

  m.kill_face(f, cb);

  auto cornerOf = [&](int ff, int v) -> int {
    int l = m.f.l[ff], x0 = m.l.c[l], x = x0;
    do {
      if (m.c.v[x] == v) return x;
      x = m.c.next[x];
    } while (x != x0);
    return ELEM_NONE;
  };
  auto snapForVert = [&](int v) -> const AttrRowSnapshot * {
    for (int i = 0; i < int(vs.size()); i++) {
      if (vs[i] == v) return &csnaps[i];
    }
    return nullptr;
  };

  for (int i = 1; i + 1 < int(vs.size()); i++) {
    int tri[3] = {vs[0], vs[i], vs[i + 1]};
    int nf = m.make_face(std::span<int>(tri, 3), cb);
    restoreAttrRow(m.f.attrs, nf, faceSnap);
    for (int k = 0; k < 3; k++) {
      int cv = cornerOf(nf, tri[k]);
      const AttrRowSnapshot *s = snapForVert(tri[k]);
      if (cv != ELEM_NONE && s) {
        restoreAttrRow(m.c.attrs, cv, *s);
      }
    }
  }
}
struct Tri {
  int v[3];
  int c[3];
  int f;
  // int e[3];
};

namespace detail_triangulate {
using litestl::math::float2;
using litestl::math::float3;

/** Convex in plane (ub,vb)? Near-collinear verts are skipped; a sign flip in the
 * consecutive edge crosses marks a reflex (concave) vertex. Winding-agnostic — a
 * convex polygon's non-zero crosses are all one sign, CW or CCW. */
static inline bool loopIsConvex2D(Mesh &m, int li, const float3 &ub, const float3 &vb)
{
  auto proj = [&](int v) { return float2(m.v.co[v].dot(ub), m.v.co[v].dot(vb)); };
  int c0 = m.l.c[li], c = c0, guard = 0;
  bool haveSign = false, sign = false;
  do {
    float2 p = proj(m.c.v[c]);
    float2 pp = proj(m.c.v[m.c.prev[c]]);
    float2 pn = proj(m.c.v[m.c.next[c]]);
    float2 e0 = p - pp, e1 = pn - p;
    float cr = detail_delaunay::dt_cross2(e0, e1);
    if (std::fabs(cr) > 1e-6f * e0.length() * e1.length()) {
      bool s = cr > 0.0f;
      if (!haveSign) {
        sign = s;
        haveSign = true;
      } else if (s != sign) {
        return false;
      }
    }
    c = m.c.next[c];
  } while (c != c0 && guard++ < MESH_FACE_VS_LIMIT);
  return true;
}

/** CDT a complex face (holes / non-convex) into `tris`. Walks every loop into 2D
 * points + closed constraint rings, runs constrainedDelaunay2D, maps each output
 * index triple back to the originating (vert, corner). Returns false (leaving
 * `tris` for the caller to truncate) on failure. */
template <int VecStaticSize>
static bool cdtTriangulateFace(Mesh &m, int f, const float3 &ub, const float3 &vb,
                               litestl::util::Vector<Tri, VecStaticSize> &tris)
{
  using namespace litestl;
  util::Vector<float2> pts;
  util::Vector<detail_delaunay::DEdge> segs;
  util::Vector<int> cornerOf, vertOf; // pts index -> source corner / vert

  for (int li = m.f.l[f]; li != ELEM_NONE; li = m.l.next[li]) {
    int base = int(pts.size());
    int c0 = m.l.c[li], c = c0, k = 0, guard = 0;
    do {
      int v = m.c.v[c];
      float3 co = m.v.co[v];
      pts.append(float2(co.dot(ub), co.dot(vb)));
      cornerOf.append(c);
      vertOf.append(v);
      c = m.c.next[c];
      k++;
    } while (c != c0 && guard++ < MESH_FACE_VS_LIMIT);
    for (int i = 0; i < k; i++) {
      segs.append({base + i, base + (i + 1) % k});
    }
  }

  util::Vector<int> idx;
  auto ok = constrainedDelaunay2D(
      util::span<const float2>(pts.data(), pts.size()),
      util::span<const detail_delaunay::DEdge>(segs.data(), segs.size()), idx, true);
  if (!ok || idx.size() < 3) return false;

  for (int i = 0; i + 2 < int(idx.size()); i += 3) {
    int a = idx[i], b = idx[i + 1], c = idx[i + 2];
    Tri t;
    t.v[0] = vertOf[a];
    t.v[1] = vertOf[b];
    t.v[2] = vertOf[c];
    t.c[0] = cornerOf[a];
    t.c[1] = cornerOf[b];
    t.c[2] = cornerOf[c];
    t.f = f;
    tris.append(t);
  }
  return true;
}
} // namespace detail_triangulate

using litestl::util::SuccessOrError;

template <int VecStaticSize>
static SuccessOrError<"triangulate", "failed to triangulate faces">
triangulateFace(Mesh &m, int f, litestl::util::Vector<Tri, VecStaticSize> &tris)
{
  using litestl::math::float3;

  int li = m.f.l[f];

  // Projection plane, computed lazily (only complex / n-gon faces need it).
  float3 ub, vb;
  bool planeReady = false;
  auto ensurePlane = [&]() {
    if (planeReady) return;
    litestl::util::Vector<float3, 16> outer3;
    int c0 = m.l.c[li], c = c0, guard = 0;
    do {
      outer3.append(m.v.co[m.c.v[c]]);
      c = m.c.next[c];
    } while (c != c0 && guard++ < MESH_FACE_VS_LIMIT);
    float3 nrm = detail_delaunay::fitPlaneNormal(
        litestl::util::span<const float3>(outer3.data(), outer3.size()));
    if (nrm.length() < 1e-6f) {
      nrm = float3(0.0f, 0.0f, 1.0f);
    } else {
      nrm.normalize();
    }
    detail_delaunay::planeBasis(nrm, ub, vb);
    planeReady = true;
  };

  // Complex = has holes, or a non-convex single n-gon. Triangles and convex
  // n-gons take the cheap fan (exact, and byte-identical to the old output).
  bool complex;
  if (m.f.list_count[f] != 1) {
    complex = true;
  } else if (m.l.size[li] <= 3) {
    complex = false;
  } else {
    ensurePlane();
    complex = !detail_triangulate::loopIsConvex2D(m, li, ub, vb);
  }

  if (complex) {
    ensurePlane();
    size_t before = tris.size();
    if (detail_triangulate::cdtTriangulateFace(m, f, ub, vb, tris) &&
        tris.size() > before) {
      return true;
    }
    while (tris.size() > before) tris.pop_back(); // CDT failed -> fan fallback
  }

  // Fan fast path (and the complex-face fallback): fan from the first corner.
  auto &cv = m.c.v;
  auto &cnext = m.c.next;
  int first = m.l.c[li];
  int last = m.c.prev[first];
  int vfirst = cv[first];

  int ci = cnext[first];
  int _i = 0;
  do {
    if (_i++ > MESH_FACE_VS_LIMIT) {
      printf("mesh error\n");
      break;
    }
    tris.append({{vfirst, cv[ci], cv[cnext[ci]]}, //
                 {first, ci, cnext[ci]},
                 f});
    ci = cnext[ci];
  } while (ci != last);

  return true;
}

/* Triangulate every face of the mesh in place: each n-gon (single outer loop)
 * is replaced by a triangle fan from its first corner via the public Euler
 * operators. Triangles and the unsupported multi-loop faces are left alone.
 * Used to feed triangle-only consumers (dyntopo) from quad generators. Delegates
 * to triangulateFaceFanCb so the face's attrs (poly `group`, …) and per-corner
 * attrs (`uv`, …) are carried onto every fan triangle — same fan topology, so
 * geometry is byte-identical to a plain make_face fan (attr-free meshes no-op
 * the snapshot/restore). */
static inline SuccessOrError<"triangulate", "failed to triangulate faces">
triangulateMesh(Mesh &m)
{
  using namespace litestl::util;

  Vector<int> faces;
  for (int f : m.f) {
    faces.append(f);
  }
  for (int f : faces) {
    triangulateFaceFanCb(m, f, nullptr);
  }
  return true;
}

template <std::ranges::range FaceIndexRange, int VecStaticSize>
  requires std::same_as<std::ranges::range_value_t<FaceIndexRange>, int>
static SuccessOrError<"triangulate", "failed to triangulate faces"> triangulate(
    Mesh &m, FaceIndexRange range, litestl::util::Vector<Tri, VecStaticSize> &tris)
{
  using namespace litestl::util;

  for (int f : range) {
    if (!triangulateFace(m, f, tris)) {
      return false;
    }
  }

  printf("%.2fk triangles\n", float(tris.size()) / 1000.0f);
  return true;
}
} // namespace sculptcore::mesh