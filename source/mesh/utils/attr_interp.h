#pragma once

/* Generic per-element attribute interpolation, shared by the topological
 * operators (edge split, edge collapse, …) that create or merge vertices.
 *
 * `interpAttrs(grp, dst, src0, src1, t)` writes every non-topology layer of
 * `dst` as a blend of `src0`/`src1` with weights (1-t)/t:
 *   - floating-point-backed columns (float, float2/3/4) lerp,
 *   - integer-backed columns (int, int2/3/4, byte, short) copy `src0`
 *     (there is no meaningful blend),
 *   - bool columns OR (a feature flag carried by either source survives;
 *     AttrFlag::NOCOPY or AttrMerge::COPY_SRC0 opt back into a src0 copy),
 *   - TOPO-flagged columns (disk/radial link indices) are NEVER touched —
 *     they are owned by the Euler operators; blending one splices the
 *     element into the wrong cycle and corrupts the mesh.
 *
 * That is only the DEFAULT rule; a layer can select another via its AttrMerge
 * policy (mesh/attr_merge.h), which is what the generation-guarded brush
 * snapshot columns and `.slayer.rest` use.
 */

#include "../attr_merge.h"
#include "../attribute.h"
#include "../mesh.h"

#include <cstring>
#include <type_traits>

namespace sculptcore::mesh {

/* A captured copy of one element's non-topology/non-temp attribute values, so a
 * value can survive the kill→recreate cycle that edge split / collapse /
 * triangulate use to rebuild faces (make_face value-inits the new element, so a
 * face's `group`, a corner's `uv`, etc. would otherwise be lost). Cells are kept
 * index-aligned with `grp.attrs` (a placeholder is stored for skipped layers);
 * the group's layer set must not change between snapshot and restore (the topo
 * operators only add/remove elements, never layers, so this holds). */
struct AttrRowSnapshot {
  struct Cell {
    bool present = false;
    bool isBool = false;
    bool bval = false;
    uint8_t bytes[16] = {};
  };
  litestl::util::Vector<Cell, 18> cells;
};

static inline void snapshotAttrRow(AttrGroup &grp, int elem, AttrRowSnapshot &snap)
{
  snap.cells.clear();
  for (AttrRef &attr : grp.attrs) {
    AttrRowSnapshot::Cell cell;
    if (attr.flag & (AttrFlag::TOPO | AttrFlag::NOCOPY)) {
      snap.cells.append(cell); /* placeholder: keep index alignment */
      continue;
    }
    if (attr.type == AttrType::BOOL) {
      BoolAttrView *view = static_cast<BoolAttrView *>(attr.data);
      if (view) {
        cell.isBool = true;
        cell.bval = (*view)[elem];
        cell.present = true;
      }
      snap.cells.append(cell);
      continue;
    }
    detail::type_dispatch(attr.type, [&]<typename T>() {
      if constexpr (std::is_same_v<T, bool>) {
        return;
      } else {
        auto *data = static_cast<AttrData<T> *>(attr.data);
        if (data) {
          static_assert(sizeof(T) <= 16, "attr cell too large for snapshot");
          // safe_get: elem's page may be lazily unmaterialized (.brush.orig.*).
          T v = data->safe_get(elem);
          std::memcpy(cell.bytes, &v, sizeof(T));
          cell.present = true;
        }
      }
    });
    snap.cells.append(cell);
  }
}

static inline void restoreAttrRow(AttrGroup &grp, int elem, const AttrRowSnapshot &snap)
{
  int i = 0;
  for (AttrRef &attr : grp.attrs) {
    if (i >= int(snap.cells.size())) {
      break;
    }
    const AttrRowSnapshot::Cell &cell = snap.cells[i++];
    if (!cell.present) {
      continue;
    }
    if (attr.type == AttrType::BOOL) {
      BoolAttrView *view = static_cast<BoolAttrView *>(attr.data);
      if (view) {
        view->set(elem, cell.bval);
      }
      continue;
    }
    detail::type_dispatch(attr.type, [&]<typename T>() {
      if constexpr (std::is_same_v<T, bool>) {
        return;
      } else {
        auto *data = static_cast<AttrData<T> *>(attr.data);
        if (data) {
          data->materialize(elem); // elem's page may be lazily unmaterialized
          std::memcpy(static_cast<void *>(&(*data)[elem]), cell.bytes, sizeof(T));
        }
      }
    });
  }
}

/* OR a snapshot's bool columns into a live element's (set where the snapshot is
 * true, never clear), leaving every non-bool column untouched. Used by edge
 * collapse's triangle-merge case: when two edges sharing a far endpoint weld into
 * one survivor, the survivor must keep the feature flags (sharp/seam/projected) of
 * *either*, not just the first row restored over it. */
static inline void unionBoolAttrRow(AttrGroup &grp, int elem, const AttrRowSnapshot &snap)
{
  int i = 0;
  for (AttrRef &attr : grp.attrs) {
    if (i >= int(snap.cells.size())) {
      break;
    }
    const AttrRowSnapshot::Cell &cell = snap.cells[i++];
    if (!cell.present || !cell.isBool || attr.type != AttrType::BOOL) {
      continue;
    }
    BoolAttrView *view = static_cast<BoolAttrView *>(attr.data);
    if (view && cell.bval) {
      view->set(elem, true);
    }
  }
}

/* Blend two captured rows into a live element (weights (1-t)/t), matching
 * interpAttrs' rules: float/float-vector lerp, integer/bool copy s0. Used for the
 * midpoint corner of an edge split, whose two sources (the split edge's endpoints
 * on one face) were captured before the face was killed. */
static inline void interpAttrRows(AttrGroup &grp,
                                  int dst,
                                  const AttrRowSnapshot &s0,
                                  const AttrRowSnapshot &s1,
                                  float t)
{
  int i = 0;
  for (AttrRef &attr : grp.attrs) {
    if (i >= int(s0.cells.size())) {
      break;
    }
    const AttrRowSnapshot::Cell &c0 = s0.cells[i];
    const AttrRowSnapshot::Cell &c1 = (i < int(s1.cells.size())) ? s1.cells[i] : c0;
    i++;
    if (!c0.present || attr.merge == AttrMerge::NONE) {
      continue;
    }
    /* Both sources are captured rows, not live elements, so a CUSTOM handler has
     * nothing to inspect here — it falls back to the generic rule. */
    const bool copy_src0 = attr.merge == AttrMerge::COPY_SRC0;
    if (attr.type == AttrType::BOOL) {
      BoolAttrView *view = static_cast<BoolAttrView *>(attr.data);
      if (view) {
        /* OR by default, like defaultMerge: a feature flag carried by either
         * source row survives. COPY_SRC0/NOCOPY keep the plain c0 copy. */
        const bool or_sources = !copy_src0 && !(attr.flag & AttrFlag::NOCOPY);
        view->set(dst, or_sources ? (c0.bval || (c1.present && c1.bval)) : c0.bval);
      }
      continue;
    }
    detail::type_dispatch(attr.type, [&attr, &c0, &c1, &t, &dst, copy_src0]<typename T>() {
      auto *data = static_cast<AttrData<T> *>(attr.data);
      if (!data) {
        return;
      }
      data->materialize(dst); // dst's page may be lazily unmaterialized
      T a;
      std::memcpy(static_cast<void *>(&a), c0.bytes, sizeof(T));
      if (copy_src0) {
        (*data)[dst] = a;
        return;
      }
      if constexpr (std::is_floating_point_v<T>) {
        T b;
        std::memcpy(static_cast<void *>(&b), c1.bytes, sizeof(T));
        (*data)[dst] = a * (T(1) - T(t)) + b * T(t);
      } else if constexpr (requires { typename T::value_type; }) {
        using Scalar = typename T::value_type;
        if constexpr (std::is_floating_point_v<Scalar>) {
          T b;
          std::memcpy(static_cast<void *>(&b), c1.bytes, sizeof(T));
          (*data)[dst] = a * Scalar(1.0f - t) + b * Scalar(t);
        } else {
          (*data)[dst] = a;
        }
      } else {
        (*data)[dst] = a;
      }
    });
  }
}

/**
 * Merge every layer of `dst` from `src0`/`src1` with weights (1-t)/t, each
 * according to its AttrMerge policy (attr_merge.h).
 *
 * `mesh` lets CUSTOM handlers see the sources' live position/normal and must be
 * passed by any caller whose elements belong to it; `merged_co` is dst's final
 * position when the operator places it somewhere other than the lerp of the
 * sources (edge collapse). Omitting either only costs handler fidelity — those
 * layers fall back to the DEFAULT rule.
 */
static inline void interpAttrs(AttrGroup &grp,
                               int dst,
                               int src0,
                               int src1,
                               float t,
                               Mesh *mesh = nullptr,
                               const math::float3 *merged_co = nullptr)
{
  AttrMergeCtx ctx;
  ctx.mesh = mesh;
  ctx.grp = &grp;
  ctx.dst = dst;
  ctx.src0 = src0;
  ctx.src1 = src1;
  ctx.t = t;
  ctx.merged_co = merged_co;
  if (mesh && &grp == &mesh->v.attrs) {
    ctx.src_co[0] = mesh->v.co[src0];
    ctx.src_co[1] = mesh->v.co[src1];
    ctx.src_no[0] = mesh->v.no[src0];
    ctx.src_no[1] = mesh->v.no[src1];
    ctx.have_live = true;
  }

  for (AttrRef &attr : grp.attrs) {
    /* Topology links are indices, not interpolable data; NOINTERP attrs (e.g.
     * .spatial.{v,f}.node) are derived state owned by the spatial tree — copying
     * a parent's node id onto a new vert would mis-attribute it. */
    if (attr.flag & (AttrFlag::TOPO | AttrFlag::NOINTERP)) {
      continue;
    }
    switch (attr.merge) {
    case AttrMerge::NONE:
      continue;
    case AttrMerge::COPY_SRC0:
      defaultMerge(attr, ctx, /*copy_src0=*/true);
      continue;
    case AttrMerge::CUSTOM:
      if (attr.merge_fn) {
        attr.merge_fn(attr, ctx);
        continue;
      }
      break; /* policy declared but no handler installed: use the generic rule */
    case AttrMerge::DEFAULT:
      break;
    }
    defaultMerge(attr, ctx);
  }
}

} // namespace sculptcore::mesh
