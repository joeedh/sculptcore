
#include <cassert>

#include "gpu/types.h"
#include "spatial.h"

#include "node.h"

#include "litestl/math/vector.h"
#include "litestl/util/task.h"
#include "litestl/util/vector.h"

#include "gpu/manager.h"
#include "gpu/vbo.h"

using namespace litestl::util;
using namespace litestl::math;
using namespace sculptcore::mesh;
using namespace sculptcore::gpu;
using namespace litestl;

namespace sculptcore::spatial {

void SpatialTree::collect_subtree_leaves(SpatialNode *node,
                                         util::Vector<SpatialNode *> &out)
{
  if (node->flag & Spatial_Leaf) {
    out.append(node);
    return;
  }
  for (int i = 0; i < 2; i++) {
    if (node->children[i]) {
      collect_subtree_leaves(node->children[i], out);
    }
  }
}

/* Map a poly-group id to a stable, visually-distinct color. Group 0 (the
 * default/unassigned id) stays neutral grey so unpainted faces read as
 * "no group". */
static float4 polyGroupColor(int group)
{
  if (group == 0) {
    return float4(0.7f, 0.7f, 0.7f, 1.0f);
  }
  uint32_t h = uint32_t(group) * 2654435761u; /* Knuth multiplicative hash */
  float r = float((h >> 0) & 0xFFu) / 255.0f;
  float g = float((h >> 8) & 0xFFu) / 255.0f;
  float b = float((h >> 16) & 0xFFu) / 255.0f;
  /* Bias toward brighter colors so adjacent groups are easy to tell apart. */
  return float4(0.25f + 0.7f * r, 0.25f + 0.7f * g, 0.25f + 0.7f * b, 1.0f);
}

/* Write one leaf's tris into the given pos/nor/col pointers (already offset to
 * the leaf's slice). Mirrors the per-tri body of the old per-leaf gpu
 * buffer fill. The color source depends on displayColorMode, a bitmask:
 *   bit 0 (1) = vertex `color` attr, bit 1 (2) = per-face `group` id (hashed).
 * Both bits set composites them (painted color modulated by the group color).
 * Neither set renders plain white. */
void SpatialTree::fill_leaf_slice(SpatialNode *leaf, float3 *pos, float3 *nor, float4 *col)
{
  const bool smooth_shading = false;
  Mesh *m = this->m;
  auto &tris = leaf->data->tris;

  const bool show_vcol = (displayColorMode & 1) != 0;
  const bool show_group = (displayColorMode & 2) != 0;

  /* Resolve the color source(s) for the active display mode. Both layers are
   * created lazily by the paint brushes; until then vertex color defaults to
   * white and group to grey (group 0). Once a layer exists the executor has
   * materialized it mesh-wide, so operator[] is safe for any element. */
  AttrData<float4> *cdata = nullptr;
  AttrData<int> *gdata = nullptr;
  // Sculpt-mask overlay (#20): the mask brush writes the ".spatial.v.mask" FLOAT
  // vert attr; absent until first painted, so guard and treat missing as 0.
  AttrData<float> *mdata = nullptr;
  if (col && displayMask && m->v.attrs.has(AttrType::FLOAT, ".spatial.v.mask")) {
    mdata = m->v.attrs.find_attribute(AttrType::FLOAT, ".spatial.v.mask").get_data<float>();
  }
  if (col) {
    if (show_vcol) {
      // Prefer the active layer index (displayColorAttr); fall back to the
      // layer literally named "color" when unset (-1) or the index is stale /
      // wrong-typed. NOTE: the index is resolved by position only — C++ can't
      // validate it by identity. If a same-typed layer is added/removed/reordered
      // ahead of the active one, this silently targets the wrong layer; the app
      // must re-set displayColorAttr (via _syncDisplayAttrs) after any such edit.
      int ci = displayColorAttr;
      if (ci >= 0 && ci < int(m->v.attrs.attrs.size()) &&
          m->v.attrs.attrs[ci].type == AttrType::FLOAT4) {
        cdata = m->v.attrs.attrs[ci].get_data<float4>();
      } else if (m->v.attrs.has(AttrType::FLOAT4, "color")) {
        cdata = m->v.attrs.find_attribute(AttrType::FLOAT4, "color").get_data<float4>();
      }
    }
    if (show_group) {
      int gi = displayGroupAttr;
      if (gi >= 0 && gi < int(m->f.attrs.attrs.size()) &&
          m->f.attrs.attrs[gi].type == AttrType::INT) {
        gdata = m->f.attrs.attrs[gi].get_data<int>();
      } else if (m->f.attrs.has(AttrType::INT, "group")) {
        gdata = m->f.attrs.find_attribute(AttrType::INT, "group").get_data<int>();
      }
    }
  }

  float3 no;
  for (int i : util::IndexRange(tris.size())) {
    int vert_i = i * 3;
    auto &tri = tris[i];

    if (!smooth_shading) {
      no = m->f.no[tri.f];
    }

    /* Per-face group color (same for all 3 corners of the tri). */
    float4 fgcol;
    if (col && show_group) {
      fgcol = polyGroupColor(gdata ? (*gdata)[tri.f] : 0);
    }

    for (int j = 0; j < 3; j++, vert_i++) {
      int c = tri.c[j];
      int v = m->c.v[c];

      pos[vert_i] = m->v.co[v];
      nor[vert_i] = smooth_shading ? m->v.no[v] : no;
      if (col) {
        // Start from white; layer each enabled source on top.
        float4 out(1.0f, 1.0f, 1.0f, 1.0f);
        if (show_vcol) {
          // The paint brush stores premultiplied RGBA and leaves unpainted
          // verts at (0,0,0,0). Composite over an opaque white base so
          // unpainted reads white (not transparent black).
          float4 cc = cdata ? (*cdata)[v] : float4(1.0f, 1.0f, 1.0f, 1.0f);
          float inv = 1.0f - cc[3];
          out = float4(cc[0] + inv, cc[1] + inv, cc[2] + inv, 1.0f);
        }
        if (show_group) {
          // Modulate by the group color (so both-on shows painted color
          // tinted per group; group-only shows the flat group color).
          out = float4(out[0] * fgcol[0], out[1] * fgcol[1], out[2] * fgcol[2], 1.0f);
        }
        if (mdata) {
          // Sculpt-mask overlay (#20): darken + slightly blue-tint masked verts
          // (mask 0 = unaffected, 1 = fully masked), like Blender's mask display.
          float mk = (*mdata)[v];
          mk = mk < 0.0f ? 0.0f : (mk > 1.0f ? 1.0f : mk);
          const float d = 1.0f - 0.6f * mk;
          out = float4(out[0] * d, out[1] * d, out[2] * (d + 0.12f * mk), 1.0f);
        }
        col[vert_i] = out;
      }
    }
  }
}

/* Generic per-corner gather of one requested attribute into `dst` (req.elemSize
 * floats per render vertex, already offset to the leaf's slice). `src` is the
 * resolved mesh layer (nullptr => default-fill). Domains: CORNER indexes by
 * corner, FACE broadcasts the face value across its 3 corners, VERTEX (default)
 * gathers by the corner's vertex. Only float-family source types are gathered;
 * anything else default-fills (never throws / emits garbage). */
void SpatialTree::fill_leaf_attr(SpatialNode *leaf,
                                 const gpu::RequestedAttr &req,
                                 AttrRef *src,
                                 float *dst)
{
  Mesh *m = this->m;
  auto &tris = leaf->data->tris;
  const int dn = req.elemSize;

  /* Value written to missing/out-of-range channels. */
  float def[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  if (req.defaultKind == gpu::AttrDefaultKind::White) {
    for (int k = 0; k < dn && k < 4; k++) {
      def[k] = 1.0f;
    }
  }

  auto writeDefault = [&]() {
    int vert_i = 0;
    for (int i : util::IndexRange(tris.size())) {
      (void)i;
      for (int j = 0; j < 3; j++, vert_i++) {
        float *o = dst + vert_i * dn;
        for (int k = 0; k < dn; k++) {
          o[k] = def[k];
        }
      }
    }
  };

  if (!src || !src->exists()) {
    writeDefault();
    return;
  }

  bool handled = false;
  mesh::detail::type_dispatch(AttrType(req.srcType), [&]<typename T>() {
    if constexpr (std::is_same_v<T, float> || std::is_same_v<T, math::float2> ||
                  std::is_same_v<T, math::float3> || std::is_same_v<T, math::float4>) {
      AttrData<T> *data = src->get_data<T>();
      const int sn = int(sizeof(T) / sizeof(float));
      int vert_i = 0;
      for (int i : util::IndexRange(tris.size())) {
        auto &tri = tris[i];
        for (int j = 0; j < 3; j++, vert_i++) {
          int idx;
          switch (req.domain) {
          case 4:  idx = tri.c[j];          break; /* CORNER */
          case 16: idx = tri.f;             break; /* FACE */
          default: idx = m->c.v[tri.c[j]];  break; /* VERTEX */
          }
          T val = data->safe_get(idx);
          const float *s = reinterpret_cast<const float *>(&val);
          float *o = dst + vert_i * dn;
          for (int k = 0; k < dn; k++) {
            o[k] = (k < sn) ? s[k] : def[k];
          }
        }
      }
      handled = true;
    }
  });

  if (!handled) {
    writeDefault();
  }
}

/* Write one leaf's per-slot global vertex indices (slot order == fill_leaf_slice
 * pos/nor order) into `out` (already offset to the leaf's slice). */
void SpatialTree::fill_leaf_slot_verts(SpatialNode *leaf, uint32_t *out)
{
  Mesh *m = this->m;
  auto &tris = leaf->data->tris;
  for (int i : util::IndexRange(tris.size())) {
    int vert_i = i * 3;
    auto &tri = tris[i];
    for (int j = 0; j < 3; j++, vert_i++) {
      out[vert_i] = uint32_t(m->c.v[tri.c[j]]);
    }
  }
}

/* Build gd.slotVertex for one GPU node from its (current) slices, in the exact
 * same DFS-leaf / per-tri-corner order regen_gpu_node fills gd.pos/gd.nor — so
 * the scatter pass writes the right vertex into each render-VBO slot. Also
 * flips pos/nor to gpu_storage|gpu_owned for the stroke. */
void SpatialTree::buildGpuNodeSlotVertex(SpatialNode *gpu_node, gpu::GPUManager *gpu)
{
  if (!gpu_node->gpu_data) {
    return;
  }
  GpuData &gd = *gpu_node->gpu_data;

  if (gd.slotVertex) {
    alloc::Delete(gd.slotVertex);
    gd.slotVertex = nullptr;
  }
  if (gd.total_verts <= 0) {
    return;
  }

  gd.slotVertex = gpu->createBuffer(
      litestl::util::string("slotVertex"), GPUType::UINT32, 1, gd.total_verts);
  gd.slotVertex->gpu_storage = true; /* compute-read; host-uploaded by backend */
  gd.slotVertex->update_buffer = true;

  uint32_t *out = gd.slotVertex->get_data<uint32_t>();
  int filled = 0;
  for (LeafSlice &s : gd.slices) {
    if (s.vert_count > 0) {
      fill_leaf_slot_verts(s.leaf, out + s.vert_start);
    }
    filled += s.vert_count;
  }
  assert(filled == gd.total_verts);

  /* pos/nor are produced GPU-side by the scatter pass for the stroke. */
  if (gd.pos) {
    gd.pos->gpu_storage = true;
    gd.pos->gpu_owned = true;
  }
  if (gd.nor) {
    gd.nor->gpu_storage = true;
    gd.nor->gpu_owned = true;
  }
}

void SpatialTree::buildGpuScatterTables(util::Vector<uint32_t> &meta,
                                        util::Vector<uint32_t> &map,
                                        util::Vector<SpatialNode *> *owners,
                                        bool fillMap)
{
  meta.clear();
  map.clear();
  if (owners) {
    owners->clear();
  }
  uint32_t offset = 0;
  for (SpatialNode *gn : gpu_nodes()) {
    if (!gn->gpu_data) {
      continue;
    }
    GpuData &gd = *gn->gpu_data;
    if (!gd.pos || !gd.nor || gd.total_verts <= 0) {
      continue;
    }
    const uint64_t pk = uint64_t(uintptr_t(gd.pos));
    const uint64_t nk = uint64_t(uintptr_t(gd.nor));
    meta.append(uint32_t(pk));
    meta.append(uint32_t(pk >> 32));
    meta.append(uint32_t(nk));
    meta.append(uint32_t(nk >> 32));
    meta.append(offset);
    meta.append(uint32_t(gd.total_verts));
    if (owners) {
      owners->append(gn);
    }
    if (fillMap) {
      map.resize(size_t(offset) + size_t(gd.total_verts));
      uint32_t *out = map.data() + offset;
      for (LeafSlice &s : gd.slices) {
        if (s.vert_count > 0) {
          fill_leaf_slot_verts(s.leaf, out + s.vert_start);
        }
      }
    }
    offset += uint32_t(gd.total_verts);
  }
}

/* Serial planning half of a full GPU-node regen — see the spatial.h doc
 * comment. Allocation, slice-table build, flag clears, and source-attr
 * resolution all happen here (GPUManager is not thread-safe); the per-slice
 * fills are deferred to fill_regen_slice. */
void SpatialTree::plan_regen_gpu_node(SpatialNode *gpu_node,
                                      gpu::GPUManager *gpu,
                                      util::Vector<mesh::AttrRef> &srcRefs)
{
  if (!gpu_node->gpu_data) {
    gpu_node->gpu_data = alloc::New<GpuData>("Spatial GpuData");
  }
  GpuData &gd = *gpu_node->gpu_data;

  /* Pos/nor sizes always derive from current tri counts, so just
   * dispose them. The DrawCommand is recreated in the draw-batch loop if
   * stale (size/start changes), so dispose it too here to be safe. */
  {
    if (gd.pos) {
      alloc::Delete(gd.pos);
      gd.pos = nullptr;
    }
    if (gd.nor) {
      alloc::Delete(gd.nor);
      gd.nor = nullptr;
    }
    for (gpu::Buffer *b : gd.attrBufs) {
      if (b) {
        alloc::Delete(b);
      }
    }
    gd.attrBufs.clear_and_contract();
    if (gd.cmd) {
      alloc::Delete(gd.cmd);
      gd.cmd = nullptr;
    }
    gd.slices.clear_and_contract();
  }

  util::Vector<SpatialNode *> leaves_v;
  int total_verts = 0;
  {
    collect_subtree_leaves(gpu_node, leaves_v);

    for (SpatialNode *leaf : leaves_v) {
      if (leaf->flag & Spatial_RegenTris) {
        regen_node_tris(leaf);
      }
      total_verts += leaf->data->tris.size() * 3;
    }
  }
  gd.total_verts = total_verts;

  gd.pos = gpu->createBuffer(
      litestl::util::string("position"), GPUType::FLOAT32, 3, total_verts);
  gd.nor = gpu->createBuffer(
      litestl::util::string("normal"), GPUType::FLOAT32, 3, total_verts);
  gd.pos->update_buffer = true;
  gd.nor->update_buffer = true;

  /* Gate the dynamic path on the draw shader being ready too, so a
   * setRequestedAttrs() not yet followed by setDrawShader() keeps rendering the
   * legacy color stream (matching basicMeshShader) rather than handing
   * mismatched buffers to it for a frame. setDrawShader() re-flags every leaf. */
  const bool dynamic = requestedAttrs.size() > 0 && drawShaderReady;

  /* Per-attribute vertex buffers. Legacy path (no requested set): a single
   * float4 `color` stream filled by the vcol/poly-group compositor. Dynamic
   * path: one buffer per requested attribute, in slot order. */
  if (!dynamic) {
    gpu::Buffer *color = gpu->createBuffer(
        litestl::util::string("color"), GPUType::FLOAT32, 4, total_verts);
    color->update_buffer = true;
    gd.attrBufs.append(color);
  } else {
    for (const gpu::RequestedAttr &req : requestedAttrs) {
      gpu::Buffer *b = gpu->createBuffer(req.name, req.gpuType, req.elemSize, total_verts);
      b->update_buffer = true;
      gd.attrBufs.append(b);
    }
  }
  gd.builtAttrsVersion = requestedAttrsVersion;

  /* Resolve each requested source layer once for this node (dynamic path). */
  if (dynamic) {
    for (const gpu::RequestedAttr &req : requestedAttrs) {
      AttrGroup *grp = m->attrGroupForDomainFlag(req.domain);
      srcRefs.append(grp ? grp->find_attribute(AttrType(req.srcType), req.name) : AttrRef());
    }
  }

  /* Slice-table build + flag clears (fills deferred to fill_regen_slice). */
  int offset = 0;
  for (SpatialNode *leaf : leaves_v) {
    int vcount = leaf->data->tris.size() * 3;
    LeafSlice &slice = gd.slices.grow_one();
    slice.leaf = leaf;
    slice.vert_start = offset;
    slice.vert_count = vcount;
    offset += vcount;

    /* Leaf's GPU dirty bits are now satisfied. */
    leaf->flag &= ~(Spatial_RegenGPU | Spatial_UpdateGPU | Spatial_UpdateGPUGeom);
  }

  // Buffer identity + corner layout changed: invalidate cached scatter tables.
  gpuLayoutGen++;
}

/* Pure fill of one planned slice — see the spatial.h doc comment. */
void SpatialTree::fill_regen_slice(SpatialNode *gpu_node, int sliceIdx, AttrRef *srcRefs)
{
  GpuData &gd = *gpu_node->gpu_data;
  const LeafSlice &slice = gd.slices[sliceIdx];
  if (slice.vert_count <= 0) {
    return;
  }

  const bool dynamic = requestedAttrs.size() > 0 && drawShaderReady;
  const int offset = slice.vert_start;
  float3 *pos = gd.pos->get_data<float3>() + offset;
  float3 *nor = gd.nor->get_data<float3>() + offset;
  float4 *col = (!dynamic && gd.attrBufs.size() > 0)
                    ? gd.attrBufs[0]->get_data<float4>() + offset
                    : nullptr;

  fill_leaf_slice(slice.leaf, pos, nor, col);
  if (dynamic) {
    for (int ai : util::IndexRange(requestedAttrs.size())) {
      const gpu::RequestedAttr &req = requestedAttrs[ai];
      float *adst = gd.attrBufs[ai]->get_data<float>() + offset * req.elemSize;
      AttrRef &ref = srcRefs[ai];
      fill_leaf_attr(slice.leaf, req, ref.exists() ? &ref : nullptr, adst);
    }
  }
}

/* Full rebuild of a GPU node's aggregated buffer: serial plan + serial
 * slice-by-slice fill. update()'s hot path plans serially and runs the fills
 * in the unified parallel pass instead; this whole-function form serves the
 * fallback sites (failed in-place slice update, gpu-resident stroke sync). */
void SpatialTree::regen_gpu_node(SpatialNode *gpu_node, gpu::GPUManager *gpu)
{
  util::Vector<AttrRef> srcRefs;
  plan_regen_gpu_node(gpu_node, gpu, srcRefs);

  GpuData &gd = *gpu_node->gpu_data;
  for (int si : util::IndexRange(gd.slices.size())) {
    fill_regen_slice(gpu_node, si, srcRefs.size() > 0 ? srcRefs.data() : nullptr);
  }
}

/* In-place rewrite of a single leaf's slice inside its GPU node's buffer
 * (vertex positions/normals only; tri count unchanged). Pure / data-race-free:
 * runs under parallel_for, so it writes only its own disjoint slice and its own
 * leaf flag — it never calls regen_gpu_node (which disposes+reallocates shared
 * GpuData) and never touches the owner-level update_buffer flags. Returns false
 * when an in-place update isn't possible (attr-version/slice/vcount mismatch);
 * the caller then regens the owner serially. */
bool SpatialTree::update_gpu_node_slice(SpatialNode *gpu_node,
                                        SpatialNode *leaf,
                                        gpu::GPUManager *gpu,
                                        int *outVertStart,
                                        int *outVertCount,
                                        bool geomOnly)
{
  (void)gpu;
  GpuData &gd = *gpu_node->gpu_data;

  /* The requested attribute set changed since these buffers were built — their
   * count/layout no longer matches. Needs a full rebuild (done serially). */
  if (gd.builtAttrsVersion != requestedAttrsVersion) {
    return false;
  }

  LeafSlice *slice = nullptr;
  for (LeafSlice &s : gd.slices) {
    if (s.leaf == leaf) {
      slice = &s;
      break;
    }
  }
  /* slice not found (partition drifted) or this leaf's topology changed since
   * the partition was built — either way the owner needs a serial full rebuild. */
  if (!slice) {
    return false;
  }
  int expected_vcount = leaf->data->tris.size() * 3;
  if (expected_vcount != slice->vert_count) {
    return false;
  }

  if (slice->vert_count > 0) {
    const bool dynamic = requestedAttrs.size() > 0 && drawShaderReady;
    float3 *pos = gd.pos->get_data<float3>() + slice->vert_start;
    float3 *nor = gd.nor->get_data<float3>() + slice->vert_start;
    /* geomOnly (pure-deform dirt): the attr streams are current — refill and
     * re-upload pos/nor only. */
    float4 *col = (!geomOnly && !dynamic && gd.attrBufs.size() > 0)
                      ? gd.attrBufs[0]->get_data<float4>() + slice->vert_start
                      : nullptr;
    fill_leaf_slice(leaf, pos, nor, col);
    if (dynamic && !geomOnly) {
      for (int ai : util::IndexRange(requestedAttrs.size())) {
        const gpu::RequestedAttr &req = requestedAttrs[ai];
        AttrGroup *grp = m->attrGroupForDomainFlag(req.domain);
        AttrRef ref = grp ? grp->find_attribute(AttrType(req.srcType), req.name) : AttrRef();
        float *adst = gd.attrBufs[ai]->get_data<float>() + slice->vert_start * req.elemSize;
        fill_leaf_attr(leaf, req, ref.exists() ? &ref : nullptr, adst);
      }
    }
  }

  if (outVertStart) {
    *outVertStart = slice->vert_start;
  }
  if (outVertCount) {
    *outVertCount = slice->vert_count;
  }
  leaf->flag &= ~(Spatial_UpdateGPU | Spatial_UpdateGPUGeom);
  return true;
}

} // namespace sculptcore::spatial
