#pragma once

#include "../mesh.h"
#include "../mesh_base.h"
#include "../mesh_proxy.h"
#include "attr_interp.h"
#include "litestl/util/error.h"
#include "litestl/util/index_range.h"
#include "litestl/util/vector.h"
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

using litestl::util::SuccessOrError;

template <int VecStaticSize>
static SuccessOrError<"triangulate", "failed to triangulate faces">
triangulateFace(Mesh &m, int f, litestl::util::Vector<Tri, VecStaticSize> &tris)
{

  auto &cv = m.c.v;
  auto &cnext = m.c.next;

  // printf("f: %d\n", f);
  int li = m.f.l[f];

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
 * Used to feed triangle-only consumers (dyntopo) from quad generators. */
static inline SuccessOrError<"triangulate", "failed to triangulate faces">
triangulateMesh(Mesh &m)
{
  using namespace litestl::util;

  Vector<int> faces;
  for (int f : m.f) {
    faces.append(f);
  }
  for (int f : faces) {
    if (m.f.freemap[f] || m.f.list_count[f] != 1) {
      continue;
    }
    int li = m.f.l[f];
    if (m.l.size[li] == 3) {
      continue;
    }
    Vector<int, 16> vs;
    int c0 = m.l.c[li], cc = c0;
    do {
      vs.append(m.c.v[cc]);
      cc = m.c.next[cc];
    } while (cc != c0);
    m.kill_face(f);
    for (int i = 1; i + 1 < int(vs.size()); i++) {
      int tri[3] = {vs[0], vs[i], vs[i + 1]};
      m.make_face(std::span<int>(tri, 3));
    }
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