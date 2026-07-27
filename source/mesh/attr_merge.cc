#include "attr_merge.h"

#include "mesh.h"
#include "sculpt_layers.h"

#include <cstring>
#include <type_traits>

namespace sculptcore::mesh {

/* Layer names owned by other modules. They cannot be included from here (mesh
 * does not depend on brush), so they are spelled out; the owners are
 * brush/brush_executor.h and brush/enhance.h. */
static constexpr const char *ORIG_CO_ATTR = ".brush.orig.co";
static constexpr const char *ORIG_NO_ATTR = ".brush.orig.no";
static constexpr const char *ORIG_GEN_ATTR = ".brush.orig.gen";
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
      // Lazily-paged attrs (.brush.orig.*): sources may sit in unmaterialized
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

/* `.brush.orig.co` / `.brush.orig.no`: a stroke-start snapshot that is only
 * valid where `.brush.orig.gen` names the current stroke, and whose pages are
 * materialized lazily (only brushed verts have one). Blending it like ordinary
 * data mixes an unstamped endpoint's page default into the result and then
 * inherits the *other* endpoint's stamp, which reads as a valid snapshot of a
 * position the surface never had.
 *
 * The stroke-start surface interpolates exactly like the live one, so:
 *   both stamped -> lerp the snapshots
 *   one stamped  -> lerp it against the other endpoint's LIVE value (an
 *                   unstamped vert has not moved this stroke, so live IS its
 *                   stroke-start value)
 *   neither      -> clear the stamp; consumers fall back to live. */
void mergeOrigSnapshot(AttrRef &attr, const AttrMergeCtx &ctx)
{
  auto *val = static_cast<AttrData<math::float3> *>(attr.data);
  auto *gen =
      static_cast<AttrData<int> *>(siblingLayer(attr, ctx, AttrType::INT, ORIG_GEN_ATTR));
  if (!val || !gen || !ctx.have_live) {
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

  const bool is_normal = std::strcmp(attr.name.c_str(), ORIG_NO_ATTR) == 0;
  const math::float3 *live = is_normal ? ctx.src_no : ctx.src_co;
  math::float3 a = g0 != 0 ? val->safe_get(ctx.src0) : live[0];
  math::float3 b = g1 != 0 ? val->safe_get(ctx.src1) : live[1];

  math::float3 out = a * (1.0f - ctx.t) + b * ctx.t;
  if (is_normal) {
    out.normalize();
  }
  val->materialize(ctx.dst);
  (*val)[ctx.dst] = out;
  (*gen)[ctx.dst] = g0 != 0 ? g0 : g1;
}

/* `.brush.disp.vec`: this stroke's accumulated brush displacement, valid only
 * where `.brush.disp.gen` names the current stroke. Same lazy-page hazard as the
 * orig snapshot, but the unstamped value is known — a vert the stroke has not
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

struct BuiltinPolicy {
  AttrType type;
  const char *name;
  AttrMerge merge;
  AttrMergeFn fn;
};

const BuiltinPolicy builtin_policies[] = {
    {AttrType::FLOAT3, ORIG_CO_ATTR, AttrMerge::CUSTOM, mergeOrigSnapshot},
    {AttrType::FLOAT3, ORIG_NO_ATTR, AttrMerge::CUSTOM, mergeOrigSnapshot},
    /* Written by mergeOrigSnapshot together with the value it guards. */
    {AttrType::INT, ORIG_GEN_ATTR, AttrMerge::NONE, nullptr},
    {AttrType::FLOAT3, DISP_VEC_ATTR, AttrMerge::CUSTOM, mergeDispField},
    /* Written by mergeDispField together with the value it guards. */
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
