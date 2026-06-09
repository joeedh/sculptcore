#include "mesh.h"

#include "boundary.h"
#include "mesh_path.h"
#include "uvgen.h"

#include "litestl/math/geom.h"
#include "litestl/util/index_range.h"
#include "litestl/util/map.h"
#include "litestl/util/vector.h"

using namespace litestl;
using namespace litestl::util;
using namespace litestl::math;

namespace sculptcore::mesh {

void Mesh::recalc_normals()
{
  /* Walks the live loop/disk links; thaw if frozen. Not in the sculpt hot path
   * (the spatial tree owns per-frame normals), so the thaw cost is irrelevant. */
  if (topo_frozen)
    thawTopo();

  auto &no = f.no;
  for (int fi : IndexRange(0, f.capacity())) {
    if (f.freemap[fi]) {
      continue;
    }

    int li = f.l[fi];
    int ci = l.c[li];

    float3 &co1 = v.co[c.v[ci]];
    float3 &co2 = v.co[c.v[c.next[ci]]];
    float3 &co3 = v.co[c.v[c.next[c.next[ci]]]];

    f.no[fi] = triNormal(co1, co2, co3);
  }

  for (int vi : IndexRange(0, v.capacity())) {
    if (v.freemap[vi]) {
      continue;
    }
    v.no[vi] = float3();
    VertProxy vert(this, vi);

    for (EdgeProxy edge : vert.edges()) {
      int ci = e.c[edge.i];
      if (ci == ELEM_NONE) {
        continue;
      }

      int fi = l.f[c.l[ci]];
      v.no[vi] += f.no[fi];
    }

    v.no[vi].normalize();
  }
}

namespace {
inline void fire(const util::function<void(int)> &cb, int idx)
{
  if (cb) {
    cb(idx);
  }
}
} // namespace

void Mesh::freezeTopo()
{
  if (topo_frozen) {
    return;
  }

  /* Snapshot the live topology, then keep the brush 1-ring CSR current so the
   * sculpt path is served from cached data while the live links are gone. */
  topo_cache.frozen.build(*this);
  topo_cache.ensureRing1(*this);

  v.attrs.freeTopoPages();
  e.attrs.freeTopoPages();
  c.attrs.freeTopoPages();
  l.attrs.freeTopoPages();
  f.attrs.freeTopoPages();

  topo_frozen = true;
}

void Mesh::thawTopo()
{
  if (!topo_frozen) {
    return;
  }

  v.attrs.materializeTopoPages();
  e.attrs.materializeTopoPages();
  c.attrs.materializeTopoPages();
  l.attrs.materializeTopoPages();
  f.attrs.materializeTopoPages();

  topo_cache.frozen.rebuildLinks(*this);
  topo_frozen = false;
}

int Mesh::markSeamPath(int vStart, int vEnd, int state)
{
  // shortestEdgePath + find_edge walk the live disk links; thaw if a prior
  // sculpt stroke left the mesh frozen.
  if (topo_frozen) {
    thawTopo();
  }
  util::Vector<int> path;
  if (!shortestEdgePath(this, vStart, vEnd, path) || path.size() < 2) {
    return -1;
  }
  int marked = 0;
  for (int i = 0; i + 1 < int(path.size()); i++) {
    int e = find_edge(path[i], path[i + 1]);
    if (e != ELEM_NONE) {
      boundary::setEdgeFlag(this, boundary::EDGE_SEAM, e, state != 0);
      marked++;
    }
  }
  boundary::recomputeDirty(this);
  return marked;
}

void Mesh::edgePathEdges(int vStart, int vEnd, util::Vector<int> &out)
{
  out.clear();
  if (topo_frozen) {
    thawTopo();
  }
  util::Vector<int> path;
  if (!shortestEdgePath(this, vStart, vEnd, path)) {
    return;
  }
  for (int i = 0; i + 1 < int(path.size()); i++) {
    int e = find_edge(path[i], path[i + 1]);
    if (e != ELEM_NONE) {
      out.append(e);
    }
  }
}

int Mesh::edgeSeam(int e)
{
  return boundary::edgeFlag(this, boundary::EDGE_SEAM, e) ? 1 : 0;
}

void Mesh::setEdgeSeam(int e, int state)
{
  boundary::setEdgeFlag(this, boundary::EDGE_SEAM, e, state != 0);
}

void Mesh::recomputeBoundary()
{
  if (topo_frozen) {
    thawTopo();
  }
  boundary::recomputeDirty(this);
}

void Mesh::edgePathCoords(int vStart, int vEnd, util::Vector<float> &out)
{
  out.clear();
  if (topo_frozen) {
    thawTopo();
  }
  util::Vector<int> path;
  if (!shortestEdgePath(this, vStart, vEnd, path)) {
    return;
  }
  for (int vi : path) {
    math::float3 co = v.co[vi];
    out.append(co[0]);
    out.append(co[1]);
    out.append(co[2]);
  }
}

int Mesh::generateUVFromSeams(int marginMilli)
{
  // The unwrapper flood-fills + walks live loop/disk links, so thaw first.
  if (topo_frozen) {
    thawTopo();
  }
  // Names don't marshal, so own naming here: a unique "uv[.NNN]" corner layer
  // (shares addAttr's helper). generateUVFromSeams ensures the layer + tags it
  // AttrUse::UV.
  util::string name = uniqueAttrName(&c.attrs, "uv");
  float margin = float(marginMilli) / 1000.0f;
  return mesh::generateUVFromSeams(this, name.c_str(), margin);
}

void Mesh::markAllSeams()
{
  if (topo_frozen) {
    thawTopo();
  }
  // Seam every edge so each face becomes its own UV chart — generateUVFromSeams
  // then planar-projects per face, i.e. a cuboid map. A test/demo helper.
  for (int e0 : IndexRange(0, e.count)) {
    boundary::setEdgeFlag(this, boundary::EDGE_SEAM, e0, true);
  }
  boundary::recomputeDirty(this);
}

void Mesh::fillVertexColorFromPosition()
{
  // Fill the first vertex-domain FLOAT4 layer tagged COLOR with a deterministic
  // position->rgb gradient (normalized into the mesh bbox, alpha 1). Gives the
  // vertex-color attribute meaningful, backend-identical values without a brush
  // stroke. No-op if no such layer exists.
  AttrData<math::float4> *cdata = nullptr;
  for (AttrRef &a : v.attrs.attrs) {
    if (a.type == AttrType::FLOAT4 && (a.use & AttrUse::COLOR)) {
      cdata = a.get_data<math::float4>();
      break;
    }
  }
  if (!cdata || v.count == 0) {
    return;
  }

  math::float3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
  for (int i : IndexRange(0, v.count)) {
    math::float3 &co = v.co[i];
    for (int k = 0; k < 3; k++) {
      if (co[k] < mn[k])
        mn[k] = co[k];
      if (co[k] > mx[k])
        mx[k] = co[k];
    }
  }
  math::float3 size = mx - mn;
  for (int k = 0; k < 3; k++) {
    if (size[k] < 1e-6f)
      size[k] = 1.0f;
  }
  for (int i : IndexRange(0, v.count)) {
    math::float3 &co = v.co[i];
    float r = (co[0] - mn[0]) / size[0];
    float g = (co[1] - mn[1]) / size[1];
    float b = (co[2] - mn[2]) / size[2];
    (*cdata)[i] = math::float4(r, g, b, 1.0f);
  }
}

int Mesh::make_vertex(math::float3 co, MeshCallbacks *cb)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;
  int r = v.alloc();

