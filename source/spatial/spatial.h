#pragma once

#include "node.h"

#include "litestl/math/vector.h"
#include "litestl/util/map.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "mesh/mesh_proxy.h"
#include "mesh/utils/triangulate.h"

#include "napi/napi_log.h"
#include "spatial_attrs.h"
#include "spatial_enums.h"

#include <cmath>
#include <mutex>


#include "gpu/batch.h"
#include "gpu/gpu_attr_request.h"
#include "gpu/manager.h"
#include "gpu/shader.h"

namespace sculptcore::gpu {
struct GPUManager;
struct DrawBatch;
} // namespace sculptcore::gpu

using namespace litestl;
namespace sculptcore::spatial {

/* Which halves of the update pipeline updateImpl runs. The queries half is
 * everything the next brush dab's spatial queries need (split/merge, tris,
 * bounds); normals are their own phase — raycasts and node filters don't read
 * vertex normals, so per-dab callers skip them and the per-frame update
 * refreshes every leaf dirtied since the last frame once, not once per dab.
 * The GPU half is the buffer pipeline (partition, dirty-bit propagation, plan,
 * fill, upload, draw-batch rebuild). */
enum UpdatePhases {
  Update_Queries = 1 << 0,
  Update_Gpu = 1 << 1,
  Update_Normals = 1 << 2,
  Update_All = Update_Queries | Update_Normals | Update_Gpu,
};

struct SpatialTree {
  using Mesh = mesh::Mesh;

  int leaf_limit = 512;
  int depth_limit = 10;
  /* True once any face has a nonzero `.detail.bound` displacement bound, so
   * regen_node_bounds only pays the per-face pad scan when a carrier is live.
   * Set by setFaceDisplacementBounds; never cleared (bounds regen to zero pad
   * naturally once the bounds column is zeroed). */
  bool hasDetailBounds = false;
  /* Target tri count per GPU mesh. The set of "GPU nodes" is chosen so
   * each owns a subtree whose tri count is <= this target (a single leaf
   * exceeding the target becomes its own GPU node). Independent of
   * leaf_limit, so leaves can stay small for spatial queries / brush
   * iteration while GPU draws are batched. */
  int gpu_tri_target = 2048;

  /* Bitmask of attributes feeding the per-vertex `gd.color` render stream:
   * bit 0 (1) = vertex `color` attr, bit 1 (2) = per-face `group` id (hashed).
   * Both bits composite; 0 = plain white. Set via setColorDisplayMode(), which
   * re-fills every GPU node's color. */
  int displayColorMode = 1;

  /* Which layer (by index in its element AttrGroup) feeds each display source,
   * so the viewport can show the user-selected *active* color/group attr rather
   * than always the layer literally named "color"/"group". -1 = fall back to
   * the by-name default. Set via setDisplayColorAttr/setDisplayGroupAttr (both
   * flag every leaf for a color re-fill). Index-addressed because layer names
   * can't cross the binding. */
  int displayColorAttr = -1;
  int displayGroupAttr = -1;

  /** Darken masked vertices in the solid display (the sculpt-mask overlay,
   * ImmediateTODOs #20). Independent of displayColorMode so the mask shows over
   * any color source. Default on; toggled via setDisplayMask(), which re-fills
   * the GPU color buffers. */
  bool displayMask = true;

  /* Dynamic attribute set requested by the active material's shader (M4). Empty
   * => legacy single-color render stream + basicMeshShader (sculpt/paint
   * display, behaviour-identical to before). Non-empty => one vertex buffer per
   * entry (slot order) gathered from the mesh, drawn with `drawShader` (the
   * material's WGSL). Set via setRequestedAttrs(); bumps requestedAttrsVersion
   * and flags every leaf for a GPU regen. */
  util::Vector<gpu::RequestedAttr> requestedAttrs;
  uint64_t requestedAttrsVersion = 0;

  /** GPU node buffer-layout generation: bumped whenever a GPU node's
   * aggregated buffers are (re)allocated or the partition changes
   * (regen_gpu_node / assign_gpu_nodes). Callers caching per-corner scatter
   * tables (buildGpuScatterTables) key on it — an unchanged gen means every
   * node's pos/nor identity, slice layout, and corner order are still valid. */
  uint64_t gpuLayoutGen = 1;
  /* Slots (RequestedAttr::slot) whose source layer is absent from the mesh, so
   * their buffer was default-filled. Advisory feedback for the renderengine;
   * recomputed by computeMissingAttrSlots(). Never causes a throw. */
  util::Vector<int> missingAttrSlots;
  /* Material draw shader for the requested-attr path (attrs = position, normal,
   * + requestedAttrs by slot; WGSL from setDrawShader). Default-constructed and
   * unused until setDrawShader() runs (drawShaderReady). */
  gpu::ShaderDef drawShader;
  bool drawShaderReady = false;

  SpatialTreeMesh treeMesh;
  bool done_gpu_assignment = false;
  Mesh *m;

  SpatialTree(Mesh *m_) : m(m_)
  {

    root = alloc_node();
    root->flag =
        Spatial_Leaf | Spatial_RegenTris | Spatial_RegenGPU | Spatial_UpdateNormals;
    root->create_data();
  }

  bool filterNodes(float3 co, float radius, Vector<SpatialNode *> &out);

  /* Switch which attribute the mesh render colors by (see displayColorMode)
   * and flag every leaf so the next update() re-fills the color stream. */
  void setColorDisplayMode(int mode);

  /* Point the color / poly-group display source at a specific layer index
   * (-1 = by-name default). Each flags every leaf for a color re-fill. */
  void setDisplayColorAttr(int index);
  void setDisplayGroupAttr(int index);
  /** Toggle the sculpt-mask darkening overlay (#20). */
  void setDisplayMask(bool on);

