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

/* Per-node attribute-pointer arrays backing ScExternalDrawNode::attrs: one
 * fixed-width block per node, reserved up front so appends never realloc while
 * earlier nodes' `attrs` point into it. The width is the *slot count* the host
 * addresses, not the buffer count this tree happens to own — the host reads a
 * slot it asked for whenever the object has that layer, so a short block would
 * be read out of bounds (a legacy tree owns one buffer while Blender still
 * probes uv@1). */
litestl::util::Vector<const void *> &attr_ptrs()
{
  static litestl::util::Vector<const void *> ptrs;
  return ptrs;
}

int extdraw_nodes_get(void * /*user_data*/,
                      unsigned int object_key,
                      const ScExternalDrawAttrRequest *req,
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
  /* Every node gets a block this wide, whether or not it owns that many
   * buffers: the host addresses slots (color@0, uv@1) and probes one whenever
   * the object carries that layer. Missing slots are null-padded below. */
  size_t attrs_per_node = tree.requestedAttrs.size() > 0 ? size_t(tree.requestedAttrs.size()) :
                                                           size_t(1);
  if (req != nullptr && req->attrs_num > 0 && size_t(req->attrs_num) > attrs_per_node) {
    attrs_per_node = size_t(req->attrs_num);
  }
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
    /* Expose this node's attribute buffers in slot order (== attrBufs index
     * order, see SpatialTree::setRequestedAttrs). Legacy path: a single
     * composited float4 color stream at slot 0. Dynamic path: one per requested
     * attr (e.g. color@0, uv@1). Slots this tree has no buffer for are null, so
     * a host probe of a higher slot reads a defined "absent" rather than off
     * the end of the block. */
    const size_t base = attrs.size();
    for (size_t slot = 0; slot < attrs_per_node; slot++) {
      gpu::Buffer *b = slot < size_t(gd.attrBufs.size()) ? gd.attrBufs[slot] : nullptr;
      attrs.append((b && b->data) ? b->data : nullptr);
    }
    dn.attrs = &attrs[base];
    dn.verts_num = gd.total_verts;
    dn.material_index = 0;
    dn.node_id = uint32_t(node->id);
    // DATA: positions dirty since Blender last consumed this node -> re-upload.
    // TOPOLOGY: buffers re-planned (content can change at an identical vertex
    // count) -> realloc. Both cleared below so an editless redraw reports NONE.
    dn.update_flags = SC_EXTERNAL_DRAW_UPDATE_NONE;
    if (gd.pos->update_buffer) {
      dn.update_flags |= SC_EXTERNAL_DRAW_UPDATE_DATA;
    }
    if (gd.extern_topo_dirty) {
      dn.update_flags |= SC_EXTERNAL_DRAW_UPDATE_TOPOLOGY;
    }
    dn.bounds_min[0] = node->aabb.min[0];
    dn.bounds_min[1] = node->aabb.min[1];
    dn.bounds_min[2] = node->aabb.min[2];
    dn.bounds_max[0] = node->aabb.max[0];
    dn.bounds_max[1] = node->aabb.max[1];
    dn.bounds_max[2] = node->aabb.max[2];
    out.append(dn);

    gd.pos->update_buffer = false;
    gd.extern_topo_dirty = false;
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