  v.co[r] = co;
  v.e[r] = ELEM_NONE;

  if (cb) {
    fire(cb->onVertCreate, r);
  }

  return r;
}

int Mesh::make_edge(int v1, int v2, MeshCallbacks *cb)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;
  int r = e.alloc();

  e.c[r] = ELEM_NONE;

  e.vs[r][0] = v1;
  e.vs[r][1] = v2;

  e.disk[r] = math::int4(ELEM_NONE);

  if (cb) {
    /* disk_insert rewires the existing disk neighbors at each endpoint; snapshot
     * their pre-insert links for the meshlog BEFORE they change, or undo leaves
     * those neighbors pointing at the (released) new edge. */
    int ends[2] = {v1, v2};
    for (int vv : ends) {
      if (v.e[vv] == ELEM_NONE) {
        continue;
      }
      int e2 = v.e[vv];
      int prevn = e.disk[e2][edge_side(e2, vv) * 2];
      fire(cb->onEdgeChange, e2);
      if (prevn != e2) {
        fire(cb->onEdgeChange, prevn);
      }
    }
  }

  disk_insert(r, v1);
  disk_insert(r, v2);

  if (cb) {
    fire(cb->onEdgeCreate, r);
    fire(cb->onVertChange, v1);
    fire(cb->onVertChange, v2);
  }

  return r;
}

