#pragma once

#include "brush_concepts.h"
#include "spatial/node.h"

namespace sculptcore::brush {

struct CommandExecutor;
struct BasicVertexIter {
  using sub_iterator = util::OrderedSet<int>::iterator;

  struct PtrHelper {
    float3 &co;
    float3 &no;
    float &mask;
    int v;
    int indexInNode = 0;

    CommandExecutor &ctx;

    PtrHelper(float3 &co_, float3 &no_, float &mask_, int v, CommandExecutor &ctx)
        : co(co_), no(no_), mask(mask_), v(v), ctx(ctx)
    {
    }
    PtrHelper(const PtrHelper &b)
        : co(b.co), no(b.no), mask(b.mask), v(b.v), indexInNode(b.indexInNode), ctx(b.ctx)
    {
    }
  };

  CommandExecutor &ctx;
  PtrHelper ptrs;
  spatial::SpatialNode &node;

  /** Note: do not ever create a vertex iter on an empty node with no vertices! */
  BasicVertexIter(spatial::SpatialNode &node, CommandExecutor &ctx)
      : node(node), iter(node.data->unique_verts.begin()),
        start_iter(node.data->unique_verts.begin()),
        end_iter(node.data->unique_verts.end()),
        ptrs(node.data->m->v.co[*node.data->unique_verts.begin()],
             node.data->m->v.no[*node.data->unique_verts.begin()],
             node.treeMesh->v.mask[*node.data->unique_verts.begin()],
             *node.data->unique_verts.begin(),
             ctx),
        ctx(ctx)
  {
  }

  BasicVertexIter(const BasicVertexIter &b)
      : node(b.node), iter(b.iter), start_iter(b.start_iter), end_iter(b.end_iter),
        _nodeIndex(b._nodeIndex), ptrs(b.ptrs), ctx(b.ctx)
  {
  }

  BasicVertexIter(spatial::SpatialNode &node, sub_iterator iter, CommandExecutor &ctx) : BasicVertexIter(node, ctx)
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
      new (&ptrs) PtrHelper(m->v.co[i], m->v.no[i], node.treeMesh->v.mask[i], i, ctx);
      ptrs.indexInNode = _nodeIndex++;
    }

    return *this;
  }

  BasicVertexIter begin()
  {
    return BasicVertexIter(node, start_iter, ctx);
  }

  BasicVertexIter end()
  {
    return BasicVertexIter(node, end_iter, ctx);
  }

private:
  sub_iterator iter;
  sub_iterator start_iter;
  sub_iterator end_iter;
  int _nodeIndex = 0;
};
}; // namespace sculptcore::brush