  /* Install the material's requested attribute set (M4). Early-returns when the
   * set is unchanged (so nothing rebuilds per frame); otherwise bumps
   * requestedAttrsVersion, recomputes missingAttrSlots, clears the draw batch,
   * and flags every leaf Spatial_RegenGPU so the next update() rebuilds the
   * per-attribute vertex buffers. Pass an empty set to return to the legacy
   * single-color path. */
  void setRequestedAttrs(const util::Vector<gpu::RequestedAttr> &reqs);

  /* Set the WGSL source for the requested-attr draw shader and (re)build +
   * link its ShaderDef (attrs = position, normal, + requestedAttrs by slot).
   * Flags every leaf for a GPU regen so draw commands repoint at it. No-op-safe
   * before setRequestedAttrs (the shader just goes unused until the set is
   * non-empty). */
  void setDrawShader(const char *wgsl);

  /* Re-resolve which requested slots have no matching mesh layer (advisory).
   * Called by setRequestedAttrs; the renderengine can call it again after a
   * mesh attribute edit. */
  void computeMissingAttrSlots();
  util::Vector<int> &getMissingAttrSlots()
  {
    return missingAttrSlots;
  }

  /* Force a rebuild of the per-attribute vertex buffers against the *current*
   * mesh layers, even when the requested descriptor set is byte-identical.
   * setRequestedAttrs short-circuits on an unchanged set, but adding/removing a
   * mesh layer whose domain matches the category default leaves the descriptors
   * unchanged while the buffer contents (default-fill vs real data) must change.
   * Recomputes missingAttrSlots, bumps requestedAttrsVersion, drops the draw
   * batch, and reflags every leaf Spatial_RegenGPU. The renderengine calls this
   * (instead of re-issuing setDrawShader) when only the mesh's attribute layers
   * changed. */
  void refreshRequestedAttrs();

  /* Brush/circle select: faces + verts inside a view cone (object-local), built
   * JS-side exactly like the WebGL BVH path. Face indices are deduped (a face
   * spans 2 tris). Out-params are appended to; returns true if anything hit. */
  bool castScreenCircle(const math::float3 &co,
                        const math::float3 &ray,
                        float r1,
                        float r2,
                        util::Vector<int> &faces_out,
                        util::Vector<int> &verts_out)
  {
    util::Set<int> faceSet, vertSet;

    root->collectConeFaces(co, ray, r1, r2, faceSet);
    root->collectConeVerts(co, ray, r1, r2, vertSet);

    for (int f : faceSet) {
      faces_out.append(f);
    }
    for (int v : vertSet) {
      verts_out.append(v);
    }

    return faces_out.size() > 0 || verts_out.size() > 0;
  }

  /* Box select: faces + verts inside a screen-rectangle frustum. The volume is
   * given as its 8 unprojected corners (object-local: 0..3 near plane, 4..7 far
   * plane) passed as individual float3 args — float3 arrays can't cross the WASM
   * boundary, and individual float3 params marshal cleanly (like castRay). The 6
   * inward planes are built (auto-oriented) in C++ via buildScreenRectPlanes. */
  bool castScreenRect(const math::float3 &near0,
                      const math::float3 &near1,
                      const math::float3 &near2,
                      const math::float3 &near3,
                      const math::float3 &far0,
                      const math::float3 &far1,
                      const math::float3 &far2,
                      const math::float3 &far3,
                      util::Vector<int> &faces_out,
                      util::Vector<int> &verts_out)
  {
    const math::float3 corners[8] = {near0, near1, near2, near3, far0, far1, far2, far3};
    math::float4 planes[6];
    buildScreenRectPlanes(corners, planes);

    util::Set<int> faceSet, vertSet;

    root->collectFrustumFaces(planes, 6, faceSet);
    root->collectFrustumVerts(planes, 6, vertSet);

    for (int f : faceSet) {
      faces_out.append(f);
    }
    for (int v : vertSet) {
      verts_out.append(v);
    }

    return faces_out.size() > 0 || verts_out.size() > 0;
  }

  bool castRay(const math::float3 &orig, const math::float3 &dir, CastRayIsect &out)
  {
    out.t = std::numeric_limits<float>::max();

    if (root->castRay(orig, dir, out)) {
      SpatialNode *node = nodes[out.nodeIndex];
      NodeTri &tri = node->data->tris[out.triIndex];
      auto *m = node->data->m;

      out.faceIndex = tri.f;

      float w = 1.0 - out.uv[0] - out.uv[1];

      // calculate position/normal
      int v1 = m->c.v[tri.c[0]];
      int v2 = m->c.v[tri.c[1]];
      int v3 = m->c.v[tri.c[2]];

      float3 co1 = m->v.co[v1];
      float3 co2 = m->v.co[v2];
      float3 co3 = m->v.co[v3];
      out.p = co1 * w + co2 * out.uv[0] + co3 * out.uv[1];

      float3 no1 = m->v.no[v1];
      float3 no2 = m->v.no[v2];
      float3 no3 = m->v.no[v3];

      out.normal = no1 * w + no2 * out.uv[0] + no3 * out.uv[1];
      out.normal.normalize();

      // Vertex nearest the hit point = the corner with the largest barycentric
      // weight (w, uv[0], uv[1] for v1, v2, v3).
      if (w >= out.uv[0] && w >= out.uv[1]) {
        out.nearestVert = v1;
      } else if (out.uv[0] >= out.uv[1]) {
        out.nearestVert = v2;
      } else {
        out.nearestVert = v3;
      }

      return true;
    }
    return false;
  }

  /* Root node accessor for read-only tree walks (closest-point query, M1). */
  SpatialNode *getRoot()
  {
    return root;
  }

  void setup()
  {
    treeMesh.setup(m);
    /* Root's data is created before treeMesh.m is assigned (in our ctor),
     * so its m pointer is stale until we patch it here. */
    if (root && root->data) {
      root->data->m = m;
    }
  }

