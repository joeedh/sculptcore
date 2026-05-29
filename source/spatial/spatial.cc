#include "spatial.h"
#include "shaders/spatial_shaders.h"

#include "node.h"

#include "litestl/math/geom.h"
#include "litestl/math/vector.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

// #include "litestl/util/map.h"
#include "litestl/util/rand.h"

#include "gpu/batch.h"
#include "gpu/command.h"
#include "gpu/manager.h"
#include "gpu/types.h"
#include "gpu/vbo.h"

#include "mesh/mesh.h"
#include "mesh/mesh_proxy.h"
#include "mesh/utils/triangulate.h"
#include "util/index_range.h"
#include "util/task.h"

#include <cmath>

using namespace litestl::util;
using namespace litestl::math;
using namespace sculptcore::mesh;
using namespace litestl;

static inline float3 calc_eps_float3(float3 size)
{
  float3 eps = size * 0.001f;

  for (int i = 0; i < 3; i++) {
    eps[i] = std::min(eps[i], 0.00001f);
  }

  return eps;
}

namespace sculptcore::spatial {

/* Descend from the root, pruning any subtree whose AABB misses the brush
 * sphere; collect the surviving leaves. Internal-node AABBs are the union of
 * their children (maintained by regen_node_bounds), so a node that misses the
 * sphere cannot contain an overlapping leaf — this returns exactly the same
 * leaf set as a linear scan of every leaf, but visits O(log n + hits) nodes
 * instead of all of them, and allocates nothing per call. */
static void filterNodes_recurse(SpatialNode *node,
                                float3 co,
                                float radius,
                                Vector<SpatialNode *> &out)
{
  if (!aabbSphereIsect(co, radius, node->aabb)) {
    return;
  }
  if (node->flag & Spatial_Leaf) {
    node->debugIdOffset++;
    out.append(node);
    return;
  }
  for (int i = 0; i < 2; i++) {
    if (node->children[i]) {
      filterNodes_recurse(node->children[i], co, radius, out);
    }
  }
}

bool SpatialTree::filterNodes(float3 co, float radius, Vector<SpatialNode *> &out)
{
  filterNodes_recurse(root, co, radius, out);
  return out.size() != 0;
}

void SpatialTree::regen_node_tris(SpatialNode *node)
{
  node->flag &= ~Spatial_RegenTris;

  /* Topology changed — affected_verts no longer captures everything whose
   * normal needs recomputing (new tris may contribute to verts that didn't
   * move). Drop the incremental hint so update_node_normals falls back to
   * a full rebuild for this leaf. */
  node->affected_verts.clear_and_contract();

  /* TODO: use a property CDT for > 4 vert or > 1 hole faces.
   * For now just handle triangles and quads.
   */
  node->data->tris.clear_and_contract();
  for (int f : node->data->unique_faces) {
    int l = m->f.l[f];
    int c = m->l.c[l];

    NodeTri &tri = node->data->tris.grow_one();
    tri.c[0] = c;
    tri.c[1] = m->c.next[c];
    tri.c[2] = m->c.next[tri.c[1]];
    tri.f = f;

    if (m->l.size[l] > 3) {
      NodeTri &tri2 = node->data->tris.grow_one();

      tri2.c[0] = c;
      tri2.c[1] = m->c.next[m->c.next[c]];
      tri2.c[2] = m->c.next[tri2.c[1]];
      tri2.f = f;
    }
  }
}

[[clang::optnone]]
void SpatialTree::add_face_intern(SpatialNode *node,
                                  int f,
                                  std::span<Tri> &tris,
                                  float3 &fcent)
{
  if ((node->flag & Spatial_Leaf) && node_needs_split(node)) {
    split_node(node);
  }

  if (!(node->flag & Spatial_Leaf)) {
    /* Route the face to the single child it belongs in, by its centroid.
     * split_node partitions the parent AABB along one axis at a midplane, so
     * the two children meet at child[0].max == child[1].min on that axis and
     * the centroid falls in exactly one of them. This is a single scalar
     * compare, replacing the old per-triangle triangle/AABB SAT overlap test
     * run against both children (the build's dominant cost). Faces are no
     * longer replicated into every overlapped leaf; each is owned by one leaf,
     * which is all the render/brush paths consume (unique_faces/unique_verts).
     * Leaf bounds still cover neighbour-owned boundary verts via the tris loop
     * in regen_node_bounds. */
    SpatialNode *c0 = node->children[0];
    SpatialNode *c1 = node->children[1];
    int axis = 0;
    for (int i = 0; i < 3; i++) {
      if (c0->aabb.max[i] != c1->aabb.max[i]) {
        axis = i;
        break;
      }
    }
    SpatialNode *child = fcent[axis] <= c0->aabb.max[axis] ? c0 : c1;
    add_face_intern(child, f, tris, fcent);
    return;
  }

  node->flag |= Spatial_RegenTris | Spatial_RegenBounds;
  FaceProxy face(m, f);

  if (treeMesh.f.node[face] == 0) {
    treeMesh.f.node[face] = node->id;
    node->data->unique_faces.add(f);
  } else {
    node->data->other_faces.add(f);
  }

  for (auto list : face.lists()) {
    for (auto c : list) {
      if (treeMesh.v.node[c.v()]) {
        node->data->other_verts.add(c.v());
      } else {
        node->data->unique_verts.add(c.v());
        treeMesh.v.node[c.v()] = node->id;
      }
    }
  }
}

void SpatialTree::split_node(SpatialNode *node)
{
  node->children[0] = alloc_node();
  node->children[1] = alloc_node();

  using namespace litestl::math;
  const float3 min(node->aabb.min), max(node->aabb.max);
  float3 mean(0.0f);

  for (int v : node->data->unique_verts) {
    VertProxy vert(m, v);
    mean += vert.co();

    /* Unassign verts. */
    treeMesh.v.node[v] = 0;
  }

  mean /= node->data->unique_verts.size();

  float3 size = max - min;
  int axis = 0;

#if 1
  for (int i = 1; i < 3; i++) {
    if (size[i] > size[axis]) {
      axis = i;
    } else if (size[i] == size[axis]) {
      // axis = node->depth & 1 ? i : axis;
    }
  }
#else
  axis = node->depth % 3;
#endif

  /* Split at the geometric mean of the node's verts along the longest axis,
   * as a fraction of the box. Both children meet at that one plane
   * (min + size*t), so add_face_intern's centroid router partitions faces
   * cleanly there; a mean split keeps the two children's vert counts closer to
   * even than a fixed midpoint, reducing depth and empty leaves. Clamped off
   * the box edges to avoid a degenerate all-in-one-child split. */
  float t = (mean[axis] - min[axis]) / size[axis];
  t = std::min(std::max(t, 0.01f), 0.99f);

  for (int i = 0; i < 2; i++) {
    SpatialNode *child = node->children[i];
    child->parent = node;

    child->flag = Spatial_Leaf | Spatial_RegenTris | Spatial_RegenBounds |
                  Spatial_RegenGPU | Spatial_UpdateNormals;
    child->depth = node->depth + 1;

    child->aabb.min = node->aabb.min;
    child->aabb.max = node->aabb.max;

    if (i == 0) {
      child->aabb.max[axis] = child->aabb.min[axis] + size[axis] * t;
    } else {
      child->aabb.min[axis] = child->aabb.min[axis] + size[axis] * t;
    }

    child->create_data();
  }

  node->flag &= ~Spatial_Leaf;
  Vector<Tri, 16> tris;

  for (int f : node->data->unique_faces) {
    FaceProxy face(m, f);
    float3 fcent = face.calc_center();

    // unassign face
    treeMesh.f.node[f] = 0;

    tris.clear();
    if (triangulateFace(*m, f, tris)) {
      std::span<Tri> tris_span = tris;
      add_face_intern(node, f, tris_span, fcent);
    }
  }
  for (int f : node->data->other_faces) {
    FaceProxy face(m, f);
    float3 fcent = face.calc_center();

    tris.clear();
    if (triangulateFace(*m, f, tris)) {
      std::span<Tri> tris_span = tris;
      add_face_intern(node, f, tris_span, fcent);
    }
  }

  node->delete_data();
  node->flag |= Spatial_RegenBounds;
}

void SpatialTree::regen_node_bounds(SpatialNode *node, bool recurse)
{
  node->flag &= ~Spatial_RegenBounds;

  node->aabb.reset();

  if (!(node->flag & Spatial_Leaf)) {
    for (int i = 0; i < 2; i++) {
      if (recurse && (node->children[i]->flag & Spatial_RegenBounds)) {
        regen_node_bounds(node->children[i], true);
      }

      node->aabb.min.min(node->children[i]->aabb.min);
      node->aabb.max.max(node->children[i]->aabb.max);
    }
  } else {
    if (node->data->unique_verts.size() != 0) {
      node->aabb.min = float3(FLT_MAX);
      node->aabb.max = float3(FLT_MIN);
    }

    for (int v : node->data->unique_verts) {
      VertProxy vert(m, v);

      float3 &co = vert.co();

      node->aabb.min.min(co);
      node->aabb.max.max(co);
    }

    /* Cover verts referenced by this leaf's faces but owned by a neighbour
     * (not in unique_verts). Read them through the node's tris + the .corner.v
     * column, which stays materialized in frozen-topology mode — walking the
     * live face/loop links here would read freed pages mid-stroke. */
    for (const NodeTri &tri : node->data->tris) {
      for (int i = 0; i < 3; i++) {
        float3 &co = m->v.co[m->c.v[tri.c[i]]];

        node->aabb.min.min(co);
        node->aabb.max.max(co);
      }
    }

    float3 eps = calc_eps_float3(node->aabb.max - node->aabb.min);

    node->aabb.min -= eps;
    node->aabb.max += eps;
  }
}

util::Vector<SpatialNode *> SpatialTree::leaves()
{
  if (leafCacheDirty_) {
    leafCache_.clear();
    for (SpatialNode *node : nodes) {
      if (node->flag & Spatial_Leaf) {
        leafCache_.append(node);
      }
    }
    leafCacheDirty_ = false;
  }
  return leafCache_;
}

util::Vector<SpatialNode *> SpatialTree::gpu_nodes()
{
  if (gpuNodeCacheDirty_) {
    gpuNodeCache_.clear();
    for (SpatialNode *node : nodes) {
      if (node->is_gpu_node) {
        gpuNodeCache_.append(node);
      }
    }
    gpuNodeCacheDirty_ = false;
  }
  return gpuNodeCache_;
}

/* Postorder bottom-up: leaves carry their own tri count, internals sum
 * children. Called once per update() tick before assign_gpu_nodes. */
static int recompute_subtree_tri_counts_recurse(SpatialNode *node)
{
  if (node->flag & Spatial_Leaf) {
    node->subtree_tri_count = node->data ? int(node->data->tris.size()) : 0;
    return node->subtree_tri_count;
  }

  int total = 0;
  for (int i = 0; i < 2; i++) {
    if (node->children[i]) {
      total += recompute_subtree_tri_counts_recurse(node->children[i]);
    }
  }
  node->subtree_tri_count = total;
  return total;
}

void SpatialTree::recompute_subtree_tri_counts()
{
  if (root) {
    recompute_subtree_tri_counts_recurse(root);
  }
}

/* Top-down: a node becomes a GPU node when its subtree fits the target —
 * else descend. Leaves always fit (cannot split further from the GPU
 * layer's POV). When a node *becomes* a GPU node, any descendants that
 * were previously GPU nodes get unmarked and their buffers disposed. */
static void unmark_descendant_gpu_nodes(SpatialNode *node)
{
  if (node->flag & Spatial_Leaf) {
    return;
  }
  for (int i = 0; i < 2; i++) {
    SpatialNode *c = node->children[i];
    if (!c) {
      continue;
    }
    if (c->is_gpu_node) {
      c->is_gpu_node = false;
      if (c->gpu_data) {
        alloc::Delete<GpuData>(c->gpu_data);
        c->gpu_data = nullptr;
      }
    }
    unmark_descendant_gpu_nodes(c);
  }
}

static void assign_gpu_nodes_recurse(SpatialNode *node, int target)
{
  bool fits = (node->flag & Spatial_Leaf) || (node->subtree_tri_count <= target);
  if (fits) {
    node->is_gpu_node = true;
    unmark_descendant_gpu_nodes(node);
    return;
  }

  if (node->is_gpu_node) {
    node->is_gpu_node = false;
    if (node->gpu_data) {
      alloc::Delete<GpuData>(node->gpu_data);
      node->gpu_data = nullptr;
    }
  }

  for (int i = 0; i < 2; i++) {
    if (node->children[i]) {
      assign_gpu_nodes_recurse(node->children[i], target);
    }
  }
}

void SpatialTree::assign_gpu_nodes()
{
  if (!root) {
    return;
  }
  assign_gpu_nodes_recurse(root, gpu_tri_target);
  done_gpu_assignment = true;
  /* is_gpu_node flags just changed across the tree. */
  gpuNodeCacheDirty_ = true;
}

SpatialNode *SpatialTree::find_gpu_owner(SpatialNode *node)
{
  for (SpatialNode *n = node; n; n = n->parent) {
    if (n->is_gpu_node) {
      return n;
    }
  }
  return nullptr;
}

void SpatialTree::autoTuneLimits()
{
  const int verts = m->v.count;
  /* Quads triangulate to ~2 tris; good enough before tris are built. */
  const long tris = (long)m->f.count * 2;

  /* leaf_limit ~512 (the sweet spot), but small meshes shrink it so they still
   * produce ~16+ leaves rather than one giant leaf. */
  int byVerts = verts / 16;
  leaf_limit = byVerts < 64 ? 64 : (byVerts > 512 ? 512 : byVerts);

  /* Aim for ~256 GPU nodes (== draw calls): coarse enough for low draw
   * overhead, fine enough for frustum culling and bounded per-node regen. */
  long byBudget = tris / 256;
  if (byBudget < 2048) {
    byBudget = 2048;
  } else if (byBudget > 65536) {
    byBudget = 65536;
  }
  gpu_tri_target = (int)byBudget;
}

void SpatialTree::buildAll()
{
  setup();

  /* Clear any leaf-ownership a previous tree left on the mesh: the
   * .spatial.{v,f}.node attrs outlive the tree, and a fresh tree's node ids
   * restart at 1, so stale ids are meaningless here. Without this, building a
   * second tree on the same mesh sees every elem already owned -> unique_verts
   * stays 0 -> nothing splits -> a single empty leaf. */
  for (int i = 0; i < int(m->v.capacity()); i++) {
    treeMesh.v.node[i] = 0;
  }
  for (int i = 0; i < int(m->f.capacity()); i++) {
    treeMesh.f.node[i] = 0;
  }

  m->calcAABB(root->aabb.min, root->aabb.max);
  m->recalc_normals();
  float eps = 0.0000001f;
  root->aabb.min -= eps;
  root->aabb.max += eps;

  int n = m->f.count;

  // insert faces in random order
  // to balance tree better
  litestl::util::Random rnd(0);
  int *faces = new int[n];
  for (int i = 0; i < n; i++) {
    faces[i] = i;
  }
  for (int i = 0; i < (n >> 1); i++) {
    int ri = rnd.get_int() % n;
    std::swap(faces[i], faces[ri]);
  }

  for (int i = 0; i < n; i++) {
    add_face(faces[i]);
  }

  delete[] faces;

  /* regen_node_bounds derives leaf AABBs from each node's tris (read via the
   * frozen-safe .corner.v column), so the tris must be built first. */
  for (SpatialNode *node : nodes) {
    if (node->flag & Spatial_Leaf) {
      ensure_node_tris(node);
    }
  }

  regen_node_bounds(root, true);
}

namespace {
/* Complete a partial locality map into a full bijection over [0, cap).
 * Live-but-unmapped elements (e.g. faces owned by no leaf, or boundary verts
 * not reached by the face walk) take the next dense indices so every live
 * element lands in [0, count); free slots fill the [count, cap) tail. buildAll
 * iterates faces as [0, count), so live elements MUST be front-packed. */
void finish_map(mesh::ElemData &ed, util::Vector<int> &map, int cap, int &counter)
{
  for (int i : ed) {
    if (map[i] == -1) {
      map[i] = counter++;
    }
  }
  for (int i = 0; i < cap; i++) {
    if (map[i] == -1) {
      map[i] = counter++;
    }
  }
}
} // namespace

void SpatialTree::computeLocalityMaps(util::Vector<int> &vmap,
                                      util::Vector<int> &emap,
                                      util::Vector<int> &cmap,
                                      util::Vector<int> &lmap,
                                      util::Vector<int> &fmap)
{
  const int capV = int(m->v.capacity());
  const int capE = int(m->e.capacity());
  const int capC = int(m->c.capacity());
  const int capL = int(m->l.capacity());
  const int capF = int(m->f.capacity());

  auto init = [](util::Vector<int> &map, int cap) {
    map.resize(cap);
    for (int i = 0; i < cap; i++) {
      map[i] = -1;
    }
  };
  init(vmap, capV);
  init(emap, capE);
  init(cmap, capC);
  init(lmap, capL);
  init(fmap, capF);

  int nv = 0, ne = 0, nc = 0, nl = 0, nf = 0;

  for (SpatialNode *leaf : leaves()) {
    /* Verts owned by this leaf first, so a node's verts are contiguous even if
     * none of its faces reference them in this walk (shared boundary verts). */
    for (int vrt : leaf->unique_verts()) {
      if (vmap[vrt] == -1) {
        vmap[vrt] = nv++;
      }
    }

    for (int face : leaf->unique_faces()) {
      if (fmap[face] != -1) {
        continue;
      }
      fmap[face] = nf++;

      mesh::FaceProxy fp(m, face);
      for (auto list : fp.lists()) {
        if (lmap[list.i] == -1) {
          lmap[list.i] = nl++;
        }
        for (auto cnr : list) {
          int ci = cnr.i;
          if (cmap[ci] == -1) {
            cmap[ci] = nc++;
          }
          int edge = m->c.e[ci];
          if (edge != ELEM_NONE && emap[edge] == -1) {
            emap[edge] = ne++;
          }
          int vrt = m->c.v[ci];
          if (vrt != ELEM_NONE && vmap[vrt] == -1) {
            vmap[vrt] = nv++;
          }
        }
      }
    }
  }

  finish_map(m->v, vmap, capV, nv);
  finish_map(m->e, emap, capE, ne);
  finish_map(m->c, cmap, capC, nc);
  finish_map(m->l, lmap, capL, nl);
  finish_map(m->f, fmap, capF, nf);
}

void SpatialTree::applyReorder(util::span<int> vmap,
                               util::span<int> emap,
                               util::span<int> cmap,
                               util::span<int> lmap,
                               util::span<int> fmap)
{
  m->reorder_verts(vmap);
  m->reorder_edges(emap);
  m->reorder_corners(cmap);
  m->reorder_lists(lmap);
  m->reorder_faces(fmap);

  rebuild();
}

void SpatialTree::reorderForLocality()
{
  util::Vector<int> vmap, emap, cmap, lmap, fmap;
  computeLocalityMaps(vmap, emap, cmap, lmap, fmap);
  applyReorder(vmap, emap, cmap, lmap, fmap);
}

void SpatialTree::rebuild()
{
  for (SpatialNode *node : nodes) {
    alloc::Delete(node);
  }
  nodes.clear();
  node_idmap.clear();
  node_idgen = 1;
  done_gpu_assignment = false;
  /* Drop dangling pointers into the freed node set; buildAll repopulates. */
  leafCache_.clear();
  gpuNodeCache_.clear();
  leafCacheDirty_ = true;
  gpuNodeCacheDirty_ = true;

  if (drawBatch) {
    alloc::Delete(drawBatch);
    drawBatch = nullptr;
  }

  /* Ownership attrs rode along with the reorder; reset so add_face re-assigns
   * against the fresh node set. */
  treeMesh.setup(m);
  for (int i = 0; i < int(m->v.capacity()); i++) {
    treeMesh.v.node[i] = 0;
  }
  for (int i = 0; i < int(m->f.capacity()); i++) {
    treeMesh.f.node[i] = 0;
  }

  root = alloc_node();
  root->flag =
      Spatial_Leaf | Spatial_RegenTris | Spatial_RegenGPU | Spatial_UpdateNormals;
  root->create_data();

  buildAll();
}

sculptcore::gpu::DrawBatch *
SpatialTree::buildLeafBoundsBatch(sculptcore::gpu::GPUManager &mgr)
{
  using namespace sculptcore::gpu;
  litestl::util::Random rnd(0);

  util::Vector<SpatialNode *> ls = leaves();

  /* 12 edges per box × 2 endpoints = 24 verts per leaf. */
  const int vertsPerLeaf = 24;
  int totalVerts = ls.size() * vertsPerLeaf;

  Buffer *posBuf = mgr.createBuffer(
      litestl::util::string("position"), GPUType::FLOAT32, 3, totalVerts);
  Buffer *colorBuf =
      mgr.createBuffer(litestl::util::string("color"), GPUType::FLOAT32, 4, totalVerts);
  Buffer *uvBuf =
      mgr.createBuffer(litestl::util::string("uv"), GPUType::FLOAT32, 2, totalVerts);

  float3 *pos = posBuf->get_data<float3>();
  float4 *color = colorBuf->get_data<float4>();
  float2 *uv = uvBuf->get_data<float2>();

  int idx = 0;

  auto addLine =
      [pos, color, uv, &idx](const float3 &a, const float3 &b, const float4 &clr) {
        pos[idx] = a;
        color[idx] = clr;
        uv[idx] = float2(0.0f, 0.0f);
        idx++;

        pos[idx] = b;
        color[idx] = clr;
        uv[idx] = float2(1.0f, 1.0f);
        idx++;
      };

  for (SpatialNode *node : ls) {
    litestl::util::Random rnd2(node->id + node->debugIdOffset);

    float4 clr(0.0);

    clr[0] = rnd2.get_float();
    clr[1] = rnd2.get_float();
    clr[2] = rnd2.get_float();
    clr.normalize();
    clr[3] = 1.0;

    float3 mn = node->aabb.min;
    float3 mx = node->aabb.max;
    float3 c[8] = {
        {mn[0], mn[1], mn[2]},
        {mx[0], mn[1], mn[2]},
        {mx[0], mx[1], mn[2]},
        {mn[0], mx[1], mn[2]},
        {mn[0], mn[1], mx[2]},
        {mx[0], mn[1], mx[2]},
        {mx[0], mx[1], mx[2]},
        {mn[0], mx[1], mx[2]},
    };

    /* Bottom quad (z = mn). */
    addLine(c[0], c[1], clr);
    addLine(c[1], c[2], clr);
    addLine(c[2], c[3], clr);
    addLine(c[3], c[0], clr);

    /* Top quad (z = mx). */
    addLine(c[4], c[5], clr);
    addLine(c[5], c[6], clr);
    addLine(c[6], c[7], clr);
    addLine(c[7], c[4], clr);

    /* Vertical edges. */
    addLine(c[0], c[4], clr);
    addLine(c[1], c[5], clr);
    addLine(c[2], c[6], clr);
    addLine(c[3], c[7], clr);
  }

  posBuf->dirty();

  DrawBatch *batch = mgr.createBatch();
  batch->buffers.append(posBuf);
  batch->buffers.append(colorBuf);
  batch->buffers.append(uvBuf);

  auto *shader = &spatialShaders.basicLineShader;

  DrawCommand *cmd = mgr.createCommand(
      batch, GPUCmdType::DRAW_LINES, shader, 0, totalVerts, totalVerts / 2);
  cmd->attrs.append(posBuf);
  cmd->attrs.append(colorBuf);
  cmd->attrs.append(uvBuf);

  return batch;
}

void SpatialTree::update_node_normals(SpatialNode *node)
{
  node->flag &= ~Spatial_UpdateNormals;

  auto &node_vattr = node->treeMesh->v.node;
  auto &node_fattr = node->treeMesh->f.node;

  /* Incremental path: only recompute normals for verts the brush actually
   * moved, plus their 1-ring (since a moved vert changes the normal of
   * every face touching it, which in turn changes the normals of the other
   * verts of those faces). For small brushes on a large leaf this avoids
   * zeroing + renormalizing hundreds of unaffected verts/faces and skips
   * the cross-product accumulation for tris that didn't change. */
  if (node->affected_verts.size() > 0) {
    Set<int> moved_verts;
    for (int v : node->affected_verts) {
      moved_verts.add(v);
    }

    /* Expand to the 1-ring: any tri that touches a moved vert contributes
     * to the affected face/vert sets. */
    Set<int> affected_face_set;
    Set<int> affected_vert_set;
    Vector<int, 64> affected_tri_indices;

    for (int ti : IndexRange(node->data->tris.size())) {
      const auto &tri = node->data->tris[ti];
      int v1 = m->c.v[tri.c[0]];
      int v2 = m->c.v[tri.c[1]];
      int v3 = m->c.v[tri.c[2]];

      if (!moved_verts.contains(v1) && !moved_verts.contains(v2) &&
          !moved_verts.contains(v3)) {
        continue;
      }

      affected_tri_indices.append(ti);
      affected_face_set.add(tri.f);
      affected_vert_set.add(v1);
      affected_vert_set.add(v2);
      affected_vert_set.add(v3);
    }

    for (int v : affected_vert_set) {
      if (node_vattr[v] == node->id) {
        m->v.no[v].zero();
      }
    }
    for (int f : affected_face_set) {
      if (node_fattr[f] == node->id) {
        m->f.no[f].zero();
      }
    }

    for (int ti : affected_tri_indices) {
      const auto &tri = node->data->tris[ti];
      int v1 = m->c.v[tri.c[0]];
      int v2 = m->c.v[tri.c[1]];
      int v3 = m->c.v[tri.c[2]];

      float3 n = triNormal(m->v.co[v1], m->v.co[v2], m->v.co[v3]);

      if (node_fattr[tri.f] == node->id) {
        m->f.no[tri.f] += n;
      }
      if (node_vattr[v1] == node->id) {
        m->v.no[v1] += n;
      }
      if (node_vattr[v2] == node->id) {
        m->v.no[v2] += n;
      }
      if (node_vattr[v3] == node->id) {
        m->v.no[v3] += n;
      }
    }

    for (int f : affected_face_set) {
      if (node_fattr[f] == node->id) {
        m->f.no[f].normalize();
      }
    }
    for (int v : affected_vert_set) {
      if (node_vattr[v] == node->id) {
        m->v.no[v].normalize();
      }
    }

    node->affected_verts.clear();
    return;
  }

  /* Full rebuild path: initial build, post-topology-change, or any non-brush
   * dirty source that didn't populate affected_verts. */
  for (int v : node->unique_verts()) {
    m->v.no[v].zero();
  }
  for (int f : node->unique_faces()) {
    m->f.no[f].zero();
  }

  for (const auto &tri : node->data->tris) {
    int v1 = m->c.v[tri.c[0]];
    int v2 = m->c.v[tri.c[1]];
    int v3 = m->c.v[tri.c[2]];

    float3 n = triNormal(m->v.co[v1], m->v.co[v2], m->v.co[v3]);

    if (node_fattr[tri.f] == node->id) {
      m->f.no[tri.f] += n;
    }

    if (node_vattr[v1] == node->id) {
      m->v.no[v1] += n;
    }
    if (node_vattr[v2] == node->id) {
      m->v.no[v2] += n;
    }
    if (node_vattr[v3] == node->id) {
      m->v.no[v3] += n;
    }
  }

  for (int f : node->data->unique_faces) {
    m->f.no[f].normalize();
  }
  for (int v : node->data->unique_verts) {
    m->v.no[v].normalize();
  }
}

bool SpatialTree::update(gpu::GPUManager *gpu)
{
  bool result = false;
  bool bounds = false;
  bool drawBatchUpdated = false;

  /* Phase: regen leaf tris. Must run before the bounds phase: regen_node_bounds
   * derives leaf AABBs from node->data->tris (via the frozen-safe .corner.v
   * column), so the tris have to be current first. */
  Vector<SpatialNode *, 256> updateTriNodes;
  bool topology_changed = false;
  for (SpatialNode *node : nodes) {
    if (!(node->flag & Spatial_Leaf)) {
      continue;
    }
    if (node->flag & Spatial_RegenTris) {
      updateTriNodes.append(node);
      drawBatchUpdated = true;
      topology_changed = true;
    }
  }

  /* regen_node_tris walks the live face/loop/corner link columns (f.l, l.c,
   * c.next, l.size). Those pages are dropped in frozen-topology mode (only
   * .corner.v is kept). A brush dab freezes topology, so if a stroke runs
   * before the first tri regen (e.g. a script strokes before the initial
   * render), the pending RegenTris would read freed pages. Thaw first; the
   * next dab re-freezes. */
  if (updateTriNodes.size() > 0 && m->topo_frozen) {
    m->thawTopo();
  }

  for (SpatialNode *node : updateTriNodes) {
    ensure_node_tris(node);
  }

  for (SpatialNode *node : nodes) {
    if (node->flag & Spatial_RegenBounds) {
      while (node) {
        node->flag |= Spatial_RegenBounds;
        node = node->parent;
        bounds = true;
      }
    }
  }

  if (bounds) {
    regen_node_bounds(root, true);
    result = true;
  }

  Vector<SpatialNode *, 256> updateNormalsNodes;

  /* Phase: leaf normals (independent of partition). */
  for (SpatialNode *node : nodes) {
    if (!(node->flag & Spatial_Leaf)) {
      continue;
    }
    if (node->flag & Spatial_UpdateNormals) {
      updateNormalsNodes.append(node);
      drawBatchUpdated = true;
    }
  }

#ifdef NO_PARALLEL_FOR
  for (SpatialNode *node : updateNormalsNodes) {
    update_node_normals(node);
  }
#else
  litestl::task::parallel_for(
      util::IndexRange(updateNormalsNodes.size()),
      [&](IndexRange range) {
        for (int i : range) {
          SpatialNode *node = updateNormalsNodes[i];
          update_node_normals(node);
        }
      },
      4);
#endif

  /* Phase: GPU partition assignment. Cheap walk (O(nodes)). If topology
   * didn't change we can skip recomputing counts, but the assignment
   * walk itself is still needed first time around. */
  if (topology_changed || !done_gpu_assignment) {
    recompute_subtree_tri_counts();
    assign_gpu_nodes();
  }

  /* Phase: propagate per-leaf GPU dirty bits to their owning GPU node
   * and rebuild/update those nodes' buffers. Full regens stay serial
   * (they call into gpu::GPUManager to allocate buffers); slice updates
   * write into disjoint sub-ranges of an already-allocated VBO and run
   * in parallel below. */
  struct SliceWork {
    SpatialNode *owner;
    SpatialNode *leaf;
  };
  Vector<SliceWork, 256> sliceWork;

  for (SpatialNode *node : nodes) {
    if (!(node->flag & Spatial_Leaf)) {
      continue;
    }
    NodeFlags want = node->flag & (Spatial_RegenGPU | Spatial_UpdateGPU);
    if (!want) {
      continue;
    }

    SpatialNode *owner = find_gpu_owner(node);
    if (!owner) {
      continue;
    }

    /* GPU-resident stroke (debug app): this owner's pos/nor are produced by
     * the scatter compute pass and must not be regenerated or slice-updated
     * from the CPU mesh mid-stroke (that would clobber the GPU result). The
     * stroke syncs the CPU mesh + clears gpu_owned at end. */
    if (gpuStrokeActive && owner->gpu_data && owner->gpu_data->pos &&
        owner->gpu_data->pos->gpu_owned) {
      node->flag &= ~(Spatial_RegenGPU | Spatial_UpdateGPU);
      continue;
    }

    /* Full regen of the owner if the owner is brand-new (no buffers),
     * the leaf wants a full regen, or the slice layout is missing. */
    bool need_full = !owner->gpu_data || !owner->gpu_data->pos ||
                     owner->gpu_data->slices.size() == 0 || (want & Spatial_RegenGPU);

    if (need_full) {
      regen_gpu_node(owner, gpu);
      drawBatchUpdated = true;
    } else {
      sliceWork.append({owner, node});
    }
  }

#ifdef NO_PARALLEL_FOR
  for (const SliceWork &w : sliceWork) {
    update_gpu_node_slice(w.owner, w.leaf, gpu);
  }
#else
  litestl::task::parallel_for(
      util::IndexRange(sliceWork.size()),
      [&](IndexRange range) {
        for (int i : range) {
          update_gpu_node_slice(sliceWork[i].owner, sliceWork[i].leaf, gpu);
        }
      },
      4);
#endif

  /* Phase: any GPU node still missing buffers (because it transitioned
   * from non-GPU to GPU this tick and contains no individually-dirty
   * leaves) needs a full rebuild. */
  for (SpatialNode *node : nodes) {
    if (!node->is_gpu_node) {
      continue;
    }
    if (!node->gpu_data || !node->gpu_data->pos) {
      regen_gpu_node(node, gpu);
      drawBatchUpdated = true;
    }
  }

  if (drawBatchUpdated || !drawBatch) {
    if (!drawBatch) {
      drawBatch = gpu->createBatch();
    } else {
      drawBatch->clear();
    }

    for (SpatialNode *node : nodes) {
      if (!node->is_gpu_node || !node->gpu_data || !node->gpu_data->pos) {
        continue;
      }

      GpuData &gd = *node->gpu_data;
      drawBatch->buffers.append(gd.pos);
      drawBatch->buffers.append(gd.nor);

      if (!gd.cmd) {
        gd.cmd = gpu->createCommand(drawBatch,
                                    gpu::GPUCmdType::DRAW_TRIS,
                                    &spatialShaders.basicMeshShader,
                                    0,
                                    gd.pos->size,
                                    gd.pos->size / 3);
        gd.cmd->attrs.append(gd.pos);
        gd.cmd->attrs.append(gd.nor);
      }
      gd.cmd->primCount = gd.pos->size / 3;
      gd.cmd->end = gd.pos->size;
      drawBatch->commands.append(gd.cmd);
    }
  }

  return result;
}
} // namespace sculptcore::spatial
