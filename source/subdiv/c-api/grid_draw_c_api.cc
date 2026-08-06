/** The multires grids draw source's extdraw glue: adapts GridDrawSource to
 * the type-erased custom-source vtable of spatial/c-api/external_draw.cc
 * (spatial cannot depend on subdiv, so the concrete type lives here).
 * Registered per (Multires, level) via sc_external_draw_register_grids. */

#include "spatial/c-api/external_draw.h"
#include "subdiv/grid_draw_source.h"
#include "subdiv/multires.h"

#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

using namespace sculptcore;

namespace {

/* Scratch arrays returned by nodes_get; valid until the next call, matching
 * the mesh branch's contract (the host consumes one sync fully per call). */
litestl::util::Vector<ScExternalDrawNode> &scratch()
{
  static litestl::util::Vector<ScExternalDrawNode> nodes;
  return nodes;
}

litestl::util::Vector<const void *> &attr_ptrs()
{
  static litestl::util::Vector<const void *> ptrs;
  return ptrs;
}

int grids_nodes_get(void *src_v,
                    const ScExternalDrawAttrRequest *req,
                    ScExternalDrawNode **r_nodes)
{
  subdiv::GridDrawSource &src = *static_cast<subdiv::GridDrawSource *>(src_v);
  litestl::util::Vector<ScExternalDrawNode> &out = scratch();
  litestl::util::Vector<const void *> &attrs = attr_ptrs();
  out.clear();
  attrs.clear();
  /* The host always addresses 4 slots (color@0/uv@1/mask@2/fset@3); a
   * shorter block would be probed out of bounds. Reserved up front so
   * appends never realloc while earlier nodes' `attrs` point into it. */
  size_t attrs_per_node = 4;
  if (req != nullptr && req->attrs_num > 0 && size_t(req->attrs_num) > attrs_per_node) {
    attrs_per_node = size_t(req->attrs_num);
  }
  const int count = src.nodeCount();
  attrs.ensure_capacity(size_t(count) * attrs_per_node);

  for (int i = 0; i < count; i++) {
    subdiv::GridDrawSource::Node &n = src.node(i);
    if (n.verts == 0 || n.pos.size() == 0) {
      continue; /* unfilled node: never hand the host a null positions ptr */
    }
    ScExternalDrawNode dn = {};
    dn.positions = reinterpret_cast<const float(*)[3]>(n.pos.data());
    dn.normals = reinterpret_cast<const float(*)[3]>(n.no.data());
    /* mask@2 from the domain mirror; color/uv/fset null — the host fills
     * defaults, and those overlays fall back to the slot provider. */
    const size_t base = attrs.size();
    for (size_t slot = 0; slot < attrs_per_node; slot++) {
      attrs.append(slot == 2 ? static_cast<const void *>(n.mask.data()) : nullptr);
    }
    dn.attrs = &attrs[base];
    dn.verts_num = n.verts;
    dn.material_index = n.material;
    dn.node_id = src.nodeId(i);
    dn.update_flags = SC_EXTERNAL_DRAW_UPDATE_NONE;
    if (n.update & subdiv::GridDrawSource::Update_Data) {
      dn.update_flags |= SC_EXTERNAL_DRAW_UPDATE_DATA;
    }
    if (n.update & subdiv::GridDrawSource::Update_Topo) {
      dn.update_flags |= SC_EXTERNAL_DRAW_UPDATE_TOPOLOGY;
    }
    n.update = subdiv::GridDrawSource::Update_None;
    dn.bounds_min[0] = n.aabb.min[0];
    dn.bounds_min[1] = n.aabb.min[1];
    dn.bounds_min[2] = n.aabb.min[2];
    dn.bounds_max[0] = n.aabb.max[0];
    dn.bounds_max[1] = n.aabb.max[1];
    dn.bounds_max[2] = n.aabb.max[2];
    out.append(dn);
  }
  *r_nodes = out.data();
  return int(out.size());
}

void grids_update(void *src_v)
{
  static_cast<subdiv::GridDrawSource *>(src_v)->update();
}

void grids_destroy(void *src_v)
{
  subdiv::GridDrawSource *src = static_cast<subdiv::GridDrawSource *>(src_v);
  /* The Multires backref (dirty feeds) dies with the registration. */
  if (subdiv::Multires *mr = src->multires()) {
    if (mr->drawSource() == src) {
      mr->setDrawSource(nullptr);
    }
  }
  litestl::alloc::Delete(src);
}

const ScExternalDrawSourceOps g_grids_ops = {
    grids_nodes_get,
    grids_update,
    grids_destroy,
};

} // namespace

extern "C" {

/** Register the grids-fed draw source for a multires session: draw comes
 * straight from (mr, level)'s GridLevelDomain, no slot mesh involved. Builds
 * the node partition and does the initial fill (the caller registers when
 * the domain is live — mode enter / level switch). Replaces any prior
 * registration on the key; re-register per level (the source is bound to
 * one level). */
void sc_external_draw_register_grids(unsigned int object_key, void *multires, int level)
{
  subdiv::Multires *mr = static_cast<subdiv::Multires *>(multires);
  if (mr == nullptr || level < 1 || level > mr->maxLevel()) {
    return;
  }
  auto *src = litestl::alloc::New<subdiv::GridDrawSource>("grid draw source", mr, level);
  sc_external_draw_register_custom(object_key, src, &g_grids_ops);
  mr->setDrawSource(src);
}
}