  void update_node_normals(SpatialNode *node);

  bool node_needs_split(SpatialNode *node)
  {
    return node->data->unique_verts.size() >= leaf_limit && node->depth < depth_limit;
  }

  /* `claimTag` is the "unassigned" marker this split writes into its verts'
   * `.spatial.v.node` entries and the only value its re-file may claim (0
   * serially). The parallel deferred-split pass gives each candidate a unique
   * negative tag so a candidate can never claim a boundary vert another
   * candidate unassigned mid-race — which both prevents double-claims and
   * keeps the result bit-identical to the serial candidate order (serially, a
   * candidate only ever claims its own unassigned verts too). */
  void split_node(SpatialNode *node, int claimTag = 0);

  /* Pick leaf_limit / gpu_tri_target from the mesh size instead of fixed
   * constants. Call before buildAll(). Derived from the bench_spatial sweep:
   *   - leaf_limit: ~512 is the build-time/culling sweet spot and is flat in
   *     vertex terms across mesh sizes; shrink it only so a small mesh still
   *     yields enough leaves for parallel update + brush culling.
   *   - gpu_tri_target: sized to a target draw-call count (draw calls ~=
   *     total_tris / gpu_tri_target), clamped so tiny meshes don't fragment and
   *     a single node's VBO regen stays bounded. */
  void autoTuneLimits();

  ~SpatialTree()
  {
    for (SpatialNode *node : nodes) {
      alloc::Delete(node);
    }
    if (drawBatch) {
      alloc::Delete(drawBatch);
    }
  }

  SpatialNode *node_from_id(int id)
  {
    /* An ownership id (.spatial.{v,f}.node) outside the live node map is not a
     * live node: ids are monotonic (never reused) and rebuild() resets the map,
     * so a stale/garbage attr value can exceed its size. Return null — callers
     * all null-check and fall back (add_face → centroid descent). Reading
     * node_idmap[id] unchecked here was a wild OOB read feeding a garbage
     * SpatialNode* into add_face_at. */
    if (id < 0 || id >= int(node_idmap.size())) {
      return nullptr;
    }
    return node_idmap[id];
  }

  /* A leaf that already owns one of `face`'s verts, or null if none is owned
   * yet (the build path). Dyntopo's new geometry is spatially adjacent to
   * existing geometry, so such a vert pins the new face to the right
   * neighbourhood without a root→leaf descent (M7.6 locality shortcut). */
  SpatialNode *find_anchor_leaf(mesh::FaceProxy &face)
  {
    for (auto list : face.lists()) {
      for (auto c : list) {
        int nid = treeMesh.v.node[c.v()];
        if (nid == 0) {
          continue;
        }
        SpatialNode *n = node_from_id(nid);
        if (n && (n->flag & Spatial_Leaf) && n->data) {
          return n;
        }
      }
    }
    return nullptr;
  }

  /* O(1) placement: file `f` directly into `leaf` (no centroid descent), and do
   * NOT split inline — over-full leaves are recorded in nodeSplitCandidates_ and
   * split once, batched, by applyDeferredNodeSplit() at the next update(). The
   * leaf's AABB is left loose until then (regen_node_bounds tightens it from the
   * new tris during update). Mirrors add_face_intern's leaf body. */
  void add_face_at(SpatialNode *leaf, int f)
  {
    leaf->flag |= Spatial_RegenTris | Spatial_RegenBounds | Spatial_RegenGPU;
    /* Mark ancestors bounds-dirty so regen_node_bounds (which descends only into
     * RegenBounds-flagged children) reaches this leaf. Every setter walks to
     * root and update() clears the flags top-down, so an already-flagged ancestor
     * means the rest of the chain to root is already marked — stop there (turns
     * the per-face-op O(depth) walk into O(1) for repeat ops in the same leaf). */
    for (SpatialNode *p = leaf->parent; p && !(p->flag & Spatial_RegenBounds);
         p = p->parent)
    {
      p->flag |= Spatial_RegenBounds;
    }

    mesh::FaceProxy face(m, f);
    if (treeMesh.f.node[face] == 0) {
      treeMesh.f.node[face] = leaf->id;
      leaf->data->unique_faces.add(f);
    }

    for (auto list : face.lists()) {
      for (auto c : list) {
        int vn = treeMesh.v.node[c.v()];
        if (vn == leaf->id) {
          continue; /* already this leaf's unique vert */
        }
        if (!vn) {
          leaf->data->unique_verts.add(c.v());
          treeMesh.v.node[c.v()] = leaf->id;
        }
      }
    }

    if (node_needs_split(leaf)) {
      nodeSplitCandidates_.add(leaf->id);
      /* Growth-skew hint: a filling leaf is where a lopsided pair forms (its
       * sibling may never grow). Record the parent; node_is_skewed re-checks,
       * so a non-skewed hint is discarded cheaply. */
      if (leaf->parent) {
        mergeCandidates_.add(leaf->parent->id);
      }
    }
  }

  void clear_face_leaf_ref(int f)
  {
    treeMesh.f.node[f] = 0;
  }
  void clear_vert_leaf_ref(int v)
  {
    treeMesh.v.node[v] = 0;
  }

