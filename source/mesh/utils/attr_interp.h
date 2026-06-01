#pragma once

/* Generic per-element attribute interpolation, shared by the topological
 * operators (edge split, edge collapse, …) that create or merge vertices.
 *
 * `interpAttrs(grp, dst, src0, src1, t)` writes every non-topology layer of
 * `dst` as a blend of `src0`/`src1` with weights (1-t)/t:
 *   - floating-point-backed columns (float, float2/3/4) lerp,
 *   - integer-backed columns (int, int2/3/4, byte, short) copy `src0`
 *     (there is no meaningful blend),
 *   - bool columns copy `src0`,
 *   - TOPO-flagged columns (disk/radial link indices) are NEVER touched —
 *     they are owned by the Euler operators; blending one splices the
 *     element into the wrong cycle and corrupts the mesh.
 */

#include "../attribute.h"

#include <type_traits>

namespace sculptcore::mesh {

static inline void interpAttrs(AttrGroup &grp, int dst, int src0, int src1, float t)
{
  for (AttrRef &attr : grp.attrs) {
    /* Topology links are indices, not interpolable data. */
    if (attr.flag & AttrFlag::TOPO) {
      continue;
    }
    if (attr.type == AttrType::BOOL) {
      BoolAttrView *view = static_cast<BoolAttrView *>(attr.data);
      if (view) {
        view->set(dst, (*view)[src0]);
      }
      continue;
    }

    detail::type_dispatch(attr.type, [&]<typename T>() {
      if constexpr (std::is_same_v<T, bool>) {
        return;
      } else {
        auto *data = static_cast<AttrData<T> *>(attr.data);
        if (!data) {
          return;
        }
        if constexpr (std::is_floating_point_v<T>) {
          (*data)[dst] = (*data)[src0] * (T(1) - T(t)) + (*data)[src1] * T(t);
        } else if constexpr (requires { typename T::value_type; }) {
          using Scalar = typename T::value_type;
          if constexpr (std::is_floating_point_v<Scalar>) {
            (*data)[dst] =
                (*data)[src0] * Scalar(1.0f - t) + (*data)[src1] * Scalar(t);
          } else {
            (*data)[dst] = (*data)[src0]; /* integer vector: copy */
          }
        } else {
          (*data)[dst] = (*data)[src0]; /* int / byte / short: copy */
        }
      }
    });
  }
}

} // namespace sculptcore::mesh
