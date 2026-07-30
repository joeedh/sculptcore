#include "attr_merge.h"

#include "deform_pool.h"
#include "mesh.h"
#include "sculpt_layers.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace sculptcore::mesh {

/* Layer names owned by other modules. They cannot be included from here (mesh
 * does not depend on brush), so they are spelled out; the owners are
 * brush/brush_executor.h and brush/enhance.h. */
static constexpr const char *ORIG_NO_ATTR = ".brush.orig.no";
static constexpr const char *DISP_VEC_ATTR = ".brush.disp.vec";
static constexpr const char *DISP_GEN_ATTR = ".brush.disp.gen";
static constexpr const char *DAB_GEN_ATTR = ".brush.dab.gen";
static constexpr const char *CAVITY_ATTR = ".brush.automask.cavity";
static constexpr const char *CAVITY_GEN_ATTR = ".brush.automask.gen";
static constexpr const char *ENHANCE_DISP_ATTR = ".brush.enhance.disp";
static constexpr const char *ENHANCE_GEN_ATTR = ".brush.enhance.gen";

AttrDataBase *siblingLayer(AttrRef &attr,
                           const AttrMergeCtx &ctx,
                           AttrType type,
                           const char *name)
{
  if (!ctx.grp) {
    return nullptr;
  }
  auto &layers = ctx.grp->attrs;
  if (attr.merge_aux >= 0 && attr.merge_aux < int(layers.size())) {
    AttrRef &cached = layers[attr.merge_aux];
    if (cached.type == type && std::strcmp(cached.name.c_str(), name) == 0) {
      return cached.data;
    }
  }
  for (int i = 0; i < int(layers.size()); i++) {
    if (layers[i].type == type && std::strcmp(layers[i].name.c_str(), name) == 0) {
      attr.merge_aux = i;
      return layers[i].data;
    }
  }
  attr.merge_aux = -1;
  return nullptr;
}

void defaultMerge(AttrRef &attr, const AttrMergeCtx &ctx, bool copy_src0)
{
  if (attr.type == AttrType::BOOL) {
    BoolAttrView *view = static_cast<BoolAttrView *>(attr.data);
    if (view) {
      view->set(ctx.dst, (*view)[ctx.src0]);
    }
    return;
  }

  detail::type_dispatch(attr.type, [&]<typename T>() {
    if constexpr (std::is_same_v<T, bool>) {
      return;
    } else {
      auto *data = static_cast<AttrData<T> *>(attr.data);
      if (!data) {
        return;
      }
      // Lazily-paged attrs (.brush.disp.*): sources may sit in unmaterialized
      // pages (read as the page default) and dst's page may not exist yet.
      T a = data->safe_get(ctx.src0);
      data->materialize(ctx.dst);
      if (copy_src0) {
        (*data)[ctx.dst] = a;
        return;
      }
      T b = data->safe_get(ctx.src1);
      if constexpr (std::is_floating_point_v<T>) {
        (*data)[ctx.dst] = a * (T(1) - T(ctx.t)) + b * T(ctx.t);
      } else if constexpr (requires { typename T::value_type; }) {
        using Scalar = typename T::value_type;
        if constexpr (std::is_floating_point_v<Scalar>) {
          (*data)[ctx.dst] = a * Scalar(1.0f - ctx.t) + b * Scalar(ctx.t);
        } else {
          (*data)[ctx.dst] = a; /* integer vector: copy */
        }
      } else {
        (*data)[ctx.dst] = a; /* int / byte / short: copy */
      }
    }
  });
}