  void add_face(int f, bool search_node = true)
  {
    mesh::FaceProxy face(m, f);

    // ensure face has empty node ref
    treeMesh.f.node[face] = 0;

    /* Incremental (dyntopo) fast path: pin the new face to a neighbour's leaf in
     * O(1) and defer the split. The build path (no owned neighbour yet) falls
     * back to the root→leaf centroid descent. */
    SpatialNode *anchor = nullptr;
    if (search_node && (anchor = find_anchor_leaf(face))) {
      add_face_at(anchor, f);
      return;
    }

    math::float3 fcent = face.calc_center();

    if (root->aabb.min[0] == FLT_MAX) {
      root->aabb.min = fcent;
      root->aabb.max = fcent;
    } else {
      root->aabb.min.min(fcent);
      root->aabb.max.max(fcent);
    }

    util::Vector<Tri, 16> tris;
    if (triangulateFace(*m, f, tris)) {
      std::span<Tri> tris_span = tris;

      bool ok = true;
      double area = 0.0;

      for (auto tri : tris_span) {
        auto &co1 = m->v.co[tri.v[0]];
        auto &co2 = m->v.co[tri.v[1]];
        auto &co3 = m->v.co[tri.v[2]];
        double area2 = math::triArea(co1, co2, co3);
        if (std::isnan(area2) || !std::isfinite(area2)) {
          ok = false;
          sc_napi_logf("got a nan face at %d\n", f);
          break;
        }
        area += area2;
      }

      // XXX magic number
      if (area < 0.0000001) {
        sc_napi_logf("got a zero or near zero area face at %d, area=%lf\n", f, area);
        ok = false;
      }
      if (ok) {
        add_face_intern(root, f, fcent);
      }
    } else {
      printf("failed to triangulate face %d\n", f);
    }
  }

  /* Incremental removal: drop face `f` from its owning leaf (the inverse of
   * add_face), marking the leaf for tris/bounds regen. Used by
   * the dyntopo callbacks so the tree need not be fully rebuilt per dab. */
  void remove_face(int f)
  {
    int node_id = treeMesh.f.node[f];
    if (node_id == 0) {
      return; /* not the unique owner */
    }
    SpatialNode *node = node_from_id(node_id);
    treeMesh.f.node[f] = 0;
    if (!node || !node->data) {
      return;
    }
    node->data->unique_faces.remove(f);
    node->flag |= Spatial_RegenTris | Spatial_RegenBounds | Spatial_RegenGPU;
    /* See add_face_at: stop at the first already-flagged ancestor. */
    for (SpatialNode *p = node->parent; p && !(p->flag & Spatial_RegenBounds);
         p = p->parent)
    {
      p->flag |= Spatial_RegenBounds;
    }
  }

  /* Incremental update for a face rewired IN PLACE (id unchanged) by an
   * in-place Euler op (flipEdge / splitEdge): re-flag the owning leaf for
   * tris/bounds/GPU regen so its draw buffers pick up the new connectivity,
   * and claim any verts the rewired face now references that no leaf owns yet
   * (mirrors add_face_at's vert loop — e.g. a split's fresh midpoint). The
   * face count in the leaf is unchanged, so no split is triggered. Falls back
   * to add_face if the face has no unique owner (shouldn't happen for a live
   * dab face). Used by the onFaceChange spatial callback. */
  void touch_face(int f)
  {
    int node_id = treeMesh.f.node[f];
    if (node_id == 0) {
      add_face(f);
      return;
    }
    SpatialNode *node = node_from_id(node_id);
    if (!node || !node->data) {
      return;
    }
    node->flag |= Spatial_RegenTris | Spatial_RegenBounds | Spatial_RegenGPU;
    /* See add_face_at: stop at the first already-flagged ancestor. */
    for (SpatialNode *p = node->parent; p && !(p->flag & Spatial_RegenBounds);
         p = p->parent)
    {
      p->flag |= Spatial_RegenBounds;
    }

    mesh::FaceProxy face(m, f);
    for (auto list : face.lists()) {
      for (auto c : list) {
        int vn = treeMesh.v.node[c.v()];
        if (vn == node->id) {
          continue;
        }
        if (!vn) {
          node->data->unique_verts.add(c.v());
          treeMesh.v.node[c.v()] = node->id;
        }
      }
    }
    /* No deferred-split bookkeeping here: a split's other half-face is add_face'd
     * into this same leaf (it shares the new midpoint), so add_face_at already
     * queues the rebalance when the leaf grows over the limit. */
  }

  /* Re-flag every leaf holding a face incident to one of `verts` for tris/bounds/
   * GPU regen, so a direct positional edit (the box-modeling transform bridge's
   * setVertCo pass) shows up on the next update() without a full tree rebuild.
   * Leaf AABBs grow loose (RegenBounds) rather than re-bucketing — a confirm-time
   * rebuild re-optimizes. */
  void markVertsMoved(util::Vector<int> &verts)
  {
    for (int v : verts) {
      if (v < 0 || size_t(v) >= m->v.capacity() || m->v.freemap[v]) {
        continue;
      }
      for (int e : m->e_of_v(v)) {
        int c0 = m->e.c[e];
        if (c0 == ELEM_NONE) {
          continue;
        }
        int c = c0;
        do {
          int f = m->l.f[m->c.l[c]];
          int nid = treeMesh.f.node[f];
          if (nid != 0) {
            SpatialNode *node = node_from_id(nid);
            if (node) {
              node->flag |= Spatial_RegenTris | Spatial_RegenBounds | Spatial_RegenGPU;
              for (SpatialNode *p = node->parent; p && !(p->flag & Spatial_RegenBounds);
                   p = p->parent)
              {
                p->flag |= Spatial_RegenBounds;
              }
            }
          }
          c = m->c.radial_next[c];
        } while (c != c0);
      }
    }
  }

  /* Set the per-face conservative max|D| displacement bound (`.detail.bound`)
   * for `count` faces and flag their owning leaves Spatial_RegenBounds, so the
   * next update() re-pads the AABBs (see regen_node_bounds). This is the
   * carrier's tile-edit dirty hook — bounds-only: no tris or GPU regen, a
   * displacement-bound change moves no CPU geometry. */
  void setFaceDisplacementBounds(const int *faces, const float *bounds, int count)
  {
    for (int i = 0; i < count; i++) {
      int f = faces[i];
      if (f < 0 || size_t(f) >= m->f.capacity() || m->f.freemap[f]) {
        continue;
      }
      treeMesh.f.bound.get_data()->materialize(f);
      treeMesh.f.bound[f] = bounds[i];
    }
    hasDetailBounds = true;
    markFacesDisplacementDirty(std::span<const int>(faces, size_t(count)));
  }

