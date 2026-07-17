/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "spatial/c-api/external_draw.h"

#include "gpu/manager.h"
#include "gpu/vbo.h"
#include "spatial/node.h"
#include "spatial/spatial.h"

#include "litestl/util/map.h"
#include "litestl/util/vector.h"

using namespace sculptcore;

namespace {

/* One shared frontend GPU manager. It only allocates CPU-backed Buffers (no
 * backend is initialized inside Blender); each buffer is owned by the tree's
 * GpuData that produced it and self-removes from the manager on free, so a
 * single manager across all trees is safe. */
gpu::GPUManager &shared_gpu()
{
  static gpu::GPUManager gpu;
  return gpu;
}

/* Object key (Blender ID.session_uid) -> borrowed SpatialTree. */
litestl::util::Map<uint32_t, spatial::SpatialTree *> &registry()
{
  static litestl::util::Map<uint32_t, spatial::SpatialTree *> map;
  return map;
}

/* Scratch node array returned by nodes_get; valid until the next nodes_get. The
 * draw sync consumes one object's nodes fully (uploads their data) before the
 * next call, so a single buffer suffices. */
litestl::util::Vector<ScExternalDrawNode> &scratch()
{
  static litestl::util::Vector<ScExternalDrawNode> nodes;
  return nodes;
}

int extdraw_nodes_get(void * /*user_data*/,
                      unsigned int object_key,
                      const ScExternalDrawAttrRequest * /*req*/,
                      ScExternalDrawNode **r_nodes)
{
  spatial::SpatialTree **tree_ptr = registry().lookup_ptr(object_key);
  if (tree_ptr == nullptr || *tree_ptr == nullptr) {
    return 0;
  }
  spatial::SpatialTree &tree = **tree_ptr;

  litestl::util::Vector<ScExternalDrawNode> &out = scratch();
  out.clear();

  for (spatial::SpatialNode *node : tree.gpu_nodes()) {
    if (node == nullptr || node->gpu_data == nullptr || node->gpu_data->pos == nullptr) {
      continue;
    }
    spatial::GpuData &gd = *node->gpu_data;
    if (gd.total_verts == 0 || gd.pos->data == nullptr) {
      continue;
    }

    ScExternalDrawNode dn = {};
    dn.positions = static_cast<const float(*)[3]>(gd.pos->data);
    dn.normals = (gd.nor && gd.nor->data) ? static_cast<const float(*)[3]>(gd.nor->data) :
                                            nullptr;
    dn.attrs = nullptr;
    dn.verts_num = gd.total_verts;
    dn.material_index = 0;
    /* Positions dirty since Blender last consumed this node → re-upload. The
     * vertex-count change (topology) is caught Blender-side by comparing to the
     * cached size, so DATA covers both. Cleared below so a redraw without an
     * edit reports NONE and Blender reuses its cached batch (partial update). */
    dn.update_flags = gd.pos->update_buffer ? SC_EXTERNAL_DRAW_UPDATE_DATA :
                                              SC_EXTERNAL_DRAW_UPDATE_NONE;
    dn.bounds_min[0] = node->aabb.min[0];
    dn.bounds_min[1] = node->aabb.min[1];
    dn.bounds_min[2] = node->aabb.min[2];
    dn.bounds_max[0] = node->aabb.max[0];
    dn.bounds_max[1] = node->aabb.max[1];
    dn.bounds_max[2] = node->aabb.max[2];
    out.append(dn);

    gd.pos->update_buffer = false;
    if (gd.nor) {
      gd.nor->update_buffer = false;
    }
  }

  *r_nodes = out.data();
  return int(out.size());
}

void extdraw_nodes_release(void * /*user_data*/, unsigned int /*object_key*/) {}

ScExternalDrawProvider g_provider = {
    SC_EXTERNAL_DRAW_ABI_VERSION,
    extdraw_nodes_get,
    extdraw_nodes_release,
    nullptr,
};

}  // namespace

extern "C" {

void sc_external_draw_register(unsigned int object_key, void *spatial_tree)
{
  registry()[object_key] = static_cast<spatial::SpatialTree *>(spatial_tree);
}

void sc_external_draw_unregister(unsigned int object_key)
{
  registry().remove(object_key);
}

void sc_external_draw_update(unsigned int object_key)
{
  spatial::SpatialTree **tree_ptr = registry().lookup_ptr(object_key);
  if (tree_ptr != nullptr && *tree_ptr != nullptr) {
    (*tree_ptr)->update(&shared_gpu());
  }
}

const ScExternalDrawProvider *sc_external_draw_provider(void)
{
  return &g_provider;
}
}
