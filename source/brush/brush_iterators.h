#pragma once

#include "accum_mode.h"
#include "brush_concepts.h"
#include "spatial/node.h"

namespace sculptcore::brush {

struct CommandExecutor;

// Per-vertex iteration for `vertex` stage kernels, parameterized by the
// AccumMode policy (see accum_mode.h). The bundle's `co` is a CoProxy<AccMode>:
// under AccumLive it is a thin reference to the live position; under AccumOrig
// it reads each vert's base position until the first write. The base cache
// (`dispVec`/`dispGen`, or legacy `origCo`/`origGen`, plus `strokeGen`) is
// threaded in from the executor; all are null/0 under AccumLive.
template <class AccMode> struct BasicVertexIter {
  using sub_iterator = util::OrderedSet<int>::iterator;

  struct PtrHelper {
    CoProxy<AccMode> co;
    float3 &no;
    float &mask;
    int v;
    int indexInNode = 0;

    CommandExecutor &ctx;

    PtrHelper(float3 &co_, float3 base_, mesh::AttrData<float3> *disp_, float3 &no_,
              float &mask_, int v, CommandExecutor &ctx)
        : co{co_, base_, &ctx, disp_, v}, no(no_), mask(mask_), v(v), ctx(ctx)
    {
    }
    PtrHelper(const PtrHelper &b)
        : co(b.co), no(b.no), mask(b.mask), v(b.v), indexInNode(b.indexInNode), ctx(b.ctx)
    {
    }
  };

  CommandExecutor &ctx;
  mesh::AttrData<float3> *origCo;
  mesh::AttrData<int> *origGen;
  mesh::AttrData<float3> *dispVec;
  mesh::AttrData<int> *dispGen;
  uint32_t strokeGen;
  PtrHelper ptrs;
  spatial::SpatialNode &node;

  // The from-base position for vert v, by value because the displacement path
  // derives it: `co - disp` when stamped this stroke, else the legacy absolute
  // snapshot, else the live position. Compiles to just the live position under
  // AccumLive.
  float3 baseFor(mesh::Mesh *m, int v)
  {
    if constexpr (AccMode::reads_base) {
      if (dispVec) {
        if (dispGen->safe_get(v) == int(strokeGen)) {
          return m->v.co[v] - dispVec->safe_get(v);
        }
        return m->v.co[v];
      }
      if (origGen && origGen->safe_get(v) == int(strokeGen)) {
        return (*origCo)[v];
      }
    }
    return m->v.co[v];
  }

  // The disp attr the proxy accumulates into, or null on the legacy path. Only
  // a from-base mode records displacement; AccumLive writes absolute positions.
  mesh::AttrData<float3> *dispFor()
  {
    if constexpr (AccMode::reads_base) {
      return dispVec;
    }
    return nullptr;
  }

  /** Note: do not ever create a vertex iter on an empty node with no vertices! */
  BasicVertexIter(spatial::SpatialNode &node, CommandExecutor &ctx,
                  mesh::AttrData<float3> *origCo, mesh::AttrData<int> *origGen,
                  mesh::AttrData<float3> *dispVec, mesh::AttrData<int> *dispGen,
                  uint32_t strokeGen)
      : ctx(ctx), origCo(origCo), origGen(origGen), dispVec(dispVec), dispGen(dispGen),
        strokeGen(strokeGen),
        ptrs(node.data->m->v.co[*node.data->unique_verts.begin()],
             baseFor(node.data->m, *node.data->unique_verts.begin()), dispFor(),
             node.data->m->v.no[*node.data->unique_verts.begin()],
             node.treeMesh->v.mask[*node.data->unique_verts.begin()],
             *node.data->unique_verts.begin(), ctx),
        node(node), iter(node.data->unique_verts.begin()),
        start_iter(node.data->unique_verts.begin()),
        end_iter(node.data->unique_verts.end())
  {
  }

  BasicVertexIter(const BasicVertexIter &b)
      : ctx(b.ctx), origCo(b.origCo), origGen(b.origGen), dispVec(b.dispVec),
        dispGen(b.dispGen), strokeGen(b.strokeGen), ptrs(b.ptrs), node(b.node),
        iter(b.iter), start_iter(b.start_iter), end_iter(b.end_iter),
        _nodeIndex(b._nodeIndex)
  {
  }

  BasicVertexIter(spatial::SpatialNode &node, sub_iterator iter, CommandExecutor &ctx,
                  mesh::AttrData<float3> *origCo, mesh::AttrData<int> *origGen,
                  mesh::AttrData<float3> *dispVec, mesh::AttrData<int> *dispGen,
                  uint32_t strokeGen)
      : BasicVertexIter(node, ctx, origCo, origGen, dispVec, dispGen, strokeGen)
  {
    this->iter = iter;
  }

  bool operator==(const BasicVertexIter &b)
  {
    return iter == b.iter;
  }

  bool operator!=(const BasicVertexIter &b)
  {
    return iter != b.iter;
  }

  PtrHelper &operator*()
  {
    return ptrs;
  }

  BasicVertexIter &operator++()
  {
    ++iter;

    if (iter != end_iter) {
      auto *m = node.data->m;
      int i = *iter;

      ptrs.~PtrHelper();
      new (&ptrs) PtrHelper(m->v.co[i], baseFor(m, i), dispFor(), m->v.no[i],
                            node.treeMesh->v.mask[i], i, ctx);
      ptrs.indexInNode = _nodeIndex++;
    }

    return *this;
  }

  BasicVertexIter begin()
  {
    return BasicVertexIter(node, start_iter, ctx, origCo, origGen, dispVec, dispGen,
                           strokeGen);
  }

  BasicVertexIter end()
  {
    return BasicVertexIter(node, end_iter, ctx, origCo, origGen, dispVec, dispGen,
                           strokeGen);
  }

private:
  sub_iterator iter;
  sub_iterator start_iter;
  sub_iterator end_iter;
  int _nodeIndex = 0;
};

// Per-face iteration for `face` stage kernels. Yields one bundle per face the
// node owns (unique_faces), exposing the face index `f`, the area-agnostic
// centroid `center` (used for the brush falloff test), and the face normal
// `no`. Walking the face loop needs live topo links, so face-stage brushes run
// with topology thawed (see CommandExecutor::brushNeedsLiveLinks).
struct BasicFaceIter {
  using sub_iterator = util::OrderedSet<int>::iterator;