  /* Flag the leaves owning `faces` Spatial_RegenBounds (+ ancestor walk) after
   * their displacement bounds changed. Bounds-only by design. */
  void markFacesDisplacementDirty(std::span<const int> faces)
  {
    for (int f : faces) {
      if (f < 0 || size_t(f) >= m->f.capacity() || m->f.freemap[f]) {
        continue;
      }
      int nid = treeMesh.f.node[f];
      if (nid == 0) {
        continue;
      }
      SpatialNode *node = node_from_id(nid);
      if (!node) {
        continue;
      }
      node->flag |= Spatial_RegenBounds;
      /* See add_face_at: stop at the first already-flagged ancestor. */
      for (SpatialNode *p = node->parent; p && !(p->flag & Spatial_RegenBounds);
           p = p->parent)
      {
        p->flag |= Spatial_RegenBounds;
      }
    }
  }

  /* Incremental removal of a killed vertex from its owning leaf. */
  void remove_vert(int v)
  {
    int node_id = treeMesh.v.node[v];
    treeMesh.v.node[v] = 0;
    if (node_id == 0) {
      return;
    }
    SpatialNode *node = node_from_id(node_id);
    if (node && node->data) {
      node->data->unique_verts.remove(v);
      /* This leaf shrank; its parent may now have two under-full leaf children.
       * Record it for the periodic merge pass (M7.6b) — eligibility (both
       * children still leaves, combined verts under the low watermark) is
       * re-checked there, so a stale candidate is harmless. */
      if (node->parent) {
        mergeCandidates_.add(node->parent->id);
      }
    }
  }

  /* MeshCallbacks that keep node ownership current as topology changes, so the
   * tree need not be fully rebuilt after a dyntopo dab — call update()
   * afterward to process the accumulated dirty flags. Pass to the mesh
   * topology ops (or fan out alongside the meshlog callbacks). */
  mesh::MeshCallbacks *getSpatialCallbacks()
  {
    spatialCallbacks_.onFaceCreate = [this](int f) { this->add_face(f); };
    spatialCallbacks_.onFaceKill = [this](int f) { this->remove_face(f); };
    spatialCallbacks_.onFaceChange = [this](int f) { this->touch_face(f); };
    spatialCallbacks_.onVertKill = [this](int v) { this->remove_vert(v); };
    return &spatialCallbacks_;
  }

  util::Vector<SpatialNode *> leaves();
  util::Vector<SpatialNode *> gpu_nodes();

  /* Split the leaves that grew past leaf_limit during incremental add_face_at
   * placement, once each, batched (M7.6 deferred rebalance). Called at the top
   * of update(); public so tests can drive it without a GPUManager. Thaws
   * topology if frozen (split_node re-triangulates via live links). */
  void applyDeferredNodeSplit();

  /* Fold under-full sibling leaves back into their parent after collapse-heavy
   * strokes shrink a region (M7.6b), cascading up the chain. The inverse of the
   * deferred split: a parent whose two leaf children together own fewer than
   * leaf_limit/2 verts becomes a leaf again and re-absorbs their geometry; the
   * children are freed. Runs on a slow cadence inside update() (mergeCadence_),
   * NOT every dab; public so tests / a stroke-end hook can force it. Thaws
   * topology if frozen (re-files via live links). */
  void applyDeferredMerge();

  /* Propagate Spatial_RegenBounds flags to ancestors and regen all dirty
   * AABBs (the update() bounds phase alone). Returns whether anything was
   * dirty. Public so tests and the displacement-bounds hook can refresh
   * bounds without a GPUManager. */
  bool regenDirtyBounds();

  /* Public so tests can drive partition assignment without a GPUManager. */
  void recompute_subtree_tri_counts();
  void assign_gpu_nodes();
  SpatialNode *find_gpu_owner(SpatialNode *node);

  bool ensure_node_tris(SpatialNode *node)
  {
    if (node->flag & Spatial_RegenTris) {
      regen_node_tris(node);
      return true;
    }

    return false;
  }

  void buildAll();
  /* Serial incremental build (root->leaf face insertion). Kept as the
   * reference/fallback; buildAll() dispatches to it under SC_SERIAL_BUILD. */
  void buildAllSerial();
  /* Parallel top-down build: bulk recursive spatial-median partition of the
   * faces (level-synchronous, one task fan-out per level), then an atomic
   * lowest-incident-face vertex-ownership pass. Produces a valid tree with the
   * same leaf/ownership invariants as the serial build, ~5x faster at 1M. */
  void buildAllParallel();

  /* Build vert/edge/corner/list/face permutations (map[old] = new) that group
   * each domain's elements next to the other elements of their owning node,
   * improving cache locality of brush iteration. Each map is a full bijection
   * over its domain's storage capacity (free slots land at the tail, so the
   * mesh is also compacted). Reads ownership from the current node set. */
  void computeLocalityMaps(util::Vector<int> &vmap,
                           util::Vector<int> &emap,
                           util::Vector<int> &cmap,
                           util::Vector<int> &lmap,
                           util::Vector<int> &fmap);