int Mesh::make_face(std::span<int> verts, std::span<int> edges, MeshCallbacks *cb)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;
  int fi = f.alloc();
  int li = l.alloc();

  int vlen = verts.size();
  if (vlen > 3) {
    n_ngon_faces++;
  }

  f.l[fi] = li;
  f.list_count[fi] = 1;

  l.size[li] = vlen;
  l.f[li] = fi;
  l.next[li] = ELEM_NONE;

  util::Vector<int, 8> corners;
  for (int i = 0; i < vlen; i++) {
    int ci = c.alloc();

    c.v[ci] = verts[i];
    c.e[ci] = edges[i];
    c.l[ci] = li;

    if (cb && e.c[edges[i]] != ELEM_NONE) {
      /* radial_insert rewires the edge's existing radial neighbors; snapshot
       * their pre-insert links for the meshlog before they change. */
      int c2 = e.c[edges[i]];
      int c2prev = c.radial_prev[c2];
      fire(cb->onCornerChange, c2);
      if (c2prev != c2) {
        fire(cb->onCornerChange, c2prev);
      }
    }

    radial_insert(edges[i], ci);
    corners.append(ci);
  }

  l.c[li] = corners[0];

  for (int i = 0; i < vlen; i++) {
    int l1 = corners[(i - 1 + vlen) % vlen];
    int l2 = corners[i];
    int l3 = corners[(i + 1) % vlen];

    c.prev[l2] = l1;
    c.next[l2] = l3;
  }

  if (cb) {
    fire(cb->onFaceCreate, fi);
    fire(cb->onListCreate, li);
    for (int i = 0; i < vlen; i++) {
      fire(cb->onCornerCreate, corners[i]);
      fire(cb->onEdgeChange, edges[i]);
    }
  }

  return fi;
}

int Mesh::make_face(std::span<int> verts, MeshCallbacks *cb)
{
  /* find_edge below walks live disks, so thaw before touching connectivity. */
  if (topo_frozen)
    thawTopo();

  util::Vector<int, 6> edges;

  int vlen = verts.size();
  for (int i = 0; i < vlen; i++) {
    int v1 = verts[i], v2 = verts[(i + 1) % vlen];
    int e1 = find_edge(v1, v2);

    if (e1 == ELEM_NONE) {
      e1 = make_edge(v1, v2, cb);
    }

    edges.append(e1);
  }

  return make_face(verts, edges, cb);
}

void Mesh::kill_vertex(int v1, MeshCallbacks *cb)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;
  while (v.e[v1] != ELEM_NONE) {
    kill_edge(v.e[v1], cb);
  }

  if (cb) {
    fire(cb->onVertKill, v1);
  }

  v.release(v1);
}

void Mesh::kill_edge(int e1, MeshCallbacks *cb)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;
  while (e.c[e1] != ELEM_NONE) {
    kill_face(l.f[c.l[e.c[e1]]], cb);
  }

  int va = e.vs[e1][0];
  int vb = e.vs[e1][1];

  if (cb) {
    /* disk_remove rewires e1's disk neighbors at each endpoint; snapshot them
     * before they change (same reason as make_edge). */
    int ends[2] = {va, vb};
    for (int vv : ends) {
      int side1 = edge_side(e1, vv);
      int prevn = e.disk[e1][side1 * 2];
      int nextn = e.disk[e1][side1 * 2 + 1];
      if (prevn != e1) {
        fire(cb->onEdgeChange, prevn);
      }
      if (nextn != e1 && nextn != prevn) {
        fire(cb->onEdgeChange, nextn);
      }
    }
  }

  disk_remove(e1, va);
  disk_remove(e1, vb);

  if (cb) {
    fire(cb->onEdgeKill, e1);
    fire(cb->onVertChange, va);
    fire(cb->onVertChange, vb);
  }

  e.release(e1);
}

