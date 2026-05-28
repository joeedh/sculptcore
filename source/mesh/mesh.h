#pragma once

#include "attribute.h"
#include "litestl/binding/binding.h"
#include "litestl/math/vector.h"
#include "litestl/util/span.h"
#include "litestl/util/string.h"

#include "mesh_base.h"
#include "mesh_callbacks.h"
#include "mesh_enums.h"
#include "mesh_proxy.h"
#include "mesh_topo_cache.h"
#include "mesh_types.h"

#include <algorithm>
#include <concepts>
#include <cstdio>
#include <type_traits>

#include <cfloat>
#include <span>

using namespace litestl;
namespace sculptcore::mesh {

struct Mesh : public MeshBase {
  Mesh()
  {
  }

  /* Monotonic topology-edit counter. Bumped by every primitive topology
   * mutator (the make_, kill_ and reorder_ families); MeshTopoCache keys its
   * validity on it so any edit forces a rebuild. */
  uint64_t topo_stamp = 1;
  MeshTopoCache topo_cache;

  /* Frozen-topology mode: the live TOPO link columns are dropped and
   * topo_cache.frozen is authoritative. A RAM-saving cache state, never lossy
   * — any topology mutator auto-thaws (rebuilds the live links) first, so the
   * mode is invisible to *mutating* callers. See freezeTopo()/thawTopo().
   *
   * What stays valid while frozen (the sculpt-loop contract):
   *   - geometry attrs (v.co, v.no, ...) — never TOPO, always live.
   *   - the 1-ring CSR (topo_cache.ring1) — serves brush for_neighbor.
   *   - .corner.v — flagged TOPO_KEEP_FROZEN so the per-frame spatial path
   *     (tri bounds/normals/GPU upload, which reads c.v through cached corner
   *     indices) keeps working without a thaw.
   * The pure-iteration links (disk/radial/loop, v.e, e.c, c.next/prev/e/l,
   * l.*, f.l) are gone; any code that walks them must go through a mutator
   * (auto-thaws) or call thawTopo() first (recalc_normals does). */
  bool topo_frozen = false;

  /* Build the frozen snapshot and release the live TOPO pages. Ensures the
   * 1-ring CSR is current first so the brush path stays served while frozen. */
  void freezeTopo();
  /* Re-materialize the TOPO pages and rebuild the live links from the snapshot.
   * Idempotent; a no-op when not frozen. */
  void thawTopo();

  static binding::types::Struct<Mesh> *defineBindings()
  {
    using binding::types::Struct;

    Struct<Mesh> *st = new Struct<Mesh>("sculptcore::mesh::Mesh", sizeof(Mesh));
    BIND_STRUCT_MEMBER(st, v);
    BIND_STRUCT_MEMBER(st, e);
    BIND_STRUCT_METHOD(st, recalc_normals, MARGS());
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    return st;
  }

  void calcAABB(math::float3 &min, math::float3 &max)
  {
    min = math::float3(FLT_MAX);
    max = math::float3(FLT_MIN);
    for (int i = 0; i < v.count; i++) {
      min.min(v.co[i]);
      max.max(v.co[i]);
    }
  }

  int make_vertex(math::float3 co, MeshCallbacks *cb = nullptr);
  int make_edge(int v1, int v2, MeshCallbacks *cb = nullptr);
  int make_face(std::span<int> verts, std::span<int> edges, MeshCallbacks *cb = nullptr);
  int make_face(std::span<int> verts, MeshCallbacks *cb = nullptr);

  void kill_vertex(int v, MeshCallbacks *cb = nullptr);
  void kill_edge(int e, MeshCallbacks *cb = nullptr);
  void kill_face(int f, MeshCallbacks *cb = nullptr);

  EdgeOfVertIter e_of_v(int v1)
  {
    int e1 = v.e[v1];
    return EdgeOfVertIter(this, v1, e1);
  }

  inline int edge_side(int e1, int v1)
  {
    return v1 == e.vs[e1][0] ? 0 : 1;
  }

  int find_edge(int v1, int v2)
  {
    for (int e1 : e_of_v(v1)) {
      for (int e2 : e_of_v(v2)) {
        if (e1 == e2) {
          return e1;
        }
      }
    }

    return ELEM_NONE;
  }

  /* Reorder one element domain in place. Each map is map[old] = new and must
   * be a full bijection over that domain's storage capacity (free slots
   * included — SpatialTree::computeLocalityMaps produces such maps). Every
   * reference field pointing into the reordered domain is remapped, then the
   * domain's own storage is permuted; the domains are independent so the five
   * may be applied in any order. */
  void reorder_verts(util::span<int> vertex_map);
  void reorder_edges(util::span<int> edge_map);
  void reorder_corners(util::span<int> corner_map);
  void reorder_lists(util::span<int> list_map);
  void reorder_faces(util::span<int> face_map);

  void recalc_normals();
private:
  void radial_insert(int e1, int c1)
  {
    if (e.c[e1] == ELEM_NONE) {
      e.c[e1] = c1;
      c.radial_next[c1] = c.radial_prev[c1] = c1;
    } else {
      int c2 = e.c[e1];
      int c2prev = c.radial_prev[c2];

      c.radial_prev[c1] = c2prev;
      c.radial_next[c1] = c2;

      c.radial_next[c2prev] = c1;
      c.radial_prev[c2] = c1;
    }
  }

  void radial_remove(int e1, int c1)
  {
    if (e.c[e1] == c1) {
      e.c[e1] = c.radial_next[c1];
    }
    if (e.c[e1] == c1) {
      e.c[e1] = ELEM_NONE;
    }

    int next = c.radial_next[c1];
    int prev = c.radial_prev[c1];

    c.radial_next[prev] = next;
    c.radial_prev[next] = prev;
  }

  void disk_insert(int e1, int v1)
  {
    int side1 = edge_side(e1, v1);

    if (v.e[v1] == ELEM_NONE) {
      v.e[v1] = e1;

      e.disk[e1][side1 * 2] = e1;
      e.disk[e1][side1 * 2 + 1] = e1;

      return;
    }

    int e2 = v.e[v1];
    int side2 = edge_side(e2, v1);

    int prev = e.disk[e2][side2 * 2];
    int side3 = edge_side(prev, v1);

    e.disk[e2][side2 * 2] = e1; /* e2.prev */

    e.disk[e1][side1 * 2] = prev;   /* e1.prev */
    e.disk[e1][side1 * 2 + 1] = e2; /* e1.next */

    e.disk[prev][side3 * 2 + 1] = e1; /* prev.next */
  }

  void disk_remove(int e1, int v1)
  {
    int side1 = edge_side(e1, v1);

    int prev = e.disk[e1][side1 * 2];
    int next = e.disk[e1][side1 * 2 + 1];

    int sidep = edge_side(prev, v1);
    int siden = edge_side(next, v1);

    e.disk[prev][sidep * 2 + 1] = next; /* prev->next */
    e.disk[next][siden * 2] = prev;     /* next->prev */

    if (e1 == v.e[v1]) {
      v.e[v1] = next;
    }

    if (e1 == v.e[v1]) {
      v.e[v1] = ELEM_NONE;
    }
  }
};
}; // namespace sculptcore::mesh