  /* Partial locality map: relocate ONLY @p dirtyLeaves' elements, as a closed
   * permutation over the slots those leaves already occupy (each leaf's elements
   * re-sorted into a contiguous sub-run of that slot set). Every other element —
   * clean leaves, free slots — maps to itself, so the result is a mostly-identity
   * full bijection over each domain's capacity.
   * @p moved (optional, points at an array of 5 vectors v/e/c/l/f) receives the
   * moved (live) slot list per domain — the closed-permutation set the scoped
   * applyReorderIncremental consumes, and the "mostly identity" proof. */
  void computeLocalityMapsPartial(util::span<SpatialNode *> dirtyLeaves,
                                  util::Vector<int> &vmap,
                                  util::Vector<int> &emap,
                                  util::Vector<int> &cmap,
                                  util::Vector<int> &lmap,
                                  util::Vector<int> &fmap,
                                  util::Vector<int> *moved = nullptr);

  /* Apply precomputed permutations to the mesh, then rebuild the tree (node
   * data caches stale indices after a reorder). buildAll is deterministic, so
   * an inverse-permutation reorder reproduces the prior node set exactly — the
   * property the meshlog reorder chunk relies on for cross-step undo. */
  void applyReorder(util::span<int> vmap,
                    util::span<int> emap,
                    util::span<int> cmap,
                    util::span<int> lmap,
                    util::span<int> fmap);

  /* Like applyReorder, but WITHOUT rebuilding the tree: a reorder is a pure
   * index permutation (geometry values, node partition, bounds, normals and GPU
   * buffers are unchanged — only storage indices move, and ownership attrs ride
   * along with reorder_*). So we just remap each node's cached element indices
   * (unique_verts/unique_faces via vmap/fmap, NodeTri.c[]/.f via cmap/fmap) in
   * place. O(elements) instead of buildAll's O(mesh log mesh) — the scalable
   * compaction path (mechanism B). The maps must be full bijections over each
   * domain's capacity (SpatialTree::computeLocalityMaps produces such). */
  /* @p *moved (optional, per domain): when non-empty, the attribute permutation
   * for that domain is applied scoped to just those live slots (the closed-
   * permutation moved set from computeLocalityMapsPartial) instead of a full-array
   * rewrite. The reference fix-up + node-cache remap stay full for now. */
  void applyReorderIncremental(util::span<int> vmap,
                               util::span<int> emap,
                               util::span<int> cmap,
                               util::span<int> lmap,
                               util::span<int> fmap,
                               util::span<int> vmoved = {},
                               util::span<int> emoved = {},
                               util::span<int> cmoved = {},
                               util::span<int> lmoved = {},
                               util::span<int> fmoved = {});

  /* Compute + apply locality maps in one shot (no undo recording). */
  void reorderForLocality();

  /* DRAM-locality diagnostics. For each leaf, how many distinct attribute pages
   * (ATTR_PAGESIZE-sized) its verts/faces are spread across vs. the ideal
   * (ceil(count/ATTR_PAGESIZE)); summed across leaves. ratio = actual/ideal:
   * 1.0 = perfectly compact, higher = more fragmented (worse cache locality for
   * brush iteration / GPU buffer fill). Read-only; used to measure mechanism A
   * and to score leaves for compaction. */
  struct FragStats {
    int leaves = 0;
    int64_t vertCount = 0, vertPagesActual = 0, vertPagesIdeal = 0;
    int64_t faceCount = 0, facePagesActual = 0, facePagesIdeal = 0;
    double vertRatio = 1.0;
    double faceRatio = 1.0;
  };
  FragStats fragmentationStats();

  /* Material-fragmentation diagnostics: how far the per-face material attr
   * cuts across the tree's spatial grouping. Appends (id, distinctSlots,
   * slotMask) triples -- one per leaf when @p perLeaf, else one per GPU node
   * in drawBatch->commands order (same filter, so triple i matches command i).
   * Per-GPU-node counts predict the draw-command multiplier of splitting draws
   * by material; per-leaf counts say whether whole LeafSlices can be reordered
   * by slot instead of sorting tris within a leaf. slotMask covers slots 0..30,
   * folding anything >= 31 into bit 31. */
  void materialStats(bool perLeaf, util::Vector<int> &out);

  /* Region selection for partial compaction: append every leaf whose vert
   * page-spread exceeds @p ratioThreshold × its ideal page count (i.e. the
   * fragmented leaves a stroke just churned). */
  void selectFragmentedLeaves(double ratioThreshold,
                              util::Vector<SpatialNode *> &out);

  /* Tear down every node and rebuild from the (possibly reordered) mesh. */
  void rebuild();

  sculptcore::gpu::DrawBatch *getDrawBatch()
  {
    return drawBatch;
  }
  sculptcore::gpu::DrawBatch *buildLeafBoundsBatch(sculptcore::gpu::GPUManager &mgr);

  /* Build a DRAW_LINES batch of the whole mesh's AABB as one white 12-edge box
   * (the per-object BOUNDS draw mode / selection overlay; host tints via
   * uColor). Returns nullptr when the mesh has no geometry. */
  sculptcore::gpu::DrawBatch *buildBoundsBatch(sculptcore::gpu::GPUManager &mgr);

  /* Build a DRAW_LINES batch of the mesh's marked seam edges (boundary
   * EDGE_SEAM) for the persistent viewport overlay. Returns nullptr when no
   * edge is flagged (caller skips the dispatch). Thaws frozen topology to read
   * live edge endpoints; the batch is a static VBO, so callers rebuild it only
   * when the seam set / geometry changes, not per frame. */
  /** `includePolyGroup` adds the poly-group boundary edges (magenta) to the
   * overlay; off by default since they're a separate, opt-in toggle (#28). */
  sculptcore::gpu::DrawBatch *buildSeamBatch(sculptcore::gpu::GPUManager &mgr,
                                             bool includePolyGroup = false);

