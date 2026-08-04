#include "spatial.h"
#include "shaders/spatial_shaders.h"

#include "node.h"

#include "litestl/math/geom.h"
#include "litestl/math/vector.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

// #include "litestl/util/map.h"
#include "litestl/util/rand.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>

#include "gpu/batch.h"
#include "gpu/command.h"
#include "gpu/manager.h"
#include "gpu/shader.h"
#include "gpu/types.h"
#include "gpu/uniform_link.h"
#include "gpu/vbo.h"

#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
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

void SpatialTree::setColorDisplayMode(int mode)
{
  if (mode == displayColorMode) {
    return;
  }
  displayColorMode = mode;
  /* The color stream is filled per-leaf-slice; flag every leaf so the next
   * update() re-runs fill_leaf_slice with the new source attribute. */
  for (SpatialNode *leaf : leaves()) {
    leaf->flag |= Spatial_UpdateGPU;
  }
}

void SpatialTree::setDisplayColorAttr(int index)
{
  if (index == displayColorAttr) {
    return;
  }
  displayColorAttr = index;
  for (SpatialNode *leaf : leaves()) {
    leaf->flag |= Spatial_UpdateGPU;
  }
}

void SpatialTree::setDisplayGroupAttr(int index)
{
  if (index == displayGroupAttr) {
    return;
  }
  displayGroupAttr = index;
  for (SpatialNode *leaf : leaves()) {
    leaf->flag |= Spatial_UpdateGPU;
  }
}

void SpatialTree::setDisplayMask(bool on)
{
  if (on == displayMask) {
    return;
  }
  displayMask = on;
  for (SpatialNode *leaf : leaves()) {
    leaf->flag |= Spatial_UpdateGPU;
  }
}

void SpatialTree::setDefaultGroupId(int group)
{
  if (m == nullptr || group == m->default_group_id) {
    return;
  }
  m->default_group_id = group;
  for (SpatialNode *leaf : leaves()) {
    leaf->flag |= Spatial_UpdateGPU;
  }
}

/* True if two requested sets are identical (same slots/names/types/order) — so
 * setRequestedAttrs can early-return and avoid a per-frame rebuild. */
static bool requested_attrs_equal(const util::Vector<gpu::RequestedAttr> &a,
                                  const util::Vector<gpu::RequestedAttr> &b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (int i : util::IndexRange(a.size())) {
    const gpu::RequestedAttr &x = a[i];
    const gpu::RequestedAttr &y = b[i];
    if (x.name != y.name || x.srcType != y.srcType || x.gpuType != y.gpuType ||
        x.elemSize != y.elemSize || x.slot != y.slot || x.domain != y.domain ||
        x.defaultKind != y.defaultKind)
    {
      return false;
    }
  }
  return true;
}

void SpatialTree::computeMissingAttrSlots()
{
  missingAttrSlots.clear();
  for (const gpu::RequestedAttr &req : requestedAttrs) {
    AttrGroup *grp = m->attrGroupForDomainFlag(req.domain);
    if (!grp || !grp->has(AttrType(req.srcType), req.name)) {
      missingAttrSlots.append(req.slot);
    }
  }
}

void SpatialTree::setRequestedAttrs(const util::Vector<gpu::RequestedAttr> &reqs)
{
  /* Store canonically in slot order. The per-attribute buffers are built and
   * filled in requestedAttrs *index* order (regen_gpu_node / fill paths in
   * spatial_gpu.cc) and bound to cmd->attrs in that same order, while the
   * ShaderDef vertex layout is in *slot* order. Sorting here makes index order
   * == slot order, so the two agree by construction no matter what order the
   * caller passes the set in (it never depends on the caller pre-sorting). */
  util::Vector<gpu::RequestedAttr> sorted;
  for (const gpu::RequestedAttr &req : reqs) {
    sorted.append(req);
  }
  /* selection sort by slot — the set is tiny (one entry per material attr) */
  for (int n = 0; n < int(sorted.size()); n++) {
    int best = n;
    for (int i = n + 1; i < int(sorted.size()); i++) {
      if (sorted[i].slot < sorted[best].slot) {
        best = i;
      }
    }
    if (best != n) {
      gpu::RequestedAttr tmp = sorted[n];
      sorted[n] = sorted[best];
      sorted[best] = tmp;
    }
  }

  if (requested_attrs_equal(requestedAttrs, sorted)) {
    /* Unchanged — refresh the missing-attr advisory (mesh layers may have come
     * or gone) but do not force a GPU rebuild. */
    computeMissingAttrSlots();
    return;
  }

  requestedAttrs.clear();
  for (const gpu::RequestedAttr &req : sorted) {
    requestedAttrs.append(req);
  }
  requestedAttrsVersion++;

  computeMissingAttrSlots();

  /* Existing draw commands reference stale buffers/shader; drop the batch so it
   * rebuilds, and flag every leaf for a full GPU regen (rare — only on material
   * edits). */
  if (drawBatch) {
    alloc::Delete(drawBatch);
    drawBatch = nullptr;
  }
  for (SpatialNode *leaf : leaves()) {
    leaf->flag |= Spatial_RegenGPU;
  }
}

void SpatialTree::setDrawShader(const char *wgsl)
{
  // Empty WGSL reverts to the built-in basic mesh shader (drawShaderReady=false),
  // NOT a degenerate empty material shader. Used when the viewport leaves
  // rendered mode (SHOW_RENDER off) so the solid draw works again (#1).
  if (!wgsl || wgsl[0] == '\0') {
    drawShaderReady = false;
    if (drawBatch) {
      alloc::Delete(drawBatch);
      drawBatch = nullptr;
    }
    for (SpatialNode *leaf : leaves()) {
      leaf->flag |= Spatial_RegenGPU;
    }
    return;
  }

  /* Attr layout: position@0, normal@1, then requestedAttrs. The set is stored
   * slot-ordered by setRequestedAttrs (and the buffers are bound in that same
   * order), so a straight append already lands each attr at its @location. */
  util::Vector<gpu::AttrDef> attrs;
  attrs.append({util::string("position"), gpu::GPUType::FLOAT32, 3});
  attrs.append({util::string("normal"), gpu::GPUType::FLOAT32, 3});

  for (const gpu::RequestedAttr &req : requestedAttrs) {
    attrs.append({req.name, req.gpuType, req.elemSize});
  }

  /* DefaultBlock UBO (drawMatrix + normalMatrix + uColor), mirroring the basic
   * mesh shader's block. The material's own uniform schema is refined when the
   * renderengine wiring lands (M6); this keeps the def linkable meanwhile. */
  util::Vector<gpu::UniformDefBase *> fields;
  fields.append(alloc::New<gpu::UniformDef<mat4>>(
      "UniformDef", "drawMatrix", gpu::GPUType::FLOAT32, 16, mat4().identity()));
  fields.append(alloc::New<gpu::UniformDef<mat4>>(
      "UniformDef", "normalMatrix", gpu::GPUType::FLOAT32, 16, mat4().identity()));
  fields.append(alloc::New<gpu::UniformDef<float4>>(
      "UniformDef", "uColor", gpu::GPUType::FLOAT32, 4, float4(1.0f, 1.0f, 1.0f, 1.0f)));
  auto *block = alloc::New<gpu::UniformBlockDef>(
      "UniformBlockDef", util::string("DefaultBlock"), std::move(fields));
  block->set = 0;
  block->binding = 0;

  util::Vector<gpu::UniformBlockDef *> uniforms;
  uniforms.append(block);

  drawShader = gpu::ShaderDef(util::string("Spatial Material Shader"),
                              util::string(wgsl),
                              std::move(attrs),
                              std::move(uniforms),
                              {});
  gpu::linkShaderDef(&drawShader);
  drawShaderReady = true;

  /* Commands hold the old shader pointer; force them to rebuild. */
  if (drawBatch) {
    alloc::Delete(drawBatch);
    drawBatch = nullptr;
  }
  for (SpatialNode *leaf : leaves()) {
    leaf->flag |= Spatial_RegenGPU;
  }
}

void SpatialTree::refreshRequestedAttrs()
{
  /* Force a rebuild even when the requested descriptor set is byte-identical:
   * adding/removing a mesh layer whose domain matches the category default
   * leaves the descriptors unchanged, so setRequestedAttrs would short-circuit
   * and the per-attribute buffers would stay default-filled. The shader/ShaderDef
   * is unaffected (the layout depends only on the descriptors), so we do *not*
   * relink it — only the buffer contents need to re-gather. */
  computeMissingAttrSlots();
  requestedAttrsVersion++;

  if (drawBatch) {
    alloc::Delete(drawBatch);
    drawBatch = nullptr;
  }
  for (SpatialNode *leaf : leaves()) {
    leaf->flag |= Spatial_RegenGPU;
  }
}

/* Fan-triangulate face `f` into `out`, exactly as regen_node_tris always has.
 * TODO: use a property CDT for > 4 vert or > 1 hole faces.
 * For now just handle triangles and quads. */
static void appendFaceTris(Mesh *m, util::Vector<NodeTri> &out, int f)
{
  int l = m->f.l[f];
  int c = m->l.c[l];

  NodeTri &tri = out.grow_one();
  tri.c[0] = c;
  tri.c[1] = m->c.next[c];
  tri.c[2] = m->c.next[tri.c[1]];
  tri.f = f;

  if (m->l.size[l] > 3) {
    NodeTri &tri2 = out.grow_one();

    tri2.c[0] = c;
    tri2.c[1] = m->c.next[m->c.next[c]];
    tri2.c[2] = m->c.next[tri2.c[1]];
    tri2.f = f;
  }
}

void SpatialTree::regen_node_tris(SpatialNode *node)
{
  node->flag &= ~Spatial_RegenTris;

  /* Topology changed — the affected_verts hints no longer cover everything
   * needing new normals. Drop them and arm the sticky full-rebuild marker (see
   * Spatial_NormalsFullRebuild); whether normals are dirty at all stays the
   * flagger's call, so Spatial_UpdateNormals is deliberately NOT set here. */
  node->affected_verts.clear_and_contract();
  node->flag |= Spatial_NormalsFullRebuild;

  /* Own-node flag only (parallel-safe): regens also happen outside updateImpl
   * (initial build, split/merge), where the skirt phase's updateTriNodes walk
   * never sees them — self-arming keeps the skirt phase's collection loop the
   * single rebuild site. */
  node->flag |= Spatial_RegenSkirt;

  node->data->tris.clear_and_contract();
  for (int f : node->data->unique_faces) {
    appendFaceTris(m, node->data->tris, f);
  }

  node->data->foreign_verts.clear_and_contract();
  node->data->border_tris.clear_and_contract();
  node->data->foreign_verts_valid = false;
}

void SpatialTree::build_node_skirt(SpatialNode *node)
{
  node->flag &= ~Spatial_RegenSkirt;
  node->data->skirt_tris.clear_and_contract();

  auto &node_fattr = node->treeMesh->f.node;
  util::Set<int> seen;

  /* Walk each owned vert's face fan (disk cycle -> radial cycles); faces owned
   * elsewhere are this leaf's skirt. Requires live topology — callers run in
   * the queries-half skirt phase, which thaws first (same contract as
   * regen_node_tris). */
  for (int v : node->data->unique_verts) {
    int e0 = m->v.e[v];
    if (e0 == ELEM_NONE) {
      continue;
    }
    for (int e : mesh::EdgeOfVertIter(m, v, e0)) {
      int c0 = m->e.c[e];
      if (c0 == ELEM_NONE) {
        continue; // wire edge
      }
      int c = c0;
      do {
        int f = m->l.f[m->c.l[c]];
        if (node_fattr[f] != node->id && !seen.contains(f)) {
          seen.add(f);
          appendFaceTris(m, node->data->skirt_tris, f);
        }
        c = m->c.radial_next[c];
      } while (c != c0 && c != ELEM_NONE);
    }
  }
}

void SpatialTree::ensure_border_cache(SpatialNode *node)
{
  if (node->data->foreign_verts_valid) {
    return;
  }
  util::Set<int> seen;
  const util::Vector<NodeTri> &tris = node->data->tris;
  for (int i : util::IndexRange(tris.size())) {
    bool border = false;
    for (int j = 0; j < 3; j++) {
      int v = m->c.v[tris[i].c[j]];
      if (treeMesh.v.node[v] == node->id) {
        continue;
      }
      border = true;
      if (!seen.contains(v)) {
        seen.add(v);
        node->data->foreign_verts.append(v);
      }
    }
    if (border) {
      node->data->border_tris.append(i);
    }
  }
  node->data->foreign_verts_valid = true;
}

void SpatialTree::add_face_intern(SpatialNode *node, int f, float3 &fcent, int claimTag)
{
  if ((node->flag & Spatial_Leaf) && node_needs_split(node)) {
    split_node(node, claimTag);
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
    add_face_intern(child, f, fcent, claimTag);
    return;
  }

  /* RegenGPU too: a leaf that gains a face needs its GPU node's VBO rebuilt
   * with the new tris (the incremental dyntopo path relies on this — without it
   * the new geometry never reaches the renderer). split_node already sets it on
   * fresh child leaves; this covers adding into an existing leaf. */
  node->flag |= Spatial_RegenTris | Spatial_RegenBounds | Spatial_RegenGPU;
  FaceProxy face(m, f);

  if (treeMesh.f.node[face] == 0) {
    treeMesh.f.node[face] = node->id;
    node->data->unique_faces.add(f);
  }

  for (auto list : face.lists()) {
    for (auto c : list) {
      int vn = treeMesh.v.node[c.v()];
      if (vn == node->id) {
        continue; /* already this leaf's unique vert  */
      }
      /* Claim only verts carrying this split's own unassign tag (0 outside a
       * split). A concurrent candidate's verts show its tag or a positive id
       * — never claimable here, so parallel candidates stay disjoint. */
      if (vn == claimTag) {
        node->data->unique_verts.add(c.v());
        treeMesh.v.node[c.v()] = node->id;
      }
    }
  }
}

