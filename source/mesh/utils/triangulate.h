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