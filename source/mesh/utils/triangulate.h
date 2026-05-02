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
  // int c[3];
  // int e[3];
};

using litestl::util::SuccessOrError;

template <std::ranges::range FaceIndexRange>
  requires std::same_as<std::ranges::range_value_t<FaceIndexRange>, int>
static SuccessOrError<"triangulate", "failed to triangulate faces">
triangulate(Mesh &m, FaceIndexRange range, litestl::util::Vector<Tri> &tris)
{
  using namespace litestl::util;

  for (int f : range) {
    int l = m.f.l[f];
    for (int i : IndexRange(m.f.list_count[f])) {
      int first = m.l.c[l + i];
      int c = first;
      int _i = 0;
      do {
        if (_i++ > MESH_FACE_VS_LIMIT) {
          break;
        }
        c = m.c.next[c];
      } while (c != first);
    }
  }

  return true;
}
} // namespace sculptcore::mesh