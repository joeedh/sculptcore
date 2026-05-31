
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

/* Write one leaf's tris into the given pos/nor pointers (already offset to
 * the leaf's slice). Mirrors the per-tri body of the old per-leaf gpu
 * buffer fill. */
void SpatialTree::fill_leaf_slice(SpatialNode *leaf, float3 *pos, float3 *nor, float4 *col)
{
  const bool smooth_shading = false;
  Mesh *m = this->m;
  auto &tris = leaf->data->tris;

  /* Per-vertex color layer is created lazily by the `color` paint brush; until
   * then every slot defaults to white. Once it exists the executor has
   * materialized it mesh-wide, so operator[] is safe for any vert. */
  AttrData<float4> *cdata = nullptr;
  if (col && m->v.attrs.has(AttrType::FLOAT4, "color")) {
    cdata = m->v.attrs.find_attribute(AttrType::FLOAT4, "color").get_data<float4>();
  }

  float3 no;
  for (int i : util::IndexRange(tris.size())) {
    int vert_i = i * 3;
    auto &tri = tris[i];

    if (!smooth_shading) {
      no = m->f.no[tri.f];
    }

    for (int j = 0; j < 3; j++, vert_i++) {
      int c = tri.c[j];
      int v = m->c.v[c];

      pos[vert_i] = m->v.co[v];
      nor[vert_i] = smooth_shading ? m->v.no[v] : no;
      if (col) {
        col[vert_i] = cdata ? (*cdata)[v] : float4(1.0f, 1.0f, 1.0f, 1.0f);
      }
    }
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

/* Full rebuild of a GPU node's aggregated buffer. Disposes any existing
 * pos/nor (but keeps cmd — the caller's draw-batch loop reuses it),
 * collects subtree leaves, sizes one buffer covering all their tris, and
 * fills it slice by slice. */
void SpatialTree::regen_gpu_node(SpatialNode *gpu_node, gpu::GPUManager *gpu)
{
  if (!gpu_node->gpu_data) {
    gpu_node->gpu_data = alloc::New<GpuData>("Spatial GpuData");
  }
  GpuData &gd = *gpu_node->gpu_data;

  /* Pos/nor sizes always derive from current tri counts, so just
   * dispose them. The DrawCommand is recreated in the draw-batch loop if
   * stale (size/start changes), so dispose it too here to be safe. */
  if (gd.pos) {
    alloc::Delete(gd.pos);
    gd.pos = nullptr;
  }
  if (gd.nor) {
    alloc::Delete(gd.nor);
    gd.nor = nullptr;
  }
  if (gd.color) {
    alloc::Delete(gd.color);
    gd.color = nullptr;
  }
  if (gd.cmd) {
    alloc::Delete(gd.cmd);
    gd.cmd = nullptr;
  }
  gd.slices.clear_and_contract();

  util::Vector<SpatialNode *> leaves_v;
  collect_subtree_leaves(gpu_node, leaves_v);

  int total_verts = 0;
  for (SpatialNode *leaf : leaves_v) {
    if (leaf->flag & Spatial_RegenTris) {
      regen_node_tris(leaf);
    }
    total_verts += leaf->data->tris.size() * 3;
  }
  gd.total_verts = total_verts;

  gd.pos = gpu->createBuffer(
      litestl::util::string("position"), GPUType::FLOAT32, 3, total_verts);
  gd.nor = gpu->createBuffer(
      litestl::util::string("normal"), GPUType::FLOAT32, 3, total_verts);
  gd.color = gpu->createBuffer(
      litestl::util::string("color"), GPUType::FLOAT32, 4, total_verts);
  gd.pos->update_buffer = true;
  gd.nor->update_buffer = true;
  gd.color->update_buffer = true;

  float3 *pos = gd.pos->get_data<float3>();
  float3 *nor = gd.nor->get_data<float3>();
  float4 *col = gd.color->get_data<float4>();

  int offset = 0;
  for (SpatialNode *leaf : leaves_v) {
    int vcount = leaf->data->tris.size() * 3;
    LeafSlice &slice = gd.slices.grow_one();
    slice.leaf = leaf;
    slice.vert_start = offset;
    slice.vert_count = vcount;

    if (vcount > 0) {
      fill_leaf_slice(leaf, pos + offset, nor + offset, col + offset);
    }
    offset += vcount;

    /* Leaf's GPU dirty bits are now satisfied. */
    leaf->flag &= ~(Spatial_RegenGPU | Spatial_UpdateGPU);
  }
}

/* In-place rewrite of a single leaf's slice inside its GPU node's buffer
 * (vertex positions/normals only; tri count unchanged). */
void SpatialTree::update_gpu_node_slice(SpatialNode *gpu_node,
                                        SpatialNode *leaf,
                                        gpu::GPUManager *gpu)
{
  GpuData &gd = *gpu_node->gpu_data;

  LeafSlice *slice = nullptr;
  for (LeafSlice &s : gd.slices) {
    if (s.leaf == leaf) {
      slice = &s;
      break;
    }
  }
  if (!slice) {
    /* Shouldn't happen if assign_gpu_nodes ran, but fall back to full
     * regen rather than corrupting the buffer. */
    regen_gpu_node(gpu_node, gpu);
    return;
  }

  int expected_vcount = leaf->data->tris.size() * 3;
  if (expected_vcount != slice->vert_count) {
    /* Topology of this leaf changed since the partition was built;
     * the whole gpu node needs a full rebuild to recompute offsets. */
    regen_gpu_node(gpu_node, gpu);
    return;
  }

  if (slice->vert_count > 0) {
    float3 *pos = gd.pos->get_data<float3>() + slice->vert_start;
    float3 *nor = gd.nor->get_data<float3>() + slice->vert_start;
    float4 *col = gd.color ? gd.color->get_data<float4>() + slice->vert_start : nullptr;
    fill_leaf_slice(leaf, pos, nor, col);
    gd.pos->update_buffer = true;
    gd.nor->update_buffer = true;
    if (gd.color) {
      gd.color->update_buffer = true;
    }
  }

  leaf->flag &= ~Spatial_UpdateGPU;
}

} // namespace sculptcore::spatial