/* Bit-identical to FaceProxy::calc_center(), walking the loop/corner columns
 * directly — the proxy-iterator overhead dominated the split re-file (M2.a). */
static float3 face_calc_center_direct(Mesh *m, int f)
{
  float tot = 0.0f;
  float3 cent(0.0f);
  for (int l = m->f.l[f]; l != ELEM_NONE; l = m->l.next[l]) {
    int c0 = m->l.c[l];
    if (c0 == ELEM_NONE) {
      continue;
    }
    int c = c0;
    do {
      cent += m->v.co[m->c.v[c]];
      tot += 1.0f;
      c = m->c.next[c];
    } while (c != c0);
  }
  if (tot == 0.0f) {
    return cent;
  }
  return cent / tot;
}

void SpatialTree::split_node(SpatialNode *node, int claimTag)
{
  node->children[0] = alloc_node();
  node->children[1] = alloc_node();

  using namespace litestl::math;
  const float3 min(node->aabb.min), max(node->aabb.max);
  float3 mean(0.0f);

  for (int v : node->data->unique_verts) {
    VertProxy vert(m, v);
    mean += vert.co();

    /* Unassign verts (claimTag = this split's private marker; 0 serially). */
    treeMesh.v.node[v] = claimTag;
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

  /* Re-filing does not re-triangulate: add_face_intern routes purely by the
   * centroid, and every face here already passed add_face's validity check
   * when it entered the tree (M2.a — the old triangulateFace call was dead
   * work, ~42% of the split pass). */
  for (int f : node->data->unique_faces) {
    if (m->f.freemap[f]) {
      continue; /* tolerate a stale entry from incremental removal */
    }
    float3 fcent = face_calc_center_direct(m, f);

    // unassign face
    treeMesh.f.node[f] = 0;

    add_face_intern(node, f, fcent, claimTag);
  }

  /* A vert the node owned but that no re-filed face referenced (its only
   * incident faces are owned by *other* leaves) would otherwise be lost — the
   * unassign loop set v.node=0 and the face re-file never re-claimed it. Sort it
   * to the leaf whose region contains it, so coverage stays complete (sum
   * unique_verts == count). The face re-file above can recursively split a child
   * (add_face_intern's inline-split path), so node->children[i] may no longer be
   * a leaf and its data may be gone — descend to the current leaf by the same
   * centroid routing add_face_intern uses. Mirrors merge_node's orphan recovery. */
  for (int v : node->data->unique_verts) {
    if (m->v.freemap[v] || treeMesh.v.node[v] != claimTag) {
      continue;
    }
    float3 vco = VertProxy(m, v).co();
    SpatialNode *leaf = node;
    while (!(leaf->flag & Spatial_Leaf)) {
      SpatialNode *c0 = leaf->children[0];
      SpatialNode *c1 = leaf->children[1];
      int ax = 0;
      for (int i = 0; i < 3; i++) {
        if (c0->aabb.max[i] != c1->aabb.max[i]) {
          ax = i;
          break;
        }
      }
      leaf = vco[ax] <= c0->aabb.max[ax] ? c0 : c1;
    }
    treeMesh.v.node[v] = leaf->id;
    leaf->data->unique_verts.add(v);
  }

  node->delete_data();
  node->flag |= Spatial_RegenBounds;
}

void SpatialTree::applyDeferredNodeSplit()
{
  if (nodeSplitCandidates_.size() == 0) {
    return;
  }

  /* split_node walks the leaf's faces through the live face/loop links, which
   * are dropped in frozen-topology mode — thaw first (one thaw covers the
   * whole pass; the next dab re-freezes). */
  if (m->topo_frozen) {
    m->thawTopo();
  }

  /* Each over-full leaf is split exactly once here; split_node itself recurses
   * (its re-insert goes through add_face_intern's inline-split path), so one call
   * turns a leaf that gained ~1500 verts in a dab into a balanced subtree —
   * replacing the N threshold-crossing re-inserts the inline path used to do.
   * Candidates are resolved up-front: nothing in the split pass may read
   * node_idmap once splits run in parallel (alloc_node grows it). */
  Vector<SpatialNode *, 32> cands;
  for (int leafId : nodeSplitCandidates_) {
    SpatialNode *node = node_from_id(leafId);
    if (node && (node->flag & Spatial_Leaf) && node->data && node_needs_split(node)) {
      cands.append(node);
    }
  }
  nodeSplitCandidates_.clear();
  leafCacheDirty_ = true;

  if (cands.size() == 0) {
    return;
  }

  /* Split candidates are disjoint leaf subtrees: each split touches only its
   * own subtree nodes, its own geometry's `.spatial.{v,f}.node` entries, and
   * the shared node bookkeeping serialized inside alloc_node. So the
   * candidates can split in parallel; the id/order nondeterminism from
   * parallel completion is erased by the renumbering epilogue below. */
  // SC_SPLIT_SERIAL=1 forces the serial path (regression A/Bs).
  static const bool forceSerialSplit = std::getenv("SC_SPLIT_SERIAL") != nullptr;

#ifdef NO_PARALLEL_FOR
  const bool serialSplit = true;
#else
  const bool serialSplit = forceSerialSplit || cands.size() == 1;
#endif

  if (serialSplit) {
    for (SpatialNode *node : cands) {
      split_node(node);
    }
    return;
  }

#ifndef NO_PARALLEL_FOR
  const int preIdGen = node_idgen;
  const int preNodes = int(nodes.size());

  litestl::task::parallel_for(
      util::IndexRange(cands.size()),
      [&](util::IndexRange range) {
        for (int i : range) {
          /* Unique negative claim tag per candidate — see split_node's doc
           * comment (prevents cross-candidate boundary-vert claims). */
          split_node(cands[i], -(i + 1));
        }
      },
      1);

  /* Deterministic renumbering epilogue: parallel completion order made the
   * new nodes' ids and their order in `nodes` nondeterministic. Reassign both
   * in candidate order (pre-order DFS per candidate — deterministic because
   * the tree *structure* is: routing math is unaffected by scheduling), and
   * rewrite the new leaves' ownership columns to the new ids. Undo parity and
   * cross-backend A/Bs rely on run-to-run identical trees. */
  Vector<SpatialNode *, 64> ordered;
  for (SpatialNode *cand : cands) {
    /* cand itself kept its id; its descendants are exactly this pass's new
     * nodes (a candidate was a leaf, so everything below it is fresh). */
    Vector<SpatialNode *, 16> stack;
    stack.append(cand);
    while (stack.size() > 0) {
      SpatialNode *n = stack.pop_back();
      if (n != cand) {
        ordered.append(n);
      }
      if (!(n->flag & Spatial_Leaf)) {
        /* Push children[1] first so children[0] pops (pre-order) first. */
        if (n->children[1]) {
          stack.append(n->children[1]);
        }
        if (n->children[0]) {
          stack.append(n->children[0]);
        }
      }
    }
  }
  if (int(ordered.size()) != int(nodes.size()) - preNodes) {
    fprintf(stderr,
            "applyDeferredNodeSplit: renumber walk found %d nodes, expected %d\n",
            int(ordered.size()), int(nodes.size()) - preNodes);
  }

  /* Clear the parallel-assigned idmap slots, then reassign sequentially. */
  for (int i = preNodes; i < int(nodes.size()); i++) {
    if (nodes[i]->id < int(node_idmap.size())) {
      node_idmap[nodes[i]->id] = nullptr;
    }
  }
  for (int k = 0; k < int(ordered.size()); k++) {
    SpatialNode *n = ordered[k];
    n->id = preIdGen + k;
    n->index = preNodes + k;
    nodes[preNodes + k] = n;
    if (n->id >= int(node_idmap.size())) {
      node_idmap.resize(n->id + 1);
    }
    node_idmap[n->id] = n;

    /* Re-stamp this leaf's owned geometry with the renumbered id. */
    if ((n->flag & Spatial_Leaf) && n->data) {
      for (int v : n->data->unique_verts) {
        treeMesh.v.node[v] = n->id;
      }
      for (int f : n->data->unique_faces) {
        treeMesh.f.node[f] = n->id;
      }
    }
  }
  node_idgen = preIdGen + int(ordered.size());
#endif
}

void SpatialTree::free_node(SpatialNode *n)
{
  /* Swap-remove from `nodes` so castRay's node->index == position invariant
   * (node.h: out.nodeIndex = index; spatial.h: nodes[out.nodeIndex]) holds for
   * the node moved into the gap. */
  int idx = n->index;
  int last = int(nodes.size()) - 1;
  if (idx != last) {
    nodes[idx] = nodes[last];
    nodes[idx]->index = idx;
  }
  nodes.remove_at(last, /*swap_end_only=*/true);

  if (n->id < int(node_idmap.size())) {
    node_idmap[n->id] = nullptr;
  }
  leafCacheDirty_ = true;
  gpuNodeCacheDirty_ = true;

  alloc::Delete(n);
}

void SpatialTree::merge_node(SpatialNode *parent)
{
  SpatialNode *c0 = parent->children[0];
  SpatialNode *c1 = parent->children[1];

  /* Unassign the subtree's owned geometry, collecting the owned faces and the
   * owned verts so the re-file below re-sorts them into the merged leaf. */
  Vector<int> faces, verts;
  for (SpatialNode *c : {c0, c1}) {
    for (int v : c->data->unique_verts) {
      treeMesh.v.node[v] = 0;
      verts.append(v);
    }
    for (int f : c->data->unique_faces) {
      treeMesh.f.node[f] = 0;
      if (!m->f.freemap[f]) {
        faces.append(f);
      }
    }
  }

  /* Parent becomes a leaf again and re-absorbs the faces through the build path
   * (add_face_intern's leaf body re-derives unique ownership). In the
   * under-full/skew bands the combined count stays below leaf_limit, so the merged
   * leaf does not re-split; the skew path's win is removing the wasted level. (A
   * caller that merged an over-full pair would auto re-split here via
   * add_face_intern — the mean-split predictor guards that against thrash.) */
  parent->create_data();
  parent->children[0] = parent->children[1] = nullptr;
  parent->flag |= Spatial_Leaf | Spatial_RegenTris | Spatial_RegenBounds |
                  Spatial_RegenGPU | Spatial_UpdateNormals;

  /* No re-triangulation — centroid routing only (see split_node's re-file). */
  for (int f : faces) {
    float3 fcent = face_calc_center_direct(m, f);
    add_face_intern(parent, f, fcent);
  }

  /* A vert the subtree owned but that no re-filed face referenced (it is only
   * touched by faces owned *outside* the subtree) would otherwise be orphaned.
   * It still belongs to the merged region geometrically, so give it to the
   * parent leaf — keeps ownership coverage complete (sum unique_verts == count). */
  for (int v : verts) {
    if (!m->v.freemap[v] && treeMesh.v.node[v] == 0) {
      treeMesh.v.node[v] = parent->id;
      parent->data->unique_verts.add(v);
    }
  }

  for (SpatialNode *p = parent->parent; p; p = p->parent) {
    p->flag |= Spatial_RegenBounds;
  }

  free_node(c0);
  free_node(c1);
}

/* Predict split_node's clamp decision for a fresh split of `verts` inside `box`:
 * mirrors split_node's longest-axis + mean t (spatial.cc split_node). Returns true
 * when t lands comfortably interior (re-splitting would rebalance), false when it
 * clamps (the skew is the best a mean split can do — collapsing it just invites the
 * split path to recreate it, i.e. thrash). Tighter bound than split's 0.01/0.99. */
static bool
mean_split_interior(Mesh *m, const SpatialNode::AABB &box, const Vector<int> &verts)
{
  if (verts.size() == 0) {
    return false;
  }
  float3 size = box.size();
  int axis = 0;
  for (int i = 1; i < 3; i++) {
    if (size[i] > size[axis]) {
      axis = i;
    }
  }
  if (size[axis] <= 0.0f) {
    return false;
  }
  float sum = 0.0f;
  for (int v : verts) {
    sum += VertProxy(m, v).co()[axis];
  }
  float t = (sum / float(verts.size()) - box.min[axis]) / size[axis];
  return t > 0.02f && t < 0.98f;
}

/* Sum a subtree's owned (unique) verts, stopping early once `cap` is reached. */
static int subtree_vert_count_capped(SpatialNode *n, int cap)
{
  if (n->flag & Spatial_Leaf) {
    return n->data ? int(n->data->unique_verts.size()) : 0;
  }
  int a = subtree_vert_count_capped(n->children[0], cap);
  if (a >= cap) {
    return a;
  }
  return a + subtree_vert_count_capped(n->children[1], cap);
}

/* Collect a subtree's owned (unique) faces and verts into `faces` / `verts`. */
static void gather_subtree(SpatialNode *n, Vector<int> &faces, Vector<int> &verts)
{
  if (n->flag & Spatial_Leaf) {
    if (n->data) {
      for (int v : n->data->unique_verts) {
        verts.append(v);
      }
      for (int f : n->data->unique_faces) {
        faces.append(f);
      }
    }
    return;
  }
  gather_subtree(n->children[0], faces, verts);
  gather_subtree(n->children[1], faces, verts);
}

bool SpatialTree::node_is_skewed(SpatialNode *parent)
{
  SpatialNode *c0 = parent->children[0];
  SpatialNode *c1 = parent->children[1];
  if (!c0 || !c1 || !(c0->flag & Spatial_Leaf) || !(c1->flag & Spatial_Leaf) ||
      !c0->data || !c1->data)
  {
    return false; /* internal child: subtree_wants_collapse handles that */
  }

  int n0 = int(c0->data->unique_verts.size());
  int n1 = int(c1->data->unique_verts.size());
  int total = n0 + n1;
  int lo = std::min(n0, n1);

  /* Band between the under-full watermark (which the existing merge owns) and
   * leaf_limit (at/above which a merged leaf re-splits unconditionally). */
  if (total < leaf_limit / 2 || total >= leaf_limit) {
    return false;
  }

  /* H1 count skew: one child near-empty, absolutely (leaf_limit/8) and
   * relatively (< 15% of the pair). */
  bool skewed = lo <= leaf_limit / 8 && lo * 20 < total * 3;

  /* H2 deformation skew: child AABBs interpenetrate, so the split plane no longer
   * separates the geometry (verts drifted across it). Reads last frame's tightened
   * bounds — applyDeferredMerge runs before this frame's regen_node_bounds. */
  if (!skewed) {
    float3 s0 = c0->aabb.size(), s1 = c1->aabb.size();
    float vol0 = s0[0] * s0[1] * s0[2], vol1 = s1[0] * s1[1] * s1[2];
    float ov = 1.0f;
    for (int i = 0; i < 3; i++) {
      float a = std::max(c0->aabb.min[i], c1->aabb.min[i]);
      float b = std::min(c0->aabb.max[i], c1->aabb.max[i]);
      ov *= std::max(0.0f, b - a);
    }
    skewed = ov > 0.5f * std::min(vol0, vol1);
  }

  if (!skewed) {
    return false;
  }

  /* Thrash guard: only act if a fresh mean split of the combined verts would land
   * interior (bounded: total < leaf_limit). */
  Vector<int> verts;
  for (SpatialNode *c : {c0, c1}) {
    for (int v : c->data->unique_verts) {
      verts.append(v);
    }
  }
  return mean_split_interior(m, parent->aabb, verts);
}

bool SpatialTree::subtree_wants_collapse(SpatialNode *parent)
{
  SpatialNode *c0 = parent->children[0];
  SpatialNode *c1 = parent->children[1];
  if (!c0 || !c1) {
    return false;
  }

  int n0 = subtree_vert_count_capped(c0, leaf_limit);
  if (n0 >= leaf_limit) {
    return false; /* a child already too large; never collapse to rebuild it */
  }
  int n1 = subtree_vert_count_capped(c1, leaf_limit);
  int total = n0 + n1;
  if (total >= leaf_limit || total < leaf_limit / 2) {
    return false;
  }

  int lo = std::min(n0, n1);
  if (!(lo <= leaf_limit / 8 && lo * 20 < total * 3)) {
    return false;
  }

  Vector<int> faces, verts;
  gather_subtree(parent, faces, verts);
  return mean_split_interior(m, parent->aabb, verts);
}

void SpatialTree::free_subtree(SpatialNode *n)
{
  if (!(n->flag & Spatial_Leaf)) {
    free_subtree(n->children[0]);
    free_subtree(n->children[1]);
  }
  free_node(n);
}

void SpatialTree::collapse_subtree(SpatialNode *node)
{
  /* Gather the subtree's owned geometry before freeing it (mirrors merge_node:
   * only unique_* — other_* are owned outside and stay there). */
  Vector<int> faces, verts;
  gather_subtree(node, faces, verts);

  for (int v : verts) {
    treeMesh.v.node[v] = 0;
  }
  for (int f : faces) {
    treeMesh.f.node[f] = 0;
  }

  free_subtree(node->children[0]);
  free_subtree(node->children[1]);

  node->create_data();
  node->children[0] = node->children[1] = nullptr;
  node->flag |= Spatial_Leaf | Spatial_RegenTris | Spatial_RegenBounds |
                Spatial_RegenGPU | Spatial_UpdateNormals;

  /* No re-triangulation — centroid routing only (see split_node's re-file). */
  for (int f : faces) {
    if (m->f.freemap[f]) {
      continue;
    }
    float3 fcent = face_calc_center_direct(m, f);
    add_face_intern(node, f, fcent);
  }

  /* Orphan-recovery, as in merge_node: a vert no re-filed face referenced still
   * belongs to the collapsed region. */
  for (int v : verts) {
    if (!m->v.freemap[v] && treeMesh.v.node[v] == 0) {
      treeMesh.v.node[v] = node->id;
      node->data->unique_verts.add(v);
    }
  }

  for (SpatialNode *p = node->parent; p; p = p->parent) {
    p->flag |= Spatial_RegenBounds;
  }
}

void SpatialTree::applyDeferredMerge()
{
  std::function<void(SpatialNode *)> recurse = [&](SpatialNode *node) {
    if (node->parent && node_is_skewed(node->parent)) {
      mergeCandidates_.add(node->parent->id);
    }

    if (!(node->flag & Spatial_Leaf)) {
      recurse(node->children[0]);
      recurse(node->children[1]);
    }
  };
  recurse(root);

  if (mergeCandidates_.size() == 0) {
    return;
  }

  /* merge_node re-triangulates through the live face/loop links (frozen-dropped
   * in stroke mode) — thaw once for the whole pass. */
  if (m->topo_frozen) {
    m->thawTopo();
  }

  /* Hysteresis: only merge when the pair owns well under leaf_limit, so the
   * merged leaf doesn't immediately re-cross the split threshold. */
  const int watermark = leaf_limit / 2;

  /* Worklist (not a range-for: merging pushes the grandparent, which may now be
   * mergeable too, so merges cascade up a chain in one pass). */
  Vector<int> work;
  for (int id : mergeCandidates_) {
    work.append(id);
  }
  mergeCandidates_.clear();

  for (int wi = 0; wi < int(work.size()); wi++) {
    int id = work[wi];
    if (id >= int(node_idmap.size())) {
      continue;
    }
    SpatialNode *parent = node_idmap[id];
    if (!parent || (parent->flag & Spatial_Leaf)) {
      continue; /* freed, or already a leaf */
    }
    SpatialNode *c0 = parent->children[0];
    SpatialNode *c1 = parent->children[1];
    if (!c0 || !c1) {
      continue;
    }
    SpatialNode *gp = parent->parent;

    bool twoLeaves =
        (c0->flag & Spatial_Leaf) && (c1->flag & Spatial_Leaf) && c0->data && c1->data;
    if (twoLeaves) {
      int combined = int(c0->data->unique_verts.size() + c1->data->unique_verts.size());
      /* Under-full pair (existing merge), or a lopsided/deformed one whose split
       * level is wasted (rebalance, M7.6c). Either way fold into one leaf. */
      if (combined < watermark || node_is_skewed(parent)) {
        merge_node(parent);
        if (gp) {
          work.append(gp->id); /* grandparent may now be mergeable too */
        }
      }
      continue;
    }

    /* Deep case: a child is internal, so merge_node can't act. Collapse the whole
     * subtree back to a leaf when it now fits under leaf_limit and is lopsided. */
    if (subtree_wants_collapse(parent)) {
      collapse_subtree(parent);
      if (gp) {
        work.append(gp->id);
      }
    }
  }

  leafCacheDirty_ = true;
}

bool SpatialTree::regenDirtyBounds()
{
  bool bounds = false;
  Vector<SpatialNode *, 256> dirtyLeaves;
  for (SpatialNode *node : nodes) {
    if (node->flag & Spatial_RegenBounds) {
      if (node->flag & Spatial_Leaf) {
        dirtyLeaves.append(node);
      }
      while (node) {
        node->flag |= Spatial_RegenBounds;
        node = node->parent;
        bounds = true;
      }
    }
  }
  if (bounds) {
    /* Leaf refits are independent (each reads mesh columns, writes its own
     * AABB) and dominate the refit cost — run them in parallel, then let the
     * serial descent union the fresh leaf boxes up the dirty paths (a cleared
     * leaf flag stops its recursion). */
#ifndef NO_PARALLEL_FOR
    litestl::task::parallel_for(
        util::IndexRange(dirtyLeaves.size()),
        [&](IndexRange range) {
          for (int i : range) {
            regen_node_bounds(dirtyLeaves[i], false);
          }
        },
        4);
#endif
    regen_node_bounds(root, true);
  }
  return bounds;
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

    /* An empty leaf keeps the reset() sentinel ([+max, -max]); padding it would
     * overflow (max - min == -inf) and seed inf into the bounds. */
    if (!node->aabb.isEmpty()) {
      float3 eps = calc_eps_float3(node->aabb.max - node->aabb.min);

      node->aabb.min -= eps;
      node->aabb.max += eps;

      /* Displacement-bound padding (REYES-style): grow the leaf box by the max
       * per-face max|D| bound over the owned faces, so ray/cone/frustum queries
       * stay conservative against the displaced surface. Internal nodes union
       * their children, so the pad propagates up for free. */
      if (hasDetailBounds) {
        float pad = 0.0f;
        for (int f : node->data->unique_faces) {
          pad = std::max(pad, treeMesh.f.bound.get_data()->safe_get(f));
        }
        if (pad > 0.0f) {
          node->aabb.min -= float3(pad);
          node->aabb.max += float3(pad);
        }
      }
    }
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
  // Partition changes invalidate cached scatter tables (gpu_data moves).
  gpuLayoutGen++;
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
  /* The parallel top-down build is the default; SC_SERIAL_BUILD forces the
   * reference incremental build (A/B correctness + fallback). */
  if (getenv("SC_SERIAL_BUILD")) {
    buildAllSerial();
  }
  else {
    buildAllParallel();
  }
}

void SpatialTree::buildAllSerial()
{
  setup();

  /* Clear any leaf-ownership a previous tree left on the mesh: the
   * .spatial.{v,f}.node attrs outlive the tree, and a fresh tree's node ids
   * restart at 1, so stale ids are meaningless here. Without this, building a
   * second tree on the same mesh sees every elem already owned -> unique_verts
   * stays 0 -> nothing splits -> a single empty leaf. */
  for (int v : m->v) {
    treeMesh.v.node[v] = 0;
  }
  for (int f : m->f) {
    treeMesh.f.node[f] = 0;
  }

  m->calcAABB(&root->aabb.min, &root->aabb.max);
  m->recalc_normals();
  float eps = 0.0000001f;
  root->aabb.min -= eps;
  root->aabb.max += eps;

  // insert faces in random order
  // to balance tree better
  litestl::util::Random rnd(0);
  Vector<int> faces;
  faces.ensure_capacity(m->f.count);

  for (int f : m->f) {
    faces.append(f);
  }

  int n = faces.size();
  for (int i = 0; i < (n >> 1); i++) {
    int ri = rnd.get_int() % n;
    std::swap(faces[i], faces[ri]);
  }

  for (int i = 0; i < n; i++) {
    add_face(faces[i], false);
  }

  /* regen_node_bounds derives leaf AABBs from each node's tris (read via the
   * frozen-safe .corner.v column), so the tris must be built first. */
  for (SpatialNode *node : nodes) {
    if (node->flag & Spatial_Leaf) {
      ensure_node_tris(node);
    }
  }

  regen_node_bounds(root, true);

  // balance
  for (SpatialNode *node : nodes) {
    mergeCandidates_.add(node->id);
  }
  applyDeferredMerge();
  regen_node_bounds(root, true);

  for (SpatialNode *node : nodes) {
    if (node->flag & Spatial_Leaf) {
      ensure_node_tris(node);
    }
  }
}

namespace {
/* One face plus its centroid; the top-down partition reorders these in place
 * so each node owns a contiguous sub-range. */
struct BuildFace {
  int f;
  litestl::math::float3 cent;
};

/* A node awaiting split/finalization plus its [start, start+count) slice of the
 * BuildFace array. */
struct BuildFrontier {
  SpatialNode *node;
  int start;
  int count;
};
} // namespace

void SpatialTree::buildAllParallel()
{
  using litestl::math::float3;
  namespace task = litestl::task;
  using util::IndexRange;

  setup();

  /* Reset any stale leaf-ownership a previous tree left on the mesh (see
   * buildAllSerial). */
  for (int v : m->v) {
    treeMesh.v.node[v] = 0;
  }
  for (int f : m->f) {
    treeMesh.f.node[f] = 0;
  }

  m->calcAABB(&root->aabb.min, &root->aabb.max);
  m->recalc_normals();
  const float eps = 0.0000001f;
  root->aabb.min -= eps;
  root->aabb.max += eps;

  /* Gather live faces, then compute centroids in parallel. */
  Vector<BuildFace> faces;
  faces.ensure_capacity(m->f.count);
  for (int f : m->f) {
    faces.append(BuildFace{f, float3(0.0f)});
  }
  const int nfaces = faces.size();
  if (nfaces == 0) {
    /* Empty mesh: the root stays an (empty) leaf. */
    ensure_node_tris(root);
    regen_node_bounds(root, true);
    return;
  }
  task::parallel_for(
      IndexRange(nfaces),
      [&](IndexRange range) {
        for (int i : range) {
          faces[i].cent = face_calc_center_direct(m, faces[i].f);
        }
      },
      1024);

  /* leaf_limit counts vertices (node_needs_split), but the top-down partition
   * works in faces. A leaf owns roughly `faces * verts/faces` disjoint verts,
   * so scale the face threshold by the mesh-wide F/V ratio to land near
   * leaf_limit verts per leaf — matching the serial build's leaf granularity
   * (which the reorder/merge machinery is tuned for). */
  const int liveVertCount = m->v.count > 0 ? m->v.count : 1;
  const int leafFaceLimit = std::max<int>(
      1, int(int64_t(leaf_limit) * nfaces / liveVertCount));

  /* Level-synchronous top-down partition. Each frontier node owns a disjoint
   * [start, count) slice of `faces`; splitting reorders only that slice and
   * allocates two children (alloc_node is mutex-guarded), so nodes at one level
   * process in parallel without sharing state. Nested parallel_for is avoided
   * (the pool is bounded) — parallelism comes from the many nodes per level. */
  Vector<BuildFrontier> frontier;
  frontier.append(BuildFrontier{root, 0, nfaces});

  while (frontier.size() > 0) {
    Vector<BuildFrontier> next;
    std::mutex nextMutex;

    task::parallel_for(
        IndexRange(frontier.size()),
        [&](IndexRange range) {
          Vector<BuildFrontier> localNext;
          for (int fi : range) {
            BuildFrontier fr = frontier[fi];
            SpatialNode *node = fr.node;

            const bool leaf = fr.count <= leafFaceLimit || node->depth >= depth_limit;
            if (leaf) {
              /* Terminal: claim the slice's faces (disjoint across leaves). */
              for (int k = 0; k < fr.count; k++) {
                int f = faces[fr.start + k].f;
                treeMesh.f.node[f] = node->id;
                node->data->unique_faces.add(f);
              }
              continue;
            }

            /* Split along the node's longest axis at the centroid mean, matching
             * split_node; fall back to the count median so a degenerate (all on
             * one side) split still makes progress. */
            const float3 bmin = node->aabb.min, bmax = node->aabb.max;
            const float3 size = bmax - bmin;
            int axis = 0;
            for (int i = 1; i < 3; i++) {
              if (size[i] > size[axis]) {
                axis = i;
              }
            }
            double sum = 0.0;
            for (int k = 0; k < fr.count; k++) {
              sum += faces[fr.start + k].cent[axis];
            }
            const float mean = float(sum / fr.count);
            float split = mean;

            BuildFace *base = &faces[fr.start];
            BuildFace *mid = std::partition(
                base, base + fr.count, [axis, split](const BuildFace &bf) {
                  return bf.cent[axis] <= split;
                });
            int n0 = int(mid - base);
            if (n0 == 0 || n0 == fr.count) {
              /* Centroids coincide on one side of the mean — split by count so
               * the recursion terminates; the plane becomes the median value. */
              std::nth_element(
                  base, base + fr.count / 2, base + fr.count,
                  [axis](const BuildFace &a, const BuildFace &b) {
                    return a.cent[axis] < b.cent[axis];
                  });
              n0 = fr.count / 2;
              split = base[n0].cent[axis];
            }

            SpatialNode *c0 = alloc_node();
            SpatialNode *c1 = alloc_node();
            for (int i = 0; i < 2; i++) {
              SpatialNode *child = i == 0 ? c0 : c1;
              child->parent = node;
              child->depth = node->depth + 1;
              child->flag = Spatial_Leaf | Spatial_RegenTris | Spatial_RegenBounds |
                            Spatial_RegenGPU | Spatial_UpdateNormals;
              child->aabb.min = bmin;
              child->aabb.max = bmax;
              child->create_data();
            }
            c0->aabb.max[axis] = split;
            c1->aabb.min[axis] = split;
            node->children[0] = c0;
            node->children[1] = c1;
            node->flag &= ~Spatial_Leaf;
            node->delete_data();

            localNext.append(BuildFrontier{c0, fr.start, n0});
            localNext.append(BuildFrontier{c1, fr.start + n0, fr.count - n0});
          }
          if (localNext.size() > 0) {
            std::lock_guard<std::mutex> guard(nextMutex);
            for (const BuildFrontier &bf : localNext) {
              next.append(bf);
            }
          }
        },
        1);

    frontier = std::move(next);
  }

  /* Deterministic renumber: parallel alloc_node assigned ids in a
   * scheduling-dependent order, so `nodes`/`leaves()` ordering would vary run
   * to run. The tree *structure* is deterministic (the partition is), so a
   * preorder DFS reassigns ids/indices deterministically and remaps the face
   * ownership written with the temporary ids. Verts are assigned below off the
   * now-final ids. */
  {
    Vector<int> oldToNew;
    oldToNew.resize(node_idgen);
    for (int i = 0; i < node_idgen; i++) {
      oldToNew[i] = 0;
    }
    Vector<SpatialNode *> ordered;
    ordered.ensure_capacity(nodes.size());
    Vector<SpatialNode *> stack;
    stack.append(root);
    int idc = 1;
    while (stack.size() > 0) {
      SpatialNode *n = stack.pop_back();
      oldToNew[n->id] = idc;
      n->id = idc;
      n->index = ordered.size();
      ordered.append(n);
      idc++;
      if (!(n->flag & Spatial_Leaf)) {
        /* Push right then left so the left subtree is visited first. */
        stack.append(n->children[1]);
        stack.append(n->children[0]);
      }
    }
    nodes = std::move(ordered);
    node_idgen = idc;
    node_idmap.clear();
    node_idmap.resize(idc);
    for (SpatialNode *n : nodes) {
      node_idmap[n->id] = n;
    }
    task::parallel_for(
        IndexRange(nfaces),
        [&](IndexRange range) {
          for (int i : range) {
            int f = faces[i].f;
            treeMesh.f.node[f] = oldToNew[treeMesh.f.node[f]];
          }
        },
        1024);
    leafCacheDirty_ = true;
    gpuNodeCacheDirty_ = true;
  }

  /* Vertex ownership: each vertex is owned by the leaf of its lowest-indexed
   * incident face, so the owning leaf always references the vert (its tri-AABB
   * covers it — a brush dab that reaches the vert filters that leaf). Computed
   * as an atomic min over faces, order-independent and race-free. */
  const int capV = int(m->v.capacity());
  Vector<int> minFace;
  minFace.resize(capV);
  task::parallel_for(
      IndexRange(capV),
      [&](IndexRange range) {
        for (int i : range) {
          minFace[i] = INT_MAX;
        }
      },
      4096);
  task::parallel_for(
      IndexRange(nfaces),
      [&](IndexRange range) {
        for (int i : range) {
          int f = faces[i].f;
          mesh::FaceProxy fp(m, f);
          for (auto list : fp.lists()) {
            for (auto cnr : list) {
              int v = cnr.v();
              std::atomic_ref<int> slot(minFace[v]);
              int cur = slot.load(std::memory_order_relaxed);
              while (f < cur &&
                     !slot.compare_exchange_weak(cur, f, std::memory_order_relaxed)) {
              }
            }
          }
        }
      },
      1024);

  /* Gather live verts, resolve each to its owning leaf id, then populate the
   * leaves' unique_verts (serial add — OrderedSet is single-writer). Loose
   * verts (no incident face) descend the tree to the leaf containing them. */
  Vector<int> liveVerts;
  liveVerts.ensure_capacity(m->v.count);
  for (int v : m->v) {
    liveVerts.append(v);
  }
  const int nverts = liveVerts.size();
  Vector<int> vLeaf;
  vLeaf.resize(nverts);
  task::parallel_for(
      IndexRange(nverts),
      [&](IndexRange range) {
        for (int i : range) {
          int v = liveVerts[i];
          int mf = minFace[v];
          if (mf != INT_MAX) {
            vLeaf[i] = treeMesh.f.node[mf];
          }
          else {
            /* Loose vert: route by position to the containing leaf. */
            float3 vco = m->v.co[v];
            SpatialNode *leaf = root;
            while (!(leaf->flag & Spatial_Leaf)) {
              SpatialNode *a = leaf->children[0];
              SpatialNode *b = leaf->children[1];
              int ax = 0;
              for (int j = 0; j < 3; j++) {
                if (a->aabb.max[j] != b->aabb.max[j]) {
                  ax = j;
                  break;
                }
              }
              leaf = vco[ax] <= a->aabb.max[ax] ? a : b;
            }
            vLeaf[i] = leaf->id;
          }
        }
      },
      2048);
  for (int i = 0; i < nverts; i++) {
    treeMesh.v.node[liveVerts[i]] = vLeaf[i];
    node_idmap[vLeaf[i]]->data->unique_verts.add(liveVerts[i]);
  }

  /* Finalize: leaf tris, bounds, the balance merge pass, then bounds/tris again
   * (mirrors buildAllSerial's tail). The two per-leaf loops parallelize. */
  {
    util::Vector<SpatialNode *> leafNodes = leaves();
    task::parallel_for(
        IndexRange(leafNodes.size()),
        [&](IndexRange range) {
          for (int i : range) {
            ensure_node_tris(leafNodes[i]);
          }
        },
        8);
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
  /* Walks face loop/list topology (FaceProxy::lists below), so the mesh must not
   * be topo-frozen — after a brush stroke it is (disk/radial pages freed), and
   * reading those links would segfault. Thaw first (no-op if already live). */
  if (m->topo_frozen) {
    m->thawTopo();
  }

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

void SpatialTree::computeLocalityMapsPartial(util::span<SpatialNode *> dirtyLeaves,
                                             util::Vector<int> &vmap,
                                             util::Vector<int> &emap,
                                             util::Vector<int> &cmap,
                                             util::Vector<int> &lmap,
                                             util::Vector<int> &fmap,
                                             util::Vector<int> *moved)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }

  struct Domain {
    mesh::ElemData &ed;
    util::Vector<int> &map;
    util::Vector<int> walk; // dirty slots in leaf-grouped walk order (deduped)
    util::BoolVector<> seen;
  };
  Domain dv{m->v, vmap, {}, {}};
  Domain de{m->e, emap, {}, {}};
  Domain dc{m->c, cmap, {}, {}};
  Domain dl{m->l, lmap, {}, {}};
  Domain df{m->f, fmap, {}, {}};
  Domain *doms[5] = {&dv, &de, &dc, &dl, &df};

  // Identity baseline; seen sized per domain capacity.
  for (Domain *d : doms) {
    int cap = int(d->ed.capacity());
    d->map.resize(cap);
    for (int i = 0; i < cap; i++) {
      d->map[i] = i;
    }
    d->seen.resize(cap);
  }

  auto claim = [](Domain &d, int i) {
    if (i != ELEM_NONE && !d.seen[i]) {
      d.seen.set(i, true);
      d.walk.append(i);
    }
  };

  /* Interior-only selection: move an element ONLY if it lives exclusively inside
   * the dirty leaves (face-anchored). Then every reference into a moved element
   * comes from a moved element or a dirty-leaf boundary element — never a clean
   * leaf — so the scoped reference + node-cache fix-up is complete from the moved
   * sets alone. Faces in the dirty leaves are dirty (each face owned by one leaf);
   * their corners/lists are interior by construction; an edge is interior iff all
   * its radial faces are dirty; a vert iff all its incident faces are dirty. */
  util::BoolVector<> dirtyFace;
  dirtyFace.resize(int(m->f.capacity()));
  for (SpatialNode *leaf : dirtyLeaves) {
    if (!leaf->data) {
      continue;
    }
    for (int face : leaf->data->unique_faces) {
      dirtyFace.set(face, true);
    }
  }
  auto faceOf = [&](int c) { return m->l.f[m->c.l[c]]; };
  /* Walk an edge's full radial corner cycle (do-while so every corner is visited
   * — CornerOfEdgeIter stops one short). */
  auto edgeInterior = [&](int e) {
    int c0 = m->e.c[e];
    if (c0 == ELEM_NONE) {
      return false;
    }
    int c = c0;
    do {
      if (!dirtyFace[faceOf(c)]) {
        return false;
      }
      c = m->c.radial_next[c];
    } while (c != c0);
    return true;
  };
  auto vertInterior = [&](int v) {
    int e0 = m->v.e[v];
    if (e0 == ELEM_NONE) {
      return false;
    }
    bool anyFace = false;
    for (int e : mesh::EdgeOfVertIter(m, v, e0)) {
      int c0 = m->e.c[e];
      if (c0 == ELEM_NONE) {
        continue;
      }
      int c = c0;
      do {
        if (m->c.v[c] == v) {
          anyFace = true;
          if (!dirtyFace[faceOf(c)]) {
            return false;
          }
        }
        c = m->c.radial_next[c];
      } while (c != c0);
    }
    return anyFace;
  };

  for (SpatialNode *leaf : dirtyLeaves) {
    if (!leaf->data) {
      continue;
    }
    for (int face : leaf->data->unique_faces) {
      if (df.seen[face]) {
        continue;
      }
      claim(df, face);
      mesh::FaceProxy fp(m, face);
      for (auto list : fp.lists()) {
        claim(dl, list.i); // list/corners of a dirty face are interior
        for (auto cnr : list) {
          claim(dc, cnr.i);
          int e = m->c.e[cnr.i];
          if (e != ELEM_NONE && !de.seen[e] && edgeInterior(e)) {
            claim(de, e);
          }
          int vrt = m->c.v[cnr.i];
          if (vrt != ELEM_NONE && !dv.seen[vrt] && vertInterior(vrt)) {
            claim(dv, vrt);
          }
        }
      }
    }
  }

  /* Closed permutation: each leaf's walk-order elements take the sorted dirty
   * slots in order, so leaf A gets the lowest slots, B the next, etc. — each
   * leaf concentrated into a contiguous sub-range of the same slot set. */
  for (int k = 0; k < 5; k++) {
    Domain &d = *doms[k];
    util::Vector<int> sorted = d.walk;
    sorted.sort([](int a, int b) { return a - b; });
    for (size_t i = 0; i < d.walk.size(); i++) {
      d.map[d.walk[i]] = sorted[i];
    }
    if (moved) {
      moved[k] = std::move(d.walk);
    }
  }
}

void SpatialTree::applyReorder(util::span<int> vmap,
                               util::span<int> emap,
                               util::span<int> cmap,
                               util::span<int> lmap,
                               util::span<int> fmap)
{
  mesh::Mesh::ReorderMoved full; // inactive → full-path reorder
  m->reorder_verts(vmap, full);
  m->reorder_edges(emap, full);
  m->reorder_corners(cmap, full);
  m->reorder_lists(lmap, full);
  m->reorder_faces(fmap, full);

  rebuild();
}

/* Phase-0 cost-breakdown profiling (plan: defrag-scoped-compaction.md), gated by
 * SCULPTCORE_REORDER_PROFILE=1. Read once. */
static bool reorderProfileEnabled()
{
  static const bool v = [] {
    const char *s = std::getenv("SCULPTCORE_REORDER_PROFILE");
    return s && s[0] && s[0] != '0';
  }();
  return v;
}

void SpatialTree::applyReorderIncremental(util::span<int> vmap,
                                          util::span<int> emap,
                                          util::span<int> cmap,
                                          util::span<int> lmap,
                                          util::span<int> fmap,
                                          util::span<int> vmoved,
                                          util::span<int> emoved,
                                          util::span<int> cmoved,
                                          util::span<int> lmoved,
                                          util::span<int> fmoved)
{
  using Clock = std::chrono::steady_clock;
  const bool prof = reorderProfileEnabled();
  auto now = [&] { return prof ? Clock::now() : Clock::time_point{}; };
  auto ms = [](Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };

  if (prof) {
    mesh::reorderAttrPermuteMs() = 0.0;
    mesh::reorderFreeRebuildMs() = 0.0;
  }

  /* Scoped node-cache remap (mode marker: any moved span non-empty). With the
   * interior-only selection, a moved element is cached ONLY in leaves owning one
   * of the moved faces, so only those leaves' caches change. Collect them now via
   * the (still pre-reorder) face-ownership attr; remap just them below. */
  const bool scoped = fmoved.size() > 0;
  util::Vector<SpatialNode *> affected;
  if (scoped) {
    util::Set<int> seenNode;
    auto addOwner = [&](int id) {
      if (id != 0 && !seenNode.contains(id)) {
        seenNode.add(id);
        if (SpatialNode *node = node_from_id(id)) {
          affected.append(node);
        }
      }
    };
    /* A leaf caches an element via face ownership (f.node) OR vert assignment
     * (v.node — a leaf can own a vert with no owned face). Both ownerships are
     * read pre-reorder; collect every leaf that owns a moved face or vert. */
    for (int f : fmoved) {
      addOwner(treeMesh.f.node[f]);
    }
    for (int v : vmoved) {
      addOwner(treeMesh.v.node[v]);
    }
  }

  mesh::Mesh::ReorderMoved rm;
  rm.active = scoped;
  rm.v = vmoved;
  rm.e = emoved;
  rm.c = cmoved;
  rm.l = lmoved;
  rm.f = fmoved;

  auto t0 = now();
  m->reorder_verts(vmap, rm);
  auto t1 = now();
  m->reorder_edges(emap, rm);
  auto t2 = now();
  m->reorder_corners(cmap, rm);
  auto t3 = now();
  m->reorder_lists(lmap, rm, cmap);
  auto t4 = now();
  m->reorder_faces(fmap, rm, lmap);
  auto t5 = now();

  /* Topology (node partition) is unchanged — relabel the cached element indices
   * each node holds. Ownership attrs (.spatial.{v,f}.node) and geometry values
   * already moved with reorder_*, so bounds/normals/GPU buffers stay valid. In
   * scoped mode only the affected leaves' caches changed (clean leaves hold
   * non-moved indices the identity map leaves untouched). */
  auto remapNode = [&](SpatialNode *node) {
    if (!node->data) {
      return;
    }
    auto &d = *node->data;

    if (d.unique_verts.size() > 0) {
      d.unique_verts.remap([&](int v) { return vmap[v]; });
    }
    if (d.unique_faces.size() > 0) {
      d.unique_faces.remap([&](int f) { return fmap[f]; });
    }
    for (NodeTri &t : d.tris) {
      t.c[0] = cmap[t.c[0]];
      t.c[1] = cmap[t.c[1]];
      t.c[2] = cmap[t.c[2]];
      t.f = fmap[t.f];
    }

    /* Pending moved-vert ids (for the next normals pass) are stale post-permute;
     * they were already consumed by the stroke-end update, so just clear them. */
    node->affected_verts.clear();
  };
  if (scoped) {
    for (SpatialNode *node : affected) {
      remapNode(node);
    }
  } else {
    for (SpatialNode *node : nodes) {
      remapNode(node);
    }
  }
  auto t6 = now();

  if (prof) {
    double reorderXTotal = ms(t0, t5);
    double attrMs = mesh::reorderAttrPermuteMs();
    double freeMs = mesh::reorderFreeRebuildMs();
    double refScanMs = reorderXTotal - attrMs - freeMs;
    std::fprintf(stderr,
                 "[reorder_prof] reorder_X v=%.2f e=%.2f c=%.2f l=%.2f f=%.2f | "
                 "node_remap=%.2f | total=%.2f ms\n",
                 ms(t0, t1),
                 ms(t1, t2),
                 ms(t2, t3),
                 ms(t3, t4),
                 ms(t4, t5),
                 ms(t5, t6),
                 ms(t0, t6));
    std::fprintf(stderr,
                 "[reorder_prof]   split: attr_permute=%.2f free_rebuild=%.2f "
                 "ref_scan=%.2f (of reorder_X %.2f)\n",
                 attrMs,
                 freeMs,
                 refScanMs,
                 reorderXTotal);
    std::fflush(stderr);
  }

  /* NB: do NOT reclaim trailing pages here. A reorder recorded for undo must be a
   * pure, capacity-preserving permutation — the meshlog chunk replays the inverse
   * map over the same [0, capacity) range. Shrinking capacity (freeTrailingStorage)
   * would make the stored map oversized for the mesh on undo → OOB. Reclaim via the
   * standalone Mesh::freeTrailingStorage only in non-undoable contexts. */
}

void SpatialTree::reorderForLocality()
{
  util::Vector<int> vmap, emap, cmap, lmap, fmap;
  computeLocalityMaps(vmap, emap, cmap, lmap, fmap);
  applyReorder(vmap, emap, cmap, lmap, fmap);
}

SpatialTree::FragStats SpatialTree::fragmentationStats()
{
  FragStats s;
  util::Set<int> pages;
  const int shift = ATTR_PAGESHIFT;

  for (SpatialNode *leaf : leaves()) {
    s.leaves++;

    auto &verts = leaf->unique_verts();
    pages.clear();
    for (int v : verts) {
      pages.add(v >> shift);
    }
    s.vertCount += verts.size();
    s.vertPagesActual += pages.size();
    s.vertPagesIdeal += (verts.size() + ATTR_PAGESIZE - 1) >> shift;

    auto &faces = leaf->unique_faces();
    pages.clear();
    for (int f : faces) {
      pages.add(f >> shift);
    }
    s.faceCount += faces.size();
    s.facePagesActual += pages.size();
    s.facePagesIdeal += (faces.size() + ATTR_PAGESIZE - 1) >> shift;
  }

  s.vertRatio =
      s.vertPagesIdeal > 0 ? double(s.vertPagesActual) / double(s.vertPagesIdeal) : 1.0;
  s.faceRatio =
      s.facePagesIdeal > 0 ? double(s.facePagesActual) / double(s.facePagesIdeal) : 1.0;
  return s;
}

void SpatialTree::materialStats(bool perLeaf, util::Vector<int> &out)
{
  AttrData<short> *mdata = nullptr;
  if (m->f.attrs.has(AttrType::SHORT, "material")) {
    mdata = m->f.attrs.find_attribute(AttrType::SHORT, "material").get_data<short>();
  }

  util::Vector<SpatialNode *> targets;
  if (perLeaf) {
    targets = leaves();
  }
  else {
    /* Same filter as the drawBatch rebuild, so triple i matches command i. */
    for (SpatialNode *node : nodes) {
      if (node->is_gpu_node && node->gpu_data && node->gpu_data->pos) {
        targets.append(node);
      }
    }
  }

  util::Vector<SpatialNode *> group;
  for (SpatialNode *node : targets) {
    group.clear();
    if (perLeaf) {
      group.append(node);
    }
    else {
      collect_subtree_leaves(node, group);
    }

    uint32_t mask = 0;
    for (SpatialNode *leaf : group) {
      ensure_node_tris(leaf);
      /* unique_faces, not tris: a face spans 2+ tris and is already deduped. */
      for (int f : leaf->unique_faces()) {
        int slot = mdata ? int((*mdata)[f]) : 0;
        slot = slot < 0 ? 0 : (slot > 31 ? 31 : slot);
        mask |= 1u << slot;
      }
    }

    int distinct = 0;
    for (int b = 0; b < 32; b++) {
      distinct += (mask >> b) & 1;
    }

    out.append(node->id);
    out.append(distinct);
    out.append(int(mask));
  }
}

void SpatialTree::selectFragmentedLeaves(double ratioThreshold,
                                         util::Vector<SpatialNode *> &out)
{
  out.clear();
  util::Set<int> pages;
  const int shift = ATTR_PAGESHIFT;

  /* Score on faces, not verts: faces are owned by exactly one leaf, so their
   * page-spread is a clean fragmentation signal. Verts are shared across leaves
   * (boundary verts), giving the vert ratio an irreducible sharing floor that
   * would over-select. */
  for (SpatialNode *leaf : leaves()) {
    if (!leaf->data) {
      continue;
    }
    auto &faces = leaf->data->unique_faces;
    int n = int(faces.size());
    if (n == 0) {
      continue;
    }
    pages.clear();
    for (int f : faces) {
      pages.add(f >> shift);
    }
    int ideal = (n + ATTR_PAGESIZE - 1) >> shift;
    if (double(pages.size()) / double(ideal) > ratioThreshold) {
      out.append(leaf);
    }
  }
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

  util::Vector<SpatialNode *> allLeaves = leaves();

  /* Empty leaves carry the reset() sentinel bounds (no geometry to box); drawing
   * them would emit huge sentinel verts into the shared "position" buffer. */
  util::Vector<SpatialNode *> ls;
  for (SpatialNode *node : allLeaves) {
    if (!node->aabb.isEmpty()) {
      ls.append(node);
    }
  }

  /* 12 edges per box × 2 endpoints = 24 verts per leaf. */
  const int vertsPerLeaf = 24;
  int totalVerts = ls.size() * vertsPerLeaf;

  Buffer *posBuf = mgr.createBuffer(
      litestl::util::string("position"), GPUType::FLOAT32, 3, totalVerts);
  Buffer *colorBuf =
      mgr.createBuffer(litestl::util::string("color"), GPUType::FLOAT32, 4, totalVerts);

  float3 *pos = posBuf->get_data<float3>();
  float4 *color = colorBuf->get_data<float4>();

  int idx = 0;

  auto addLine = [pos, color, &idx](const float3 &a, const float3 &b, const float4 &clr) {
    pos[idx] = a;
    color[idx] = clr;
    idx++;

    pos[idx] = b;
    color[idx] = clr;
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

  auto *shader = &spatialShaders.basicLineShader;

  DrawCommand *cmd = mgr.createCommand(
      batch, GPUCmdType::DRAW_LINES, shader, 0, totalVerts, totalVerts / 2);
  cmd->attrs.append(posBuf);
  cmd->attrs.append(colorBuf);

  return batch;
}

sculptcore::gpu::DrawBatch *SpatialTree::buildBoundsBatch(sculptcore::gpu::GPUManager &mgr)
{
  using namespace sculptcore::gpu;

  /* Union the non-empty leaf bounds (empty leaves carry the reset() sentinel). */
  litestl::math::AABB<float3> aabb;
  aabb.reset();
  bool haveAny = false;
  for (SpatialNode *node : leaves()) {
    if (!node->aabb.isEmpty()) {
      aabb.add(node->aabb.min);
      aabb.add(node->aabb.max);
      haveAny = true;
    }
  }
  if (!haveAny) {
    return nullptr;
  }

  const int totalVerts = 24; /* 12 edges x 2 endpoints. */

  Buffer *posBuf = mgr.createBuffer(
      litestl::util::string("position"), GPUType::FLOAT32, 3, totalVerts);
  Buffer *colorBuf =
      mgr.createBuffer(litestl::util::string("color"), GPUType::FLOAT32, 4, totalVerts);

  float3 *pos = posBuf->get_data<float3>();
  float4 *color = colorBuf->get_data<float4>();

  int idx = 0;
  auto addLine = [pos, color, &idx](const float3 &a, const float3 &b, const float4 &clr) {
    pos[idx] = a;
    color[idx] = clr;
    idx++;
    pos[idx] = b;
    color[idx] = clr;
    idx++;
  };

  const float4 clr(1.0f, 1.0f, 1.0f, 1.0f);
  float3 mn = aabb.min;
  float3 mx = aabb.max;
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

  addLine(c[0], c[1], clr);
  addLine(c[1], c[2], clr);
  addLine(c[2], c[3], clr);
  addLine(c[3], c[0], clr);

  addLine(c[4], c[5], clr);
  addLine(c[5], c[6], clr);
  addLine(c[6], c[7], clr);
  addLine(c[7], c[4], clr);

  addLine(c[0], c[4], clr);
  addLine(c[1], c[5], clr);
  addLine(c[2], c[6], clr);
  addLine(c[3], c[7], clr);

  posBuf->dirty();

  DrawBatch *batch = mgr.createBatch();
  batch->buffers.append(posBuf);
  batch->buffers.append(colorBuf);

  auto *shader = &spatialShaders.basicLineShader;

  DrawCommand *cmd = mgr.createCommand(
      batch, GPUCmdType::DRAW_LINES, shader, 0, totalVerts, totalVerts / 2);
  cmd->attrs.append(posBuf);
  cmd->attrs.append(colorBuf);

  return batch;
}

sculptcore::gpu::DrawBatch *SpatialTree::buildSeamBatch(sculptcore::gpu::GPUManager &mgr,
                                                        bool includePolyGroup)
{
  using namespace sculptcore::gpu;

  using namespace mesh::boundary;
  BoolAttrView *seam = findBoolEdgeView(m, EDGE_SEAM);
  BoolAttrView *sharp = findBoolEdgeView(m, EDGE_SHARP);
  BoolAttrView *proj = findBoolEdgeView(m, EDGE_PROJECTED);
  BoolAttrView *pg = findBoolEdgeView(m, EDGE_POLYGROUP);
  BoolAttrView *uv = findBoolEdgeView(m, EDGE_UVCHART);
  BoolAttrView *layer = findBoolEdgeView(m, EDGE_LAYER_REGION);

  // First matching feature wins the color (seam is the user-marked one, so it
  // takes precedence). Returns false for a non-feature edge.
  auto edgeColor = [&](int e, float4 &out) -> bool {
    if (seam && seam->get(e)) {
      out = float4(1.0f, 0.4f, 0.0f, 1.0f);
      return true;
    } // orange
    if (sharp && sharp->get(e)) {
      out = float4(0.0f, 0.8f, 1.0f, 1.0f);
      return true;
    } // cyan
    if (proj && proj->get(e)) {
      out = float4(0.2f, 1.0f, 0.2f, 1.0f);
      return true;
    } // green
    if (includePolyGroup && pg && pg->get(e)) {
      out = float4(1.0f, 0.0f, 1.0f, 1.0f);
      return true;
    } // magenta (opt-in)
    if (uv && uv->get(e)) {
      out = float4(1.0f, 1.0f, 0.0f, 1.0f);
      return true;
    } // yellow
    if (layer && layer->get(e)) {
      out = float4(1.0f, 0.2f, 0.6f, 1.0f);
      return true;
    } // pink: VDM/geometry carrier-region boundary (the V5 carrier overlay)
    return false;
  };

  // Frozen-safe early-out: flag reads need no live topology, and with
  // boundaryDirty clear they are current — a mesh with no feature edges skips
  // the O(mesh) thaw below entirely (every sculpt stroke end lands here).
  int ncount = 0;
  float4 tmpClr;
  if (!m->boundaryDirty) {
    if (!seam && !sharp && !proj && !pg && !uv && !layer) {
      return nullptr;
    }
    for (int e : m->e) {
      if (edgeColor(e, tmpClr)) {
        ncount++;
      }
    }
    if (ncount == 0) {
      return nullptr;
    }
  }

  // Need live edge endpoints (e.vs); the sculpt path may have left topology frozen.
  if (m->topo_frozen) {
    m->thawTopo();
  }

  // Derived flags (poly-group, UV-chart) are refreshed so they reflect the
  // current mesh. recomputeDirty can create flag layers — re-resolve the views
  // (edgeColor captures them by reference) and count on the fresh state.
  if (m->boundaryDirty) {
    mesh::boundary::recomputeDirty(m);
    seam = findBoolEdgeView(m, EDGE_SEAM);
    sharp = findBoolEdgeView(m, EDGE_SHARP);
    proj = findBoolEdgeView(m, EDGE_PROJECTED);
    pg = findBoolEdgeView(m, EDGE_POLYGROUP);
    uv = findBoolEdgeView(m, EDGE_UVCHART);
    layer = findBoolEdgeView(m, EDGE_LAYER_REGION);
    ncount = 0;
    for (int e : m->e) {
      if (edgeColor(e, tmpClr)) {
        ncount++;
      }
    }
    if (ncount == 0) {
      return nullptr;
    }
  }

  // Geometry sits exactly on the surface; depth separation comes from the
  // overlay shaders' polygonOffset-style NDC bias (litemesh_wgsl.ts).

  const int totalVerts = ncount * 2;
  Buffer *posBuf = mgr.createBuffer(
      litestl::util::string("position"), GPUType::FLOAT32, 3, totalVerts);
  Buffer *colorBuf =
      mgr.createBuffer(litestl::util::string("color"), GPUType::FLOAT32, 4, totalVerts);

  float3 *pos = posBuf->get_data<float3>();
  float4 *color = colorBuf->get_data<float4>();

  int idx = 0;
  for (int e : m->e) {
    float4 clr;
    if (!edgeColor(e, clr)) {
      continue;
    }
    int v1 = m->e.vs[e][0];
    int v2 = m->e.vs[e][1];

    // Feature edges lie exactly on the surface, so they z-fight with / are hidden
    // behind the mesh. Float each endpoint out along its vertex normal by the
    // uniform `off` so the line hovers just above the surface (visible from
    // outside, still occluded by geometry in front of it).
    pos[idx] = m->v.co[v1];
    color[idx] = clr;
    idx++;

    pos[idx] = m->v.co[v2];
    color[idx] = clr;
    idx++;
  }

  posBuf->dirty();

  DrawBatch *batch = mgr.createBatch();
  batch->buffers.append(posBuf);
  batch->buffers.append(colorBuf);

  auto *shader = &spatialShaders.basicLineShader;

  DrawCommand *cmd = mgr.createCommand(
      batch, GPUCmdType::DRAW_LINES, shader, 0, totalVerts, totalVerts / 2);
  cmd->attrs.append(posBuf);
  cmd->attrs.append(colorBuf);

  return batch;
}

sculptcore::gpu::DrawBatch *
SpatialTree::buildSelectionBatch(sculptcore::gpu::GPUManager &mgr,
                                 int activeVert,
                                 int activeEdge,
                                 int activeFace,
                                 int hoverVert,
                                 int hoverEdge,
                                 int hoverFace)
{
  using namespace sculptcore::gpu;

  // Box-modeling selection overlay: selected faces as translucent fan-tris,
  // selected edges as lines, selected verts as small 3-axis crosses, with the
  // active element of each domain highlighted. One batch, two commands (tris +
  // lines) sharing the position/color buffers — the executor keys the pipeline
  // topology off each command's GPUCmdType + start/end range. Mirrors the
  // buildSeamBatch normal-offset trick so the overlay hovers just above the
  // surface. Billboard vertex points replace the crosses in M5.
  if (m->topo_frozen) {
    m->thawTopo();
  }

  // Bool views (guaranteed non-null after the select layers are ensured); get()
  // is safe before any set (unset bits read false), as the seam path relies on.
  mesh::BoolAttrView *vsel = m->v.select.get_data();
  mesh::BoolAttrView *esel = m->e.select.get_data();
  mesh::BoolAttrView *fsel = m->f.select.get_data();

  // A uniform push-out + cross size from the average edge length (assumes
  // m->v.no is unit-length, true after update_node_normals before drawQ).
  float lenSum = 0.0f;
  int ecount = 0;
  for (int e : m->e) {
    lenSum += (m->v.co[m->e.vs[e][1]] - m->v.co[m->e.vs[e][0]]).length();
    ecount++;
  }
  if (ecount == 0) {
    return nullptr;
  }
  const float avg = lenSum / float(ecount);
  // Cross size only; depth separation comes from the shader-side bias.
  const float cross = avg * 0.15f;

  // Element is drawn if selected OR the hover element of its domain.
  auto faceIn = [&](int fi) { return fsel->get(fi) || fi == hoverFace; };
  auto edgeIn = [&](int e) { return esel->get(e) || e == hoverEdge; };
  auto vertIn = [&](int vi) { return vsel->get(vi) || vi == hoverVert; };

  // Pass 1: count fill-tri verts (single-outer-loop faces) + line verts.
  int fillTriVerts = 0;
  for (int fi : m->f) {
    if (!faceIn(fi) || m->f.list_count[fi] != 1) {
      continue;
    }
    int sz = m->l.size[m->f.l[fi]];
    if (sz >= 3) {
      fillTriVerts += (sz - 2) * 3;
    }
  }
  int lineVerts = 0;
  for (int e : m->e) {
    if (edgeIn(e)) {
      lineVerts += 2;
    }
  }
  for (int vi : m->v) {
    if (vertIn(vi)) {
      lineVerts += 6;
    }
  }
  if (fillTriVerts == 0 && lineVerts == 0) {
    return nullptr;
  }

  const int totalVerts = fillTriVerts + lineVerts;
  Buffer *posBuf = mgr.createBuffer(
      litestl::util::string("position"), GPUType::FLOAT32, 3, totalVerts);
  Buffer *colorBuf =
      mgr.createBuffer(litestl::util::string("color"), GPUType::FLOAT32, 4, totalVerts);
  float3 *pos = posBuf->get_data<float3>();
  float4 *color = colorBuf->get_data<float4>();

  const float4 selClr(1.0f, 0.6f, 0.1f, 1.0f); // orange
  const float4 actClr(1.0f, 1.0f, 1.0f, 1.0f); // white
  const float4 hovClr(0.4f, 0.8f, 1.0f, 1.0f); // cyan (hover highlight)

  int idx = 0;

  // Pass 2a: translucent face fills [0, fillTriVerts) — selected + hover.
  for (int fi : m->f) {
    if (!faceIn(fi) || m->f.list_count[fi] != 1) {
      continue;
    }
    int li = m->f.l[fi];
    int sz = m->l.size[li];
    if (sz < 3) {
      continue;
    }
    float4 clr = fi == activeFace ? float4(1.0f, 1.0f, 1.0f, 0.45f)
                 : !fsel->get(fi) ? float4(0.4f, 0.8f, 1.0f, 0.3f)
                                  : float4(1.0f, 0.5f, 0.1f, 0.25f);

    litestl::util::Vector<int, 32> vs;
    int c0 = m->l.c[li], cc = c0;
    do {
      vs.append(m->c.v[cc]);
      cc = m->c.next[cc];
    } while (cc != c0);

    for (int i = 1; i + 1 < int(vs.size()); i++) {
      int tri[3] = {vs[0], vs[i], vs[i + 1]};
      for (int k = 0; k < 3; k++) {
        int vv = tri[k];
        pos[idx] = m->v.co[vv];
        color[idx] = clr;
        idx++;
      }
    }
  }

  // Pass 2b: lines [fillTriVerts, totalVerts) — edges + vert crosses.
  auto addLine = [&](const float3 &a, const float3 &b, const float4 &clr) {
    pos[idx] = a;
    color[idx] = clr;
    idx++;
    pos[idx] = b;
    color[idx] = clr;
    idx++;
  };

  for (int e : m->e) {
    if (!edgeIn(e)) {
      continue;
    }
    int v1 = m->e.vs[e][0];
    int v2 = m->e.vs[e][1];
    float4 clr = e == activeEdge ? actClr : !esel->get(e) ? hovClr : selClr;
    addLine(m->v.co[v1], m->v.co[v2], clr);
  }

  for (int vi : m->v) {
    if (!vertIn(vi)) {
      continue;
    }
    float4 clr = vi == activeVert ? actClr : !vsel->get(vi) ? hovClr : selClr;
    float3 p = m->v.co[vi];
    addLine(p - float3(cross, 0.0f, 0.0f), p + float3(cross, 0.0f, 0.0f), clr);
    addLine(p - float3(0.0f, cross, 0.0f), p + float3(0.0f, cross, 0.0f), clr);
    addLine(p - float3(0.0f, 0.0f, cross), p + float3(0.0f, 0.0f, cross), clr);
  }

  posBuf->dirty();

  DrawBatch *batch = mgr.createBatch();
  batch->buffers.append(posBuf);
  batch->buffers.append(colorBuf);

  auto *shader = &spatialShaders.basicLineShader;

  if (fillTriVerts > 0) {
    DrawCommand *cmd = mgr.createCommand(
        batch, GPUCmdType::DRAW_TRIS, shader, 0, fillTriVerts, fillTriVerts / 3);
    cmd->attrs.append(posBuf);
    cmd->attrs.append(colorBuf);
  }
  if (lineVerts > 0) {
    DrawCommand *cmd = mgr.createCommand(
        batch, GPUCmdType::DRAW_LINES, shader, fillTriVerts, totalVerts, lineVerts / 2);
    cmd->attrs.append(posBuf);
    cmd->attrs.append(colorBuf);
  }

  return batch;
}

sculptcore::gpu::DrawBatch *
SpatialTree::buildWireframeBatch(sculptcore::gpu::GPUManager &mgr)
{
  using namespace sculptcore::gpu;

  if (m->topo_frozen) {
    m->thawTopo();
  }

  int ecount = 0;
  for (int e : m->e) {
    (void)e;
    ecount++;
  }
  if (ecount == 0) {
    return nullptr;
  }

  const int totalVerts = ecount * 2;
  Buffer *posBuf = mgr.createBuffer(
      litestl::util::string("position"), GPUType::FLOAT32, 3, totalVerts);
  Buffer *colorBuf =
      mgr.createBuffer(litestl::util::string("color"), GPUType::FLOAT32, 4, totalVerts);
  float3 *pos = posBuf->get_data<float3>();
  float4 *color = colorBuf->get_data<float4>();

  const float4 wireClr(0.0f, 0.0f, 0.0f, 0.35f);
  int idx = 0;
  for (int e : m->e) {
    int v1 = m->e.vs[e][0], v2 = m->e.vs[e][1];
    pos[idx] = m->v.co[v1];
    color[idx] = wireClr;
    idx++;
    pos[idx] = m->v.co[v2];
    color[idx] = wireClr;
    idx++;
  }
  posBuf->dirty();

  DrawBatch *batch = mgr.createBatch();
  batch->buffers.append(posBuf);
  batch->buffers.append(colorBuf);
  DrawCommand *cmd = mgr.createCommand(batch,
                                       GPUCmdType::DRAW_LINES,
                                       &spatialShaders.basicLineShader,
                                       0,
                                       totalVerts,
                                       ecount);
  cmd->attrs.append(posBuf);
  cmd->attrs.append(colorBuf);
  return batch;
}

sculptcore::gpu::DrawBatch *
SpatialTree::buildPointsBatch(sculptcore::gpu::GPUManager &mgr)
{
  using namespace sculptcore::gpu;
  using litestl::math::float2;

  if (m->topo_frozen) {
    m->thawTopo();
  }

  int vcount = 0;
  for (int v : m->v) {
    (void)v;
    vcount++;
  }
  if (vcount == 0) {
    return nullptr;
  }

  // Two triangles per point; the per-vertex corner expands the billboard quad.
  const float2 corners[6] = {float2(-1.0f, -1.0f),
                             float2(1.0f, -1.0f),
                             float2(1.0f, 1.0f),
                             float2(-1.0f, -1.0f),
                             float2(1.0f, 1.0f),
                             float2(-1.0f, 1.0f)};

  const int totalVerts = vcount * 6;
  Buffer *posBuf = mgr.createBuffer(
      litestl::util::string("position"), GPUType::FLOAT32, 3, totalVerts);
  Buffer *cornerBuf =
      mgr.createBuffer(litestl::util::string("corner"), GPUType::FLOAT32, 2, totalVerts);
  Buffer *colorBuf =
      mgr.createBuffer(litestl::util::string("color"), GPUType::FLOAT32, 4, totalVerts);
  float3 *pos = posBuf->get_data<float3>();
  float2 *corner = cornerBuf->get_data<float2>();
  float4 *color = colorBuf->get_data<float4>();

  const float4 ptClr(0.05f, 0.05f, 0.05f, 1.0f); // near-black dots
  int idx = 0;
  for (int v : m->v) {
    float3 p = m->v.co[v];
    for (int k = 0; k < 6; k++) {
      pos[idx] = p;
      corner[idx] = corners[k];
      color[idx] = ptClr;
      idx++;
    }
  }
  posBuf->dirty();
  cornerBuf->dirty();

  DrawBatch *batch = mgr.createBatch();
  batch->buffers.append(posBuf);
  batch->buffers.append(cornerBuf);
  batch->buffers.append(colorBuf);
  DrawCommand *cmd = mgr.createCommand(batch,
                                       GPUCmdType::DRAW_TRIS,
                                       &spatialShaders.basicPointShader,
                                       0,
                                       totalVerts,
                                       totalVerts / 3);
  cmd->attrs.append(posBuf);
  cmd->attrs.append(cornerBuf);
  cmd->attrs.append(colorBuf);
  return batch;
}

void SpatialTree::update_node_normals(SpatialNode *node)
{
  const bool fullRebuild = bool(node->flag & Spatial_NormalsFullRebuild);
  node->flag &= ~(Spatial_UpdateNormals | Spatial_NormalsFullRebuild);

  auto &node_vattr = node->treeMesh->v.node;
  auto &node_fattr = node->treeMesh->f.node;

  /* Incremental path: only recompute normals for verts the brush actually
   * moved, plus their 1-ring (since a moved vert changes the normal of
   * every face touching it, which in turn changes the normals of the other
   * verts of those faces). For small brushes on a large leaf this avoids
   * zeroing + renormalizing hundreds of unaffected verts/faces and skips
   * the cross-product accumulation for tris that didn't change. */
  // The incremental path still scans every tri twice; all it saves over a
  // rebuild is the zero + normalize sweeps. Once the moved set covers a decent
  // share of the leaf that trade stops paying, so hand those to the full path.
  if (!fullRebuild && node->affected_verts.size() > 0 &&
      node->affected_verts.size() * 4 < node->data->unique_verts.size())
  {
    // Sorted id vectors rather than hash sets: this runs for every dirty leaf
    // of every frame, and the membership tests dominate it — a binary search
    // over a few thousand ids costs a fraction of hashing them.
    auto sort_unique = [](Vector<int> &ids) {
      std::sort(ids.data(), ids.data() + ids.size());
      ids.resize(int(std::unique(ids.data(), ids.data() + ids.size()) - ids.data()));
    };
    auto has = [](Vector<int> &ids, int id) {
      return std::binary_search(ids.data(), ids.data() + ids.size(), id);
    };

    Vector<int> moved_verts;
    for (int v : node->affected_verts) {
      moved_verts.append(v);
    }
    sort_unique(moved_verts);

    /* Expand to the 1-ring: any tri that touches a moved vert contributes
     * to the affected face/vert sets. */
    Vector<int> affected_face_set;
    Vector<int> affected_vert_set;

    for (int ti : IndexRange(node->data->tris.size())) {
      const auto &tri = node->data->tris[ti];
      int v1 = m->c.v[tri.c[0]];
      int v2 = m->c.v[tri.c[1]];
      int v3 = m->c.v[tri.c[2]];

      if (!has(moved_verts, v1) && !has(moved_verts, v2) && !has(moved_verts, v3)) {
        continue;
      }

      affected_face_set.append(tri.f);
      affected_vert_set.append(v1);
      affected_vert_set.append(v2);
      affected_vert_set.append(v3);
    }

    /* Skirt tris expand vert coverage the same way (their faces belong to a
     * neighbor leaf, so no face-set entry). */
    for (const auto &tri : node->data->skirt_tris) {
      int v1 = m->c.v[tri.c[0]];
      int v2 = m->c.v[tri.c[1]];
      int v3 = m->c.v[tri.c[2]];

      if (!has(moved_verts, v1) && !has(moved_verts, v2) && !has(moved_verts, v3)) {
        continue;
      }

      affected_vert_set.append(v1);
      affected_vert_set.append(v2);
      affected_vert_set.append(v3);
    }

    sort_unique(affected_face_set);
    sort_unique(affected_vert_set);

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

    /* A 1-ring vert's normal sums ALL its incident tris — including unchanged
     * ones zeroed away with it — so scan every tri, not just the movers'. Faces
     * stay restricted to the affected set (un-zeroed faces must not re-gain). */
    for (int ti : IndexRange(node->data->tris.size())) {
      const auto &tri = node->data->tris[ti];
      int v1 = m->c.v[tri.c[0]];
      int v2 = m->c.v[tri.c[1]];
      int v3 = m->c.v[tri.c[2]];

      const bool a1 = has(affected_vert_set, v1);
      const bool a2 = has(affected_vert_set, v2);
      const bool a3 = has(affected_vert_set, v3);
      const bool af = has(affected_face_set, tri.f);
      if (!a1 && !a2 && !a3 && !af) {
        continue;
      }

      float3 n = triNormal(m->v.co[v1], m->v.co[v2], m->v.co[v3]);

      if (af && node_fattr[tri.f] == node->id) {
        m->f.no[tri.f] += n;
      }
      if (a1 && node_vattr[v1] == node->id) {
        m->v.no[v1] += n;
      }
      if (a2 && node_vattr[v2] == node->id) {
        m->v.no[v2] += n;
      }
      if (a3 && node_vattr[v3] == node->id) {
        m->v.no[v3] += n;
      }
    }

    /* Skirt contributions complete the zeroed boundary verts' fans (owned
     * verts only — the face normal belongs to the owning leaf). */
    for (const auto &tri : node->data->skirt_tris) {
      int v1 = m->c.v[tri.c[0]];
      int v2 = m->c.v[tri.c[1]];
      int v3 = m->c.v[tri.c[2]];

      const bool a1 = has(affected_vert_set, v1);
      const bool a2 = has(affected_vert_set, v2);
      const bool a3 = has(affected_vert_set, v3);
      if (!a1 && !a2 && !a3) {
        continue;
      }

      float3 n = triNormal(m->v.co[v1], m->v.co[v2], m->v.co[v3]);

      if (a1 && node_vattr[v1] == node->id) {
        m->v.no[v1] += n;
      }
      if (a2 && node_vattr[v2] == node->id) {
        m->v.no[v2] += n;
      }
      if (a3 && node_vattr[v3] == node->id) {
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
   * dirty source that didn't populate affected_verts. Hints appended since the
   * tris regen are subsumed by the full pass. */
  node->affected_verts.clear();
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

  /* Skirt contributions complete the boundary verts' fans (owned verts only —
   * the face normal belongs to the owning leaf). */
  for (const auto &tri : node->data->skirt_tris) {
    int v1 = m->c.v[tri.c[0]];
    int v2 = m->c.v[tri.c[1]];
    int v3 = m->c.v[tri.c[2]];

    float3 n = triNormal(m->v.co[v1], m->v.co[v2], m->v.co[v3]);

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
  return updateImpl(gpu, Update_All);
}

bool SpatialTree::updateQueries()
{
  return updateImpl(nullptr, Update_Queries);
}

bool SpatialTree::updateNormals()
{
  return updateImpl(nullptr, Update_Normals);
}

bool SpatialTree::updateImpl(gpu::GPUManager *gpu, UpdatePhases phases)
{
  bool result = false;
  bool bounds = false;
  bool drawBatchUpdated = false;
  bool gpuWorkDone = false;

  if (phases & Update_Queries) {
    /* Phase 0: deferred rebalance. Incremental add_face_at placement skips the
     * inline split, so leaves that grew past leaf_limit during the dab are split
     * here, once each. Runs before the tris phase so the fresh child leaves (which
     * carry Spatial_RegenTris) are picked up by the collection loop below. */
    applyDeferredNodeSplit();

    /* Phase 0b: deferred merge, on a slow cadence of *frame* updates (the
     * per-dab updateQueries() calls don't count — they used to, which made
     * merges fire mid-stroke: the resulting RegenTris marked the topology
     * changed, forcing a GPU repartition + full owner-buffer regens (a
     * whole-mesh re-upload) every stroke). Also hold merges while topology is
     * frozen: only dyntopo collapses produce under-full leaves, and those
     * strokes run thawed — a frozen-mode merge would just force an O(mesh)
     * thaw in the tris phase below. */
    if ((phases & Update_Gpu) && ++updatesSinceMerge_ >= mergeCadence_ &&
        !m->topo_frozen)
    {
      applyDeferredMerge();
      updatesSinceMerge_ = 0;
    }

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

    {
#ifdef NO_PARALLEL_FOR
      for (SpatialNode *node : updateTriNodes) {
        ensure_node_tris(node);
      }
#else
      litestl::task::parallel_for(
          util::IndexRange(updateTriNodes.size()),
          [&](IndexRange range) {
            for (int i : range) {
              SpatialNode *node = updateTriNodes[i];
              ensure_node_tris(node);
            }
          },
          4);
#endif
    }

    /* Phase: skirt regen. A re-tri'd leaf's faces may have joined the fan of a
     * vert owned elsewhere, so those owners' cached skirts are stale; flag them
     * (serial — writes other nodes' flags, unsafe inside the parallel regen).
     * The re-tri'd leaf's own skirt was self-armed by regen_node_tris, and
     * onCornerKill flagged the owners of verts that LOST a face. Then rebuild
     * every flagged skirt in parallel (each build touches only its own node). */
    {
      for (SpatialNode *node : updateTriNodes) {
        for (const NodeTri &tri : node->data->tris) {
          for (int k = 0; k < 3; k++) {
            int vn = treeMesh.v.node[m->c.v[tri.c[k]]];
            if (vn == 0 || vn == node->id) {
              continue;
            }
            SpatialNode *nb = node_from_id(vn);
            if (nb && (nb->flag & Spatial_Leaf) && nb->data) {
              nb->flag |= Spatial_RegenSkirt;
            }
          }
        }
      }

      Vector<SpatialNode *, 256> updateSkirtNodes;
      for (SpatialNode *node : nodes) {
        if ((node->flag & Spatial_Leaf) && (node->flag & Spatial_RegenSkirt) &&
            node->data)
        {
          updateSkirtNodes.append(node);
        }
      }
      /* Callback-flagged skirts can outlive the thaw that accompanied their
       * topology change (the rebuild may land on a later, frozen call). */
      if (updateSkirtNodes.size() > 0 && m->topo_frozen) {
        m->thawTopo();
      }
#ifdef NO_PARALLEL_FOR
      for (SpatialNode *node : updateSkirtNodes) {
        build_node_skirt(node);
      }
#else
      litestl::task::parallel_for(
          util::IndexRange(updateSkirtNodes.size()),
          [&](IndexRange range) {
            for (int i : range) {
              build_node_skirt(updateSkirtNodes[i]);
            }
          },
          4);
#endif
    }

    {
      if (regenDirtyBounds()) {
        bounds = true;
        result = true;
        pendingCmdAabbs_ = true;
      }
    }

    /* Phase: leaf normals (independent of partition). Own phase bit: per-dab
     * updateQueries() skips it (queries never read vertex normals), leaving
     * Spatial_UpdateNormals set so the per-frame update refreshes each dirty
     * leaf once — not once per overlapping dab. */
    if (phases & Update_Normals) {
      Vector<SpatialNode *, 256> updateNormalsNodes;
      for (SpatialNode *node : nodes) {
        if (!(node->flag & Spatial_Leaf)) {
          continue;
        }
        if (node->flag & Spatial_UpdateNormals) {
          updateNormalsNodes.append(node);
          drawBatchUpdated = true;
        }
      }

      // Cross-boundary halo: a moved vert changes the normal of every vert
      // sharing a face with it, including ones owned by leaves the brush never
      // flagged, so hint those owners. One round suffices — hints did not move.
      const int primaryNormalsCount = int(updateNormalsNodes.size());
      // Only `border_tris` and the skirt are candidates: an all-owned tri can
      // name no other leaf. The scan is read-only and parallel; every write is
      // deferred to the serial merge, since hints land on leaves others scan.
      struct HaloHint {
        int node_id;
        int vert;
      };
      Vector<Vector<HaloHint>> haloHints;
      haloHints.resize(primaryNormalsCount);
      litestl::task::parallel_for(
          util::IndexRange(primaryNormalsCount),
          [&](IndexRange range) {
            Vector<int> moved;
            for (int ni : range) {
              SpatialNode *node = updateNormalsNodes[ni];
              ensure_border_cache(node);
              const bool full = bool(node->flag & Spatial_NormalsFullRebuild) ||
                                node->affected_verts.size() == 0 ||
                                node->affected_verts.size() * 4 >=
                                    node->data->unique_verts.size();
              moved.clear();
              if (!full) {
                for (int v : node->affected_verts) {
                  moved.append(v);
                }
                std::sort(moved.data(), moved.data() + moved.size());
              }
              int *mb = moved.data(), *me = mb + moved.size();
              Vector<HaloHint> &out = haloHints[ni];
              auto scanTri = [&](const NodeTri &tri) {
                int vs[3] = {m->c.v[tri.c[0]], m->c.v[tri.c[1]], m->c.v[tri.c[2]]};
                if (!full && !std::binary_search(mb, me, vs[0]) &&
                    !std::binary_search(mb, me, vs[1]) && !std::binary_search(mb, me, vs[2]))
                {
                  return;
                }
                for (int k = 0; k < 3; k++) {
                  int vn = treeMesh.v.node[vs[k]];
                  if (vn == 0 || vn == node->id) {
                    continue;
                  }
                  out.append({vn, vs[k]});
                }
              };
              for (int ti : node->data->border_tris) {
                scanTri(node->data->tris[ti]);
              }
              for (const NodeTri &tri : node->data->skirt_tris) {
                scanTri(tri);
              }
            }
          },
          4);
      for (int ni = 0; ni < primaryNormalsCount; ni++) {
        for (const HaloHint &hint : haloHints[ni]) {
          SpatialNode *nb = node_from_id(hint.node_id);
          if (!nb || !(nb->flag & Spatial_Leaf) || !nb->data) {
            continue;
          }
          if (nb->flag & Spatial_UpdateNormals) {
            // Already dirty. Empty affected_verts on a flagged leaf REQUESTS A
            // FULL REBUILD — appending a hint downgrades it to an incremental
            // pass over the hinted ring, leaving the leaf's interior stale.
            if (nb->affected_verts.size() > 0) {
              nb->affected_verts.append(hint.vert);
            }
            continue;
          }
          nb->affected_verts.append(hint.vert);
          nb->flag |= Spatial_UpdateNormals | Spatial_UpdateGPUGeom;
          updateNormalsNodes.append(nb);
          drawBatchUpdated = true;
        }
      }

      {
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
      }
    }

    /* Leaf tris regenerated (incl. fresh split/merge leaves, which carry
     * RegenTris): the GPU partition is stale. Sticky — the GPU half may run
     * in a later call (the draw frame). */
    pendingGpuTopology_ |= topology_changed;
  }

  if (!(phases & Update_Gpu)) {
    return result;
  }

  /* Phase: GPU partition assignment. Cheap walk (O(nodes)). If topology
   * didn't change we can skip recomputing counts, but the assignment
   * walk itself is still needed first time around. */
  const bool topoPending = pendingGpuTopology_;
  pendingGpuTopology_ = false;
  if (topoPending || !done_gpu_assignment) {
    recompute_subtree_tri_counts();
    assign_gpu_nodes();
  }

  /* Phase: propagate per-leaf GPU dirty bits to their owning GPU node and
   * rebuild/update those nodes' buffers. Full regens are split in two: a
   * serial planning stage per owner (buffer dispose/alloc through the
   * non-thread-safe GPUManager, slice-table build, flag clears), then the
   * per-slice fills run in one unified parallel pass together with the
   * in-place slice updates — both write only disjoint buffer sub-ranges. */
  struct SliceWork {
    SpatialNode *owner;
    SpatialNode *leaf;
    /* Leaf is dirty via Spatial_UpdateGPUGeom only (pure deform): the slice
     * fill + upload cover pos/nor and leave the attr streams untouched. */
    bool geomOnly;
  };
  Vector<SliceWork, 256> sliceWork;
  Vector<SpatialNode *, 64> regenOwners;
  util::Set<int> regenOwnerIds;

  for (SpatialNode *node : nodes) {
    if (!(node->flag & Spatial_Leaf)) {
      continue;
    }
    NodeFlags want =
        node->flag & (Spatial_RegenGPU | Spatial_UpdateGPU | Spatial_UpdateGPUGeom);
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
        owner->gpu_data->pos->gpu_owned)
    {
      node->flag &= ~(Spatial_RegenGPU | Spatial_UpdateGPU | Spatial_UpdateGPUGeom);
      continue;
    }

    /* Full regen of the owner if the owner is brand-new (no buffers),
     * the leaf wants a full regen, or the slice layout is missing. */
    bool need_full = !owner->gpu_data || !owner->gpu_data->pos ||
                     owner->gpu_data->slices.size() == 0 || (want & Spatial_RegenGPU);

    if (need_full) {
      if (!regenOwnerIds.contains(owner->id)) {
        regenOwnerIds.add(owner->id);
        regenOwners.append(owner);
      }
      drawBatchUpdated = true;
    } else {
      sliceWork.append({owner, node, !(want & Spatial_UpdateGPU)});
    }
  }

  /* Any GPU node still missing buffers (it transitioned from non-GPU to GPU
   * this tick and contains no individually-dirty leaves) needs a full rebuild
   * too — fold it into the same plan/fill path. */
  for (SpatialNode *node : nodes) {
    if (!node->is_gpu_node) {
      continue;
    }
    if ((!node->gpu_data || !node->gpu_data->pos) && !regenOwnerIds.contains(node->id)) {
      regenOwnerIds.add(node->id);
      regenOwners.append(node);
      drawBatchUpdated = true;
    }
  }

  /* GPU work of any kind (full regens or in-place slice updates) means the
   * rendered geometry changes this call — widen the return so a draw-frame
   * update() reports work even when the per-dab updateQueries() calls already
   * consumed the bounds dirt. */
  gpuWorkDone = regenOwners.size() > 0 || sliceWork.size() > 0;

  /* Serial planning stage: one plan per owner, emitting per-slice fill jobs.
   * Owner dedup happened above — duplicate-owner suppression can no longer
   * rely on the first regen clearing leaf flags mid-loop once fills overlap.
   * srcRefs are resolved once per owner into a flat array (requestedAttrs.size()
   * entries each, dynamic path only). */
  struct RegenJob {
    int ownerIdx;
    int sliceIdx;
  };
  Vector<RegenJob, 256> regenJobs;
  Vector<mesh::AttrRef> regenSrcRefs;
  const int nReq = int(requestedAttrs.size());

  for (int oi : util::IndexRange(regenOwners.size())) {
    plan_regen_gpu_node(regenOwners[oi], gpu, regenSrcRefs);
    GpuData &gd = *regenOwners[oi]->gpu_data;
    for (int si : util::IndexRange(gd.slices.size())) {
      if (gd.slices[si].vert_count > 0) {
        regenJobs.append({oi, si});
      }
    }
  }

  /* A slice-update job whose owner got planned for a full regen is redundant
   * (the plan cleared its leaf's flags and the regen fill rewrites the whole
   * buffer) — and its slice pointer may already be stale. Drop it. */
  if (regenOwners.size() > 0) {
    int out = 0;
    for (int i : util::IndexRange(sliceWork.size())) {
      if (!regenOwnerIds.contains(sliceWork[i].owner->id)) {
        sliceWork[out++] = sliceWork[i];
      }
    }
    sliceWork.resize(out);
  }

  /* Unified parallel pass: regen slice fills + in-place slice updates are the
   * same disjoint-sub-range write shape. Bodies are pure — no flag writes, no
   * GPUManager calls; all shared-state mutation (update_buffer flags, any
   * required full regen) is deferred to the serial epilogue below.
   * sliceOk[i] == 0 means sliceWork[i].owner needs a full rebuild. */
  Vector<uint8_t, 256> sliceOk;
  sliceOk.resize(sliceWork.size());
  /* Per-slice vert spans, filled by the parallel pass (each job writes only its
   * own index) and folded into the buffers' dirty ranges in the serial epilogue
   * so the TS upload can be partial. */
  Vector<int, 256> sliceVertStart, sliceVertCount;
  sliceVertStart.resize(sliceWork.size());
  sliceVertCount.resize(sliceWork.size());

  {
    const int regenJobCount = int(regenJobs.size());
    const int totalJobs = regenJobCount + int(sliceWork.size());
    auto runJob = [&](int i) {
      if (i < regenJobCount) {
        RegenJob &job = regenJobs[i];
        mesh::AttrRef *refs = (nReq > 0 && regenSrcRefs.size() > 0)
                                  ? regenSrcRefs.data() + job.ownerIdx * nReq
                                  : nullptr;
        fill_regen_slice(regenOwners[job.ownerIdx], job.sliceIdx, refs);
      } else {
        int k = i - regenJobCount;
        sliceVertStart[k] = 0;
        sliceVertCount[k] = 0;
        sliceOk[k] = update_gpu_node_slice(sliceWork[k].owner,
                                           sliceWork[k].leaf,
                                           gpu,
                                           &sliceVertStart[k],
                                           &sliceVertCount[k],
                                           sliceWork[k].geomOnly)
                         ? 1
                         : 0;
      }
    };

    // SC_FILL_SERIAL=1 forces the unified pass serial (regression A/Bs).
    static const bool forceSerialFill = std::getenv("SC_FILL_SERIAL") != nullptr;

#ifdef NO_PARALLEL_FOR
    const bool serialFill = true;
#else
    const bool serialFill = forceSerialFill;
#endif
    if (serialFill) {
      for (int i : util::IndexRange(totalJobs)) {
        runJob(i);
      }
    } else {
#ifndef NO_PARALLEL_FOR
      litestl::task::parallel_for(
          util::IndexRange(totalJobs),
          [&](IndexRange range) {
            for (int i : range) {
              runJob(i);
            }
          },
          4);
#endif
    }
  }

  /* Serial: flag each successfully-updated owner's buffers for re-upload, and
   * full-regen (once per owner) any owner whose in-place update failed. Doing
   * the regens here — not inside the parallel body — is what fixes the
   * "faces randomly don't draw" race: two threads regenning the same owner
   * concurrently could leave it with a null pos buffer, which the draw-batch
   * loop then silently skips. */
  Vector<SpatialNode *, 64> regennedOwners;
  for (int i : util::IndexRange(sliceWork.size())) {
    SpatialNode *owner = sliceWork[i].owner;
    if (sliceOk[i]) {
      /* Flag only the rewritten slice's vert span (all streams share vert
       * indexing), so the TS upload re-sends just that sub-range instead of
       * the whole owner buffer. */
      GpuData &gd = *owner->gpu_data;
      const int s = sliceVertStart[i];
      const int e = s + sliceVertCount[i];
      if (gd.pos) {
        gd.pos->markDirtyRange(s, e);
      }
      if (gd.nor) {
        gd.nor->markDirtyRange(s, e);
      }
      if (!sliceWork[i].geomOnly) {
        for (gpu::Buffer *b : gd.attrBufs) {
          if (b) {
            b->markDirtyRange(s, e);
          }
        }
      }
      continue;
    }

    bool already = false;
    for (SpatialNode *r : regennedOwners) {
      if (r == owner) {
        already = true;
        break;
      }
    }
    if (already) {
      continue;
    }
    regen_gpu_node(owner, gpu);
    regennedOwners.append(owner);
    drawBatchUpdated = true;
  }

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
      for (gpu::Buffer *b : gd.attrBufs) {
        drawBatch->buffers.append(b);
      }

      /* Once setDrawShader has built the material `drawShader`, draw with it —
       * even with zero requested attrs (a constant-color material reads no mesh
       * attributes; its VsIn is just position/normal). Otherwise the legacy
       * basic mesh shader (position, normal, color). */
      gpu::ShaderDef *shader =
          drawShaderReady ? &drawShader : &spatialShaders.basicMeshShader;

      if (!gd.cmd) {
        /* batch=nullptr: createCommand(batch) appends to the batch itself and
         * the loop tail appends again — a fresh command would land twice and
         * its node would draw twice until the next rebuild. */
        gd.cmd = gpu->createCommand(nullptr,
                                    gpu::GPUCmdType::DRAW_TRIS,
                                    shader,
                                    0,
                                    gd.pos->size,
                                    gd.pos->size / 3);
        gd.cmd->attrs.append(gd.pos);
        gd.cmd->attrs.append(gd.nor);
        /* @location(2+): per-attribute streams (always present after regen). */
        for (gpu::Buffer *b : gd.attrBufs) {
          gd.cmd->attrs.append(b);
        }
      }
      gd.cmd->primCount = gd.pos->size / 3;
      gd.cmd->end = gd.pos->size;
      drawBatch->commands.append(gd.cmd);
      // Owner AABB for view culling, parallel to `commands` (6 floats each).
      for (int k = 0; k < 3; k++) {
        drawBatch->cmdAabbs.append(node->aabb.min[k]);
      }
      for (int k = 0; k < 3; k++) {
        drawBatch->cmdAabbs.append(node->aabb.max[k]);
      }
    }
    drawBatch->version++;
    drawBatch->aabbVersion++;
    pendingCmdAabbs_ = false;
  } else if ((bounds || pendingCmdAabbs_) && drawBatch && drawBatch->commands.size() > 0) {
    /* Bounds moved without a command-list rebuild: refresh the culling AABBs
     * in place (same node iteration/filter as the rebuild loop above). */
    int idx = 0;
    const int n = int(drawBatch->cmdAabbs.size());
    for (SpatialNode *node : nodes) {
      if (!node->is_gpu_node || !node->gpu_data || !node->gpu_data->pos) {
        continue;
      }
      if ((idx + 1) * 6 > n) {
        idx = -1; /* count drifted — leave stale, next rebuild refills */
        break;
      }
      for (int k = 0; k < 3; k++) {
        drawBatch->cmdAabbs[idx * 6 + k] = node->aabb.min[k];
        drawBatch->cmdAabbs[idx * 6 + 3 + k] = node->aabb.max[k];
      }
      idx++;
    }
    if (idx >= 0 && idx * 6 == n) {
      drawBatch->aabbVersion++;
    }
    pendingCmdAabbs_ = false;
  }

  return result || drawBatchUpdated || gpuWorkDone;
}
} // namespace sculptcore::spatial
