/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "spatial/c-api/external_draw.h"

#include "gpu/gpu_attr_request.h"
#include "gpu/manager.h"
#include "gpu/types.h"
#include "gpu/vbo.h"
#include "mesh/attribute_enums.h"
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

/* Per-node attribute-pointer arrays backing ScExternalDrawNode::attrs. v1
 * exposes one attribute (the legacy float4 color stream, attrBufs[0]); reserved
 * to the node count so appends never realloc while node.attrs point into it. */
litestl::util::Vector<const void *> &attr_ptrs()
{
  static litestl::util::Vector<const void *> ptrs;
  return ptrs;
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
  litestl::util::Vector<const void *> &attrs = attr_ptrs();
  out.clear();
  attrs.clear();
  const litestl::util::Vector<spatial::SpatialNode *> gpu_node_list = tree.gpu_nodes();
  /* Attr pointers are stored contiguous per node (one block per node); the
   * blocks must not move while node.attrs point into them, so size the backing
   * store up front to node_count * attrs_per_node. */
  const size_t attrs_per_node = size_t(tree.requestedAttrs.size()) + 1;
  attrs.ensure_capacity(gpu_node_list.size() * attrs_per_node);

  for (spatial::SpatialNode *node : gpu_node_list) {
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
    /* Expose every GPU-node attribute buffer, in slot order (== attrBufs
     * index order, see SpatialTree::setRequestedAttrs). Legacy path: a single
     * composited float4 color stream. Dynamic path: one per requested attr
     * (e.g. color@0, uv@1). Blender reads them by slot. */
    if (gd.attrBufs.size() > 0) {
      const size_t base = attrs.size();
      for (gpu::Buffer *b : gd.attrBufs) {
        attrs.append((b && b->data) ? b->data : nullptr);
      }
      dn.attrs = &attrs[base];
    }
    else {
      dn.attrs = nullptr;
    }
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
    for (gpu::Buffer *b : gd.attrBufs) {
      if (b) {
        b->update_buffer = false;
      }
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

void sc_external_draw_enable_dynamic(void *spatial_tree)
{
  spatial::SpatialTree *tree = static_cast<spatial::SpatialTree *>(spatial_tree);
  if (tree == nullptr) {
    return;
  }
  /* A fixed color@0 (vertex float4) + uv@1 (corner float2) layout, so Blender
   * always addresses attrs by the same slot regardless of which the object
   * actually has (a missing source layer is filled with the default). The
   * per-attribute source is looked up by name in the engine mesh's domain group
   * (see fill_leaf_attr). */
  litestl::util::Vector<gpu::RequestedAttr> reqs;
  gpu::RequestedAttr color;
  color.name = "color";
  color.srcType = int(mesh::AttrType::FLOAT4);
  color.gpuType = gpu::GPUType::FLOAT32;
  color.elemSize = 4;
  color.slot = 0;
  color.domain = 1; /* VERTEX */
  color.defaultKind = gpu::AttrDefaultKind::White;
  reqs.append(color);
  gpu::RequestedAttr uv;
  uv.name = "uv";
  uv.srcType = int(mesh::AttrType::FLOAT2);
  uv.gpuType = gpu::GPUType::FLOAT32;
  uv.elemSize = 2;
  uv.slot = 1;
  uv.domain = 4; /* CORNER */
  uv.defaultKind = gpu::AttrDefaultKind::Zero;
  reqs.append(uv);
  tree->setRequestedAttrs(reqs);
  /* Enable the dynamic fill layout. Blender reads the CPU buffers directly and
   * never invokes the engine's renderer, but the dynamic path builds an engine
   * draw batch against `drawShader`, so it must be a real linked ShaderDef, not
   * just the `drawShaderReady` flag forced on (an unlinked shader → null call).
   * `linkShaderDef` only lays out the uniform blocks — it never parses the WGSL
   * — so a stub source with the correct attr/uniform set links fine headless. */
  tree->setDrawShader(
      "// external-draw stub: the engine renderer is never invoked in Blender.\n");
}

const ScExternalDrawProvider *sc_external_draw_provider(void)
{
  return &g_provider;
}
}