namespace {

/* `.brush.orig.no`: the stroke-start normal snapshot, stamped alongside the
 * displacement field and sharing its `.brush.disp.gen` key, with lazily
 * materialized pages (only brushed verts have one). Blending it like ordinary
 * data mixes an unstamped endpoint's page default into the result and then
 * inherits the *other* endpoint's stamp, which reads as a valid snapshot of a
 * normal the surface never had.
 *
 * The stroke-start surface interpolates exactly like the live one, so:
 *   both stamped -> lerp the snapshots
 *   one stamped  -> lerp it against the other endpoint's LIVE normal (an
 *                   unstamped vert has not moved this stroke, so live IS its
 *                   stroke-start value)
 *   neither      -> clear the stamp; consumers fall back to live. */
void mergeOrigNormal(AttrRef &attr, const AttrMergeCtx &ctx)
{
  auto *val = static_cast<AttrData<math::float3> *>(attr.data);
  auto *gen =
      static_cast<AttrData<int> *>(siblingLayer(attr, ctx, AttrType::INT, DISP_GEN_ATTR));
  if (!val || !gen || !ctx.have_live) {
    defaultMerge(attr, ctx);
    return;
  }

  int g0 = gen->safe_get(ctx.src0);
  int g1 = gen->safe_get(ctx.src1);
  if (g0 == 0 && g1 == 0) {
    return;
  }

  math::float3 a = g0 != 0 ? val->safe_get(ctx.src0) : ctx.src_no[0];
  math::float3 b = g1 != 0 ? val->safe_get(ctx.src1) : ctx.src_no[1];

  math::float3 out = a * (1.0f - ctx.t) + b * ctx.t;
  out.normalize();
  val->materialize(ctx.dst);
  (*val)[ctx.dst] = out;
}

/* `.brush.disp.vec`: this stroke's accumulated brush displacement, valid only
 * where `.brush.disp.gen` names the current stroke. Same lazy-page hazard as the
 * normal snapshot, but the unstamped value is known — a vert the stroke has not
 * touched has displaced by zero — so no live position is needed:
 *   both stamped -> lerp the displacements
 *   one stamped  -> lerp it against zero
 *   neither      -> clear the stamp
 * Inheriting a nonzero gen alongside a nonzero displacement is what keeps
 * `base = co - disp` continuous across a split. */
void mergeDispField(AttrRef &attr, const AttrMergeCtx &ctx)
{
  auto *val = static_cast<AttrData<math::float3> *>(attr.data);
  auto *gen =
      static_cast<AttrData<int> *>(siblingLayer(attr, ctx, AttrType::INT, DISP_GEN_ATTR));
  if (!val || !gen) {
    defaultMerge(attr, ctx);
    return;
  }

  int g0 = gen->safe_get(ctx.src0);
  int g1 = gen->safe_get(ctx.src1);
  gen->materialize(ctx.dst);
  if (g0 == 0 && g1 == 0) {
    (*gen)[ctx.dst] = 0;
    return;
  }

  math::float3 a = g0 != 0 ? val->safe_get(ctx.src0) : math::float3(0.0f, 0.0f, 0.0f);
  math::float3 b = g1 != 0 ? val->safe_get(ctx.src1) : math::float3(0.0f, 0.0f, 0.0f);

  val->materialize(ctx.dst);
  (*val)[ctx.dst] = a * (1.0f - ctx.t) + b * ctx.t;
  (*gen)[ctx.dst] = g0 != 0 ? g0 : g1;
}

/* Per-stroke caches whose value depends on the local topology (cavity BFS,
 * enhance band-pass) or that must re-stamp on first touch (grab's per-dab
 * counter): a merged element's cached value is stale by construction, so drop
 * the stamp and let the next dab recompute it. */
void mergeClearGen(AttrRef &attr, const AttrMergeCtx &ctx)
{
  auto *gen = static_cast<AttrData<int> *>(attr.data);
  if (!gen) {
    return;
  }
  gen->materialize(ctx.dst);
  (*gen)[ctx.dst] = 0;
}

/* `.slayer.rest`: the pre-layer rest position, whose only meaning is the delta
 * `co - rest` the active sculpt layer stores. Under a plain lerp that identity
 * holds exactly *because* dst's position is the same lerp of the sources — true
 * for a split, false for a collapse, which places the survivor wherever it
 * likes. Reconstruct rest from the merged position and the merged delta so the
 * layer's contribution is preserved rather than silently re-scaled. */
void mergeSculptLayerRest(AttrRef &attr, const AttrMergeCtx &ctx)
{
  auto *rest = static_cast<AttrData<math::float3> *>(attr.data);
  if (!rest || !ctx.merged_co || !ctx.have_live) {
    defaultMerge(attr, ctx);
    return;
  }

  math::float3 d0 = ctx.src_co[0] - rest->safe_get(ctx.src0);
  math::float3 d1 = ctx.src_co[1] - rest->safe_get(ctx.src1);
  math::float3 d = d0 * (1.0f - ctx.t) + d1 * ctx.t;

  rest->materialize(ctx.dst);
  (*rest)[ctx.dst] = *ctx.merged_co - d;
}

/** Below this, an entry is noise rather than influence. Interpolation is
 * transitive — a dyntopo region resplit a hundred times feeds every merge its
 * own output — so without a floor a run accumulates groups it will never lose,
 * one denormal at a time, until it hits DEFORM_MAX_INFLUENCES and starts
 * evicting real influences. Blender's own vertex-group cleanup uses the same
 * idea; the value is the smallest weight a 16-bit UI slider can express. */
static constexpr float WEIGHT_MERGE_EPSILON = 1e-5f;

/** The generic rule's plain copy would install src0's slot index in dst without
 * taking a reference, and the pool would then reclaim a run the column still
 * names. This is also the engine's first merge handler that allocates: intern()
 * takes a shard lock, which is why the pool is thread-safe from the start. */
void mergeWeights(AttrRef &attr, const AttrMergeCtx &ctx)
{
  auto *data = static_cast<AttrData<WeightSlot> *>(attr.data);
  DeformPool *pool = ctx.grp ? ctx.grp->deform_pool : nullptr;
  if (!data || !pool) {
    return;
  }

  const WeightSlot s0 = data->safe_get(ctx.src0);
  const WeightSlot s1 = data->safe_get(ctx.src1);

  // Endpoint cases — a collapse, or either source landing exactly on dst. The
  // answer is already an interned run, so take it whole and skip the intern.
  if (s0.index == s1.index || ctx.t <= 0.0f) {
    data->materialize(ctx.dst);
    pool->reassign((*data)[ctx.dst], s0);
    return;
  }
  if (ctx.t >= 1.0f) {
    data->materialize(ctx.dst);
    pool->reassign((*data)[ctx.dst], s1);
    return;
  }

  DeformWeight a[DEFORM_MAX_INFLUENCES], b[DEFORM_MAX_INFLUENCES];
  const int na = std::min(pool->copyRun(s0, a, DEFORM_MAX_INFLUENCES), DEFORM_MAX_INFLUENCES);
  const int nb = std::min(pool->copyRun(s1, b, DEFORM_MAX_INFLUENCES), DEFORM_MAX_INFLUENCES);

  const float t = ctx.t;
  util::Vector<DeformWeight, DEFORM_MAX_INFLUENCES * 2> out;
  auto push = [&](int group, float w0, float w1) {
    const float w = w0 * (1.0f - t) + w1 * t;
    if (w > WEIGHT_MERGE_EPSILON || w < -WEIGHT_MERGE_EPSILON) {
      out.append(DeformWeight{group, w});
    }
  };

  // Both runs are canonicalized group-ascending, so the union is one walk. A
  // group absent from a side weighs 0 there — not "unchanged".
  int i = 0, j = 0;
  while (i < na && j < nb) {
    if (a[i].group == b[j].group) {
      push(a[i].group, a[i].weight, b[j].weight);
      i++;
      j++;
    } else if (a[i].group < b[j].group) {
      push(a[i].group, a[i].weight, 0.0f);
      i++;
    } else {
      push(b[j].group, 0.0f, b[j].weight);
      j++;
    }
  }
  for (; i < na; i++) {
    push(a[i].group, a[i].weight, 0.0f);
  }
  for (; j < nb; j++) {
    push(b[j].group, 0.0f, b[j].weight);
  }

  if (out.size() > DEFORM_MAX_INFLUENCES) {
    // Keep the strongest influences. intern() re-sorts by group, so leaving the
    // survivors in magnitude order is fine. Magnitude, not value: weights are
    // not required to be positive.
    out.sort([](const DeformWeight &x, const DeformWeight &y) {
      const float ax = std::fabs(x.weight), ay = std::fabs(y.weight);
      return ax < ay ? 1 : (ax > ay ? -1 : 0);
    });
    out.resize(DEFORM_MAX_INFLUENCES);
  }

  // Deliberately not normalized: Blender does not, and a sculpt op silently
  // renormalizing a rigged mesh would be a worse bug than the one this fixes.
  WeightSlot merged = pool->intern(util::span<const DeformWeight>(out.data(), out.size()));
  data->materialize(ctx.dst);
  pool->reassign((*data)[ctx.dst], merged);
  pool->release(merged);
}

struct BuiltinPolicy {
  AttrType type;
  const char *name;
  AttrMerge merge;
  AttrMergeFn fn;
};

const BuiltinPolicy builtin_policies[] = {
    {AttrType::FLOAT3, ORIG_NO_ATTR, AttrMerge::CUSTOM, mergeOrigNormal},
    {AttrType::FLOAT3, DISP_VEC_ATTR, AttrMerge::CUSTOM, mergeDispField},
    /* Written by mergeDispField together with the gen it shares with
     * mergeOrigNormal. */
    {AttrType::INT, DISP_GEN_ATTR, AttrMerge::NONE, nullptr},
    {AttrType::INT, DAB_GEN_ATTR, AttrMerge::CUSTOM, mergeClearGen},
    {AttrType::FLOAT, CAVITY_ATTR, AttrMerge::NONE, nullptr},
    {AttrType::INT, CAVITY_GEN_ATTR, AttrMerge::CUSTOM, mergeClearGen},
    {AttrType::FLOAT3, ENHANCE_DISP_ATTR, AttrMerge::NONE, nullptr},
    {AttrType::INT, ENHANCE_GEN_ATTR, AttrMerge::CUSTOM, mergeClearGen},
    {AttrType::FLOAT3, SCULPT_LAYER_REST_ATTR, AttrMerge::CUSTOM, mergeSculptLayerRest},
};

} // namespace

AttrMergePolicy resolveMergePolicy(AttrType type, const string &name)
{
  // Keyed by type, not name: a WEIGHTS column is refcounted whatever it is
  // called, and the user-facing vertex-group layer has no dot prefix.
  if (type == AttrType::WEIGHTS) {
    return {AttrMerge::CUSTOM, mergeWeights};
  }

  /* Every builtin policy is on a dot-prefixed internal layer, so user layers
   * (and the unprefixed builtins: positions, normals, uvs, …) never pay for the
   * scan. */
  if (name.size() == 0 || name[0] != '.') {
    return {};
  }
  for (const BuiltinPolicy &p : builtin_policies) {
    if (p.type == type && std::strcmp(name.c_str(), p.name) == 0) {
      return {p.merge, p.fn};
    }
  }
  return {};
}

} // namespace sculptcore::mesh