void Mesh::kill_face(int f1, MeshCallbacks *cb)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;
  int l1 = f.l[f1];
  if (l1 != ELEM_NONE && l.size[l1] > 3) {
    n_ngon_faces--;
  }
  while (l1 != ELEM_NONE) {
    int next = l.next[l1];

    int c1 = l.c[l1], startc1 = c1;
    int cnext;
    do {
      cnext = c.next[c1];

      int eid = c.e[c1];
      if (cb) {
        /* radial_remove rewires c1's radial neighbors and may change the edge's
         * first-corner pointer; snapshot them before the change. */
        int rnext = c.radial_next[c1];
        int rprev = c.radial_prev[c1];
        if (rnext != c1) {
          fire(cb->onCornerChange, rnext);
        }
        if (rprev != c1 && rprev != rnext) {
          fire(cb->onCornerChange, rprev);
        }
        fire(cb->onEdgeChange, eid);
      }

      radial_remove(eid, c1);

      if (cb) {
        fire(cb->onCornerKill, c1);
      }

      c.release(c1);
    } while ((c1 = cnext) != startc1);

    if (cb) {
      fire(cb->onListKill, l1);
    }

    l.release(l1);
    l1 = next;
  }

  if (cb) {
    fire(cb->onFaceKill, f1);
  }

  f.release(f1);
}

void Mesh::recountNgons()
{
  if (topo_frozen) {
    thawTopo();
  }
  int64_t n = 0;
  for (int fi : f) {
    int li = f.l[fi];
    if (li != ELEM_NONE && l.size[li] > 3) {
      n++;
    }
  }
  n_ngon_faces = n;
}

namespace {
inline int remap(util::span<int> map, int idx)
{
  return idx == ELEM_NONE ? ELEM_NONE : map[idx];
}
} // namespace

void Mesh::reorder_verts(util::span<int> vmap)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;
  for (int e1 : e) {
    e.vs[e1][0] = remap(vmap, e.vs[e1][0]);
    e.vs[e1][1] = remap(vmap, e.vs[e1][1]);
  }

  for (int c1 : c) {
    c.v[c1] = remap(vmap, c.v[c1]);
  }

  v.reorder(vmap);
}

void Mesh::reorder_edges(util::span<int> emap)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;
  for (int v1 : v) {
    v.e[v1] = remap(emap, v.e[v1]);
  }

  for (int e1 : e) {
    for (int k = 0; k < 4; k++) {
      e.disk[e1][k] = remap(emap, e.disk[e1][k]);
    }
  }

  for (int c1 : c) {
    c.e[c1] = remap(emap, c.e[c1]);
  }

  e.reorder(emap);
}

void Mesh::reorder_corners(util::span<int> cmap)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;
  for (int e1 : e) {
    e.c[e1] = remap(cmap, e.c[e1]);
  }

  for (int c1 : c) {
    c.next[c1] = remap(cmap, c.next[c1]);
    c.prev[c1] = remap(cmap, c.prev[c1]);
    c.radial_next[c1] = remap(cmap, c.radial_next[c1]);
    c.radial_prev[c1] = remap(cmap, c.radial_prev[c1]);
  }

  for (int l1 : l) {
    l.c[l1] = remap(cmap, l.c[l1]);
  }

  c.reorder(cmap);
}

void Mesh::reorder_lists(util::span<int> lmap)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;
  for (int c1 : c) {
    c.l[c1] = remap(lmap, c.l[c1]);
  }

  for (int l1 : l) {
    l.next[l1] = remap(lmap, l.next[l1]);
  }

  for (int f1 : f) {
    f.l[f1] = remap(lmap, f.l[f1]);
  }

  l.reorder(lmap);
}

void Mesh::reorder_faces(util::span<int> fmap)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;
  for (int l1 : l) {
    l.f[l1] = remap(fmap, l.f[l1]);
  }

  f.reorder(fmap);
}

} // namespace sculptcore::mesh
