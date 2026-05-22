
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
void SpatialTree::fill_leaf_slice(SpatialNode *leaf, float3 *pos, float3 *nor)
{
  const bool smooth_shading = false;
  Mesh *m = this->m;
  auto &tris = leaf->data->tris;

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
    }
  }
}

/* Full rebuild of a GPU node's aggregated buffer. Disposes any existing
 * pos/nor (but keeps cmd — the caller's draw-batch loop reuses it),
 * collects subtree leaves, sizes one buffer covering all their tris, and
 * fills it slice by slice. */
void SpatialTree::regen_gpu_node(SpatialNode *gpu_node, gpu::GPUManager *gpu)
{
  printf("regen gpu node data\n");
  
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
  gd.pos->update_buffer = true;
  gd.nor->update_buffer = true;

  float3 *pos = gd.pos->get_data<float3>();
  float3 *nor = gd.nor->get_data<float3>();

  int offset = 0;
  for (SpatialNode *leaf : leaves_v) {
    int vcount = leaf->data->tris.size() * 3;
    LeafSlice &slice = gd.slices.grow_one();
    slice.leaf = leaf;
    slice.vert_start = offset;
    slice.vert_count = vcount;

    if (vcount > 0) {
      fill_leaf_slice(leaf, pos + offset, nor + offset);
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
    fill_leaf_slice(leaf, pos, nor);
    gd.pos->update_buffer = true;
    gd.nor->update_buffer = true;
  }

  leaf->flag &= ~Spatial_UpdateGPU;
}

} // namespace sculptcore::spatial