  /* Build the box-modeling selection overlay batch: selected faces as
   * translucent fan-tris, selected edges as lines, selected verts as small
   * crosses, with each domain's active element (-1 = none) highlighted white
   * and each domain's hover element (-1 = none) highlighted cyan. Returns
   * nullptr when nothing is selected or hovered. Like buildSeamBatch it floats
   * the geometry out along vertex normals; a static VBO rebuilt on change. */
  sculptcore::gpu::DrawBatch *buildSelectionBatch(sculptcore::gpu::GPUManager &mgr,
                                                  int activeVert,
                                                  int activeEdge,
                                                  int activeFace,
                                                  int hoverVert = -1,
                                                  int hoverEdge = -1,
                                                  int hoverFace = -1);

  /* Build the box-modeling wireframe overlay: every edge as a dim line, floated
   * out along vertex normals (same trick as buildSelectionBatch) so it hovers
   * just above the surface. Returns nullptr for an empty mesh. Static VBO,
   * rebuilt on geometry change. */
  sculptcore::gpu::DrawBatch *buildWireframeBatch(sculptcore::gpu::GPUManager &mgr);

  /* Build the billboard vertex-point overlay: every vertex as a screen-facing,
   * pixel-constant-size round point (two triangles on the point-sprite shader,
   * with a per-vertex `corner` attribute). Floated out along the normal like the
   * other overlays. Returns nullptr for an empty mesh. */
  sculptcore::gpu::DrawBatch *buildPointsBatch(sculptcore::gpu::GPUManager &mgr);

  static binding::types::Struct<SpatialTree> *defineBindings();

  bool update(gpu::GPUManager *gpu);

  /* Queries half only (per-dab): split/merge, tris, bounds, normals. Leaves
   * every GPU dirty bit untouched for a later update(gpu) — the draw frame —
   * to consume. Returns today's update() semantics: bounds changed. */
  bool updateQueries();

  /* GPU-resident stroke (debug app). While true, update()'s GPU phase leaves a
   * GPU node's pos/nor untouched when they are gpu_owned — the scatter compute
   * pass owns their contents for the duration of the stroke. */
  bool gpuStrokeActive = false;

  /* Build the slot->global-vertex map for one GPU node into gd.slotVertex
   * (allocated via `gpu`, host-uploaded), matching the gd.pos/gd.nor slot order
   * exactly so the scatter pass can fan co/no into the render VBOs. Also flips
   * gd.pos/gd.nor to gpu_storage|gpu_owned. The node's GpuData (slices /
   * total_verts) must already be current. */
  void buildGpuNodeSlotVertex(SpatialNode *gpu_node, gpu::GPUManager *gpu);

  /** Corner->global-vertex scatter tables for every GPU node with live
   * buffers (gpuGlobalBrushes.md M3). Appends 6 u32 per node to `meta`
   * (pos/nor gpu::Buffer addresses as lo,hi identity keys — the TS batch
   * executor's bufferKey — then corner offset+count into `map`) and one
   * global vert id per render corner to `map`, in the exact regen_gpu_node
   * fill order (fill_leaf_slot_verts per slice). `owners` (optional) gets the
   * node of each meta record, index-aligned. `fillMap=false` skips the corner
   * walk (meta/owners only — for callers whose map is cached). */
  void buildGpuScatterTables(util::Vector<uint32_t> &meta,
                             util::Vector<uint32_t> &map,
                             util::Vector<SpatialNode *> *owners = nullptr,
                             bool fillMap = true);

private:
  /* Shared body of update()/updateQueries(). `gpu` may be nullptr when
   * `phases` excludes Update_Gpu (never dereferenced there). */
  bool updateImpl(gpu::GPUManager *gpu, UpdatePhases phases);

  /* Sticky across the split calls: the queries half regenerated leaf tris (or
   * split/merge restructured nodes, which flags fresh leaves RegenTris), so
   * the next GPU half must recompute subtree tri counts + reassign the GPU
   * partition. Set by the queries half, consumed + cleared by the GPU half. */
  bool pendingGpuTopology_ = false;

  /* Sticky like pendingGpuTopology_: a queries-half bounds refit ran (possibly
   * in a per-dab updateQueries() call), so the next GPU half must refresh the
   * draw batch's per-command culling AABBs even without a rebuild. */
  bool pendingCmdAabbs_ = false;

  sculptcore::gpu::DrawBatch *drawBatch = nullptr;
  void regen_node_bounds(SpatialNode *node, bool recurse);
  void regen_node_tris(SpatialNode *node);

  /* GPU node buffer management. A "GPU node" aggregates the triangles of
   * every leaf in its subtree into one VBO + draw command. */
  void regen_gpu_node(SpatialNode *gpu_node, gpu::GPUManager *gpu);
  /* Serial planning half of a full GPU-node regen: dispose old buffers,
   * collect subtree leaves (regen-ing stale tris), allocate pos/nor/attr
   * buffers, build the slice table, clear the subtree leaves' GPU dirty
   * flags, and resolve each requested attribute's source layer once
   * (appended to `srcRefs`, requestedAttrs.size() entries, dynamic path
   * only). The fills are done afterwards per slice by fill_regen_slice —
   * in parallel from update(), serially from regen_gpu_node. */
  void plan_regen_gpu_node(SpatialNode *gpu_node,
                           gpu::GPUManager *gpu,
                           util::Vector<mesh::AttrRef> &srcRefs);
  /* Pure fill of one planned slice (pos/nor/color + requested attrs).
   * `srcRefs` is the owner's resolved-source array from plan_regen_gpu_node
   * (nullptr on the legacy path). Writes only the slice's disjoint buffer
   * sub-ranges — safe under parallel_for; must not touch flags or GpuData. */
  void fill_regen_slice(SpatialNode *gpu_node, int sliceIdx, mesh::AttrRef *srcRefs);
  /* In-place slice rewrite; returns false if a full regen is required (caller
   * regens serially — this runs under parallel_for and must not mutate shared
   * GpuData). On success `outVertStart`/`outVertCount` (optional) receive the
   * slice's vert span so the caller can flag a partial buffer re-upload. */
  bool update_gpu_node_slice(SpatialNode *gpu_node,
                             SpatialNode *leaf,
                             gpu::GPUManager *gpu,
                             int *outVertStart = nullptr,
                             int *outVertCount = nullptr,
                             bool geomOnly = false);
  void collect_subtree_leaves(SpatialNode *node, util::Vector<SpatialNode *> &out);
  void fill_leaf_slice(SpatialNode *leaf,
                       math::float3 *pos,
                       math::float3 *nor,
                       math::float4 *col = nullptr);
  /* Generic per-corner gather of one requested attribute into `dst` (a packed
   * float buffer, req.elemSize floats per render vertex, already offset to the
   * leaf's slice). `src` resolved once by the caller against the mesh (nullptr
   * => default-fill by req.defaultKind). VERTEX/CORNER/FACE domains index by
   * vertex / corner / face respectively. */
  void fill_leaf_attr(SpatialNode *leaf,
                      const gpu::RequestedAttr &req,
                      mesh::AttrRef *src,
                      float *dst);
  void fill_leaf_slot_verts(SpatialNode *leaf, uint32_t *out);

