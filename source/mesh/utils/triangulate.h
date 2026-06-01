#pragma once

#include "../mesh.h"
#include "../mesh_base.h"
#include "../mesh_proxy.h"
#include "litestl/util/error.h"
#include "litestl/util/index_range.h"
#include "litestl/util/vector.h"
#include <ranges>

namespace sculptcore::mesh {
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