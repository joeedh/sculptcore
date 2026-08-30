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

#include "litestl/util/assert.h"
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

/* Object key (Blender ID.session_uid) -> one geometry source: a borrowed
 * SpatialTree (the mesh path) or a registry-owned custom source driven
 * through its ops vtable (the multires grids source, subdiv/c-api). Exactly
 * one is set per entry. */
struct Entry {
  spatial::SpatialTree *tree = nullptr;
  void *custom = nullptr;
  const ScExternalDrawSourceOps *ops = nullptr;
};

litestl::util::Map<uint32_t, Entry> &registry()
{
  static litestl::util::Map<uint32_t, Entry> map;
  return map;
}

void free_entry(Entry &e)
{
  if (e.custom && e.ops && e.ops->destroy) {
    e.ops->destroy(e.custom);
  }
  e.custom = nullptr;
  e.ops = nullptr;
  e.tree = nullptr;
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
  Entry *entry = registry().lookup_ptr(object_key);
  if (entry == nullptr) {
    return 0;
  }
  if (entry->custom != nullptr && entry->ops != nullptr) {
    return entry->ops->nodes_get(entry->custom, req, r_nodes);
  }
  if (entry->tree == nullptr) {
    return 0;
  }
  spatial::SpatialTree &tree = *entry->tree;

  // Per-node material from the bridged "material_index" face attr (absent ->
  // all 0). One material per draw node — the first drawn face's value stands
  // for the node (the host ABI's granularity).
  mesh::AttrData<int> *material_data = nullptr;
  if (tree.m->f.attrs.has(mesh::AttrType::INT, "material_index")) {
    material_data = tree.m->f.attrs.find_attribute(mesh::AttrType::INT, "material_index")
                        .get_data<int>();
  }

  litestl::util::Vector<ScExternalDrawNode> &out = scratch();
  litestl::util::Vector<const void *> &attrs = attr_ptrs();
  out.clear();
  attrs.clear();
  const litestl::util::Vector<spatial::SpatialNode *> gpu_node_list = tree.gpu_nodes();
  /* Every node gets a block this wide, whether or not it owns that many
   * buffers: the host addresses slots (color@0, uv@1) and probes one whenever
   * the object carries that layer. Missing slots are null-padded below. */
  size_t attrs_per_node =
      tree.requestedAttrs.size() > 0 ? size_t(tree.requestedAttrs.size()) : size_t(1);
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
    dn.positions = static_cast<const float (*)[3]>(gd.pos->data);
    dn.normals = (gd.nor && gd.nor->data) ? static_cast<const float (*)[3]>(gd.nor->data)
                                          : nullptr;
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
    if (material_data != nullptr) {
      for (const spatial::LeafSlice &slice : gd.slices) {
        if (slice.leaf != nullptr && slice.leaf->tris().size() > 0) {
          dn.material_index = material_data->safe_get(slice.leaf->tris()[0].f);
          break;
        }
      }
    }
    /* Custom-source ids live above the base; a mesh id crossing into that
     * namespace would let a provider flip alias a stale host batch. The id
     * generator would need ~2^30 allocations to get here — latch loudly. */
    litestl::util::Assert(node->id >= 0 &&
                              uint32_t(node->id) < SC_EXTERNAL_DRAW_CUSTOM_ID_BASE,
                          "SpatialNode id collides with the custom draw-node namespace");
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

void extdraw_nodes_release(void * /*user_data*/, unsigned int /*object_key*/)
{
}

ScExternalDrawProvider g_provider = {
    SC_EXTERNAL_DRAW_ABI_VERSION,
    extdraw_nodes_get,
    extdraw_nodes_release,
    nullptr,
};

} // namespace

extern "C" {

void sc_external_draw_register(unsigned int object_key, void *spatial_tree)
{
  Entry &e = registry()[object_key];
  free_entry(e);
  e.tree = static_cast<spatial::SpatialTree *>(spatial_tree);
}

void sc_external_draw_register_custom(unsigned int object_key,
                                      void *src,
                                      const ScExternalDrawSourceOps *ops)
{
  if (src == nullptr || ops == nullptr || ops->nodes_get == nullptr) {
    return;
  }
  Entry &e = registry()[object_key];
  free_entry(e);
  e.custom = src;
  e.ops = ops;
}

void sc_external_draw_unregister(unsigned int object_key)
{
  Entry *e = registry().lookup_ptr(object_key);
  if (e != nullptr) {
    free_entry(*e);
    registry().remove(object_key);
  }
}

void sc_external_draw_update(unsigned int object_key)
{
  Entry *e = registry().lookup_ptr(object_key);
  if (e == nullptr) {
    return;
  }
  if (e->custom != nullptr && e->ops != nullptr) {
    if (e->ops->update != nullptr) {
      e->ops->update(e->custom);
    }
  } else if (e->tree != nullptr) {
    e->tree->update(&shared_gpu());
  }
}

void sc_external_draw_enable_dynamic(void *spatial_tree)
{
  spatial::SpatialTree *tree = static_cast<spatial::SpatialTree *>(spatial_tree);
  if (tree == nullptr) {
    return;
  }
  /* A fixed color@0 (vertex float4) + uv@1 (corner float2) + mask@2 (vertex
   * float) + fset@3 (white filler float3) layout, so Blender always addresses
   * attrs by the same slot regardless of which the object actually has (a
   * missing source layer is filled with the default). The per-attribute source
   * is looked up by name in the engine mesh's domain group (see
   * fill_leaf_attr). mask@2 feeds Blender's sculpt-mask overlay pass; fset@3
   * resolves no layer on purpose — the overlay shader multiplies by it, so
   * absent face-set data must read white, not zero. */
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
  gpu::RequestedAttr mask;
  mask.name = ".spatial.v.mask";
  mask.srcType = int(mesh::AttrType::FLOAT);
  mask.gpuType = gpu::GPUType::FLOAT32;
  mask.elemSize = 1;
  mask.slot = 2;
  mask.domain = 1; /* VERTEX */
  mask.defaultKind = gpu::AttrDefaultKind::Zero;
  reqs.append(mask);
  gpu::RequestedAttr fset;
  /* Virtual layer: fill_leaf_attr special-cases this name to the hashed
   * per-face `group` colors (white for group 0 / no groups). */
  fset.name = ".extdraw.fset";
  fset.srcType = int(mesh::AttrType::FLOAT3);
  fset.gpuType = gpu::GPUType::FLOAT32;
  fset.elemSize = 3;
  fset.slot = 3;
  fset.domain = 16; /* FACE (informational; the fill indexes itself) */
  fset.defaultKind = gpu::AttrDefaultKind::White;
  reqs.append(fset);
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

void sc_external_draw_set_default_group(void *spatial_tree, int group)
{
  spatial::SpatialTree *tree = static_cast<spatial::SpatialTree *>(spatial_tree);
  if (tree != nullptr) {
    tree->setDefaultGroupId(group);
  }
}

const ScExternalDrawProvider *sc_external_draw_provider(void)
{
  return &g_provider;
}
}