  void
  add_face_intern(SpatialNode *node, int f, math::float3 &fcent, int claimTag = 0);

  /* Re-absorb both (leaf) children of `parent` back into `parent` and free them
   * (M7.6b merge). Preconditions checked by the caller. */
  void merge_node(SpatialNode *parent);

  /* True when `parent`'s two leaf children are so lopsided that the split level is
   * wasted and re-splitting at a fresh plane would help: one child near-empty
   * (count skew), or — past leaf_limit/2 but under leaf_limit — their AABBs
   * interpenetrate (deformation skew). Gated by a mean-split predictor so we never
   * merge into a split that would just reproduce the skew (thrash). */
  bool node_is_skewed(SpatialNode *parent);

  /* Collapse a whole subtree (one near-empty leaf child + a populated internal
   * child, which merge_node can't handle) back into `node` as a single leaf and
   * re-insert its geometry through add_face_intern, which re-splits at a fresh
   * plane. Caller gates on a bounded subtree size + the mean-split predictor. */
  void collapse_subtree(SpatialNode *node);

  /* True when `parent` (at least one non-leaf child) holds a subtree that now fits
   * under leaf_limit yet is split lopsidedly — a stale level worth collapsing.
   * Predictor-gated like node_is_skewed. */
  bool subtree_wants_collapse(SpatialNode *parent);

  /* Free `n` and its whole descendant subtree (post-order, via free_node). */
  void free_subtree(SpatialNode *n);

  /* O(1) node removal: swap the node out of `nodes` (fixing the swapped node's
   * index, which castRay resolves through nodes[]), drop it from node_idmap, and
   * delete it (its dtor frees data/gpu_data). Invalidates the leaf/gpu caches. */
  void free_node(SpatialNode *n);

  SpatialNode *alloc_node()
  {
    SpatialNode *node = alloc::New<SpatialNode>("Spatial Node");

    /* Serialized so applyDeferredNodeSplit can run split candidates in
     * parallel: `nodes`, `node_idmap`, and the id counter are the only state
     * the disjoint per-candidate splits share. Uncontended elsewhere. */
    std::lock_guard<std::mutex> guard(allocMutex_);

    node->id = node_idgen++;
    node->treeMesh = &treeMesh;
    node->index = nodes.size();
    nodes.append(node);

    /* The leaf set changed (a new leaf, or split children). Both the leaf and
     * gpu-node caches are now stale. */
    leafCacheDirty_ = true;
    gpuNodeCacheDirty_ = true;

    if (node->id >= node_idmap.size()) {
      node_idmap.resize(node->id + 1);
    }

    node_idmap[node->id] = node;

    return node;
  }

  SpatialNode *root;
  util::Vector<SpatialNode *> nodes;
  util::Vector<SpatialNode *> node_idmap;
  int node_idgen = 1;

  /* Persistent callback bundle returned by getSpatialCallbacks(). */
  mesh::MeshCallbacks spatialCallbacks_;

  /* Cached results of leaves()/gpu_nodes(), rebuilt lazily only when the node
   * set / GPU partition changes (see alloc_node, assign_gpu_nodes, rebuild).
   * Avoids rescanning all `nodes` on every call — leaves() in particular was
   * hit once per brush dab via the old filterNodes. */
  util::Vector<SpatialNode *> leafCache_;
  util::Vector<SpatialNode *> gpuNodeCache_;
  bool leafCacheDirty_ = true;
  bool gpuNodeCacheDirty_ = true;

  /* Leaves that crossed leaf_limit during incremental add_face_at placement and
   * await a batched split in applyDeferredNodeSplit() (M7.6). Brush queries on an
   * over-full leaf just iterate a few extra verts until the next update(). The
   * merge counterpart (under-full siblings folded back up) is mergeCandidates_
   * below, on a slower cadence. */
  util::Set<int> nodeSplitCandidates_;

  /* Parents of leaves that shrank since the last merge pass (M7.6b), drained by
   * applyDeferredMerge(). It runs every mergeCadence_-th update() rather than per
   * dab — merging is cheap but pointless to chase on every collapse, and the low
   * watermark (leaf_limit/2) plus this cadence keep the tree off a split/merge
   * thrash boundary. */
  util::Set<int> mergeCandidates_;
  int mergeCadence_ = 8;
  int updatesSinceMerge_ = 0;

  /* Guards nodes/node_idmap/node_idgen inside alloc_node — the only shared
   * state of the parallel deferred-split pass (see applyDeferredNodeSplit). */
  std::mutex allocMutex_;
};

} // namespace sculptcore::spatial