  static float3 computeCentroid(mesh::Mesh *m, int f)
  {
    float3 c(0.0f, 0.0f, 0.0f);
    int n = 0;
    int l = m->f.l[f];
    while (l != -1) {
      int start = m->l.c[l];
      int corner = start;
      if (corner != -1) {
        do {
          c += m->v.co[m->c.v[corner]];
          n++;
          corner = m->c.next[corner];
        } while (corner != start && corner != -1);
      }
      l = m->l.next[l];
    }
    if (n > 0) c /= float(n);
    return c;
  }

  struct FacePtr {
    int f;
    float3 center;
    float3 &no;
    int indexInNode = 0;
    CommandExecutor &ctx;

    FacePtr(int f_, float3 center_, float3 &no_, CommandExecutor &ctx_)
        : f(f_), center(center_), no(no_), ctx(ctx_)
    {
    }
    FacePtr(const FacePtr &b)
        : f(b.f), center(b.center), no(b.no), indexInNode(b.indexInNode), ctx(b.ctx)
    {
    }
  };

  CommandExecutor &ctx;
  FacePtr ptrs;
  spatial::SpatialNode &node;

  /** Note: do not create a face iter on a node with no faces. */
  BasicFaceIter(spatial::SpatialNode &node, CommandExecutor &ctx)
      : node(node), iter(node.data->unique_faces.begin()),
        start_iter(node.data->unique_faces.begin()),
        end_iter(node.data->unique_faces.end()),
        ptrs(*node.data->unique_faces.begin(),
             computeCentroid(node.data->m, *node.data->unique_faces.begin()),
             node.data->m->f.no[*node.data->unique_faces.begin()],
             ctx),
        ctx(ctx)
  {
  }

  BasicFaceIter(const BasicFaceIter &b)
      : node(b.node), iter(b.iter), start_iter(b.start_iter), end_iter(b.end_iter),
        _nodeIndex(b._nodeIndex), ptrs(b.ptrs), ctx(b.ctx)
  {
  }

  BasicFaceIter(spatial::SpatialNode &node, sub_iterator iter, CommandExecutor &ctx)
      : BasicFaceIter(node, ctx)
  {
    this->iter = iter;
  }

  bool operator==(const BasicFaceIter &b) { return iter == b.iter; }
  bool operator!=(const BasicFaceIter &b) { return iter != b.iter; }
  FacePtr &operator*() { return ptrs; }

  BasicFaceIter &operator++()
  {
    ++iter;
    if (iter != end_iter) {
      auto *m = node.data->m;
      int i = *iter;
      ptrs.~FacePtr();
      new (&ptrs) FacePtr(i, computeCentroid(m, i), m->f.no[i], ctx);
      ptrs.indexInNode = _nodeIndex++;
    }
    return *this;
  }

  BasicFaceIter begin() { return BasicFaceIter(node, start_iter, ctx); }
  BasicFaceIter end() { return BasicFaceIter(node, end_iter, ctx); }

private:
  sub_iterator iter;
  sub_iterator start_iter;
  sub_iterator end_iter;
  int _nodeIndex = 0;
};
}; // namespace sculptcore::brush
