#pragma once

#include "litestl/util/vector.h"
#include "mesh/mesh.h"
#include "meshlog_types.h"
#include "spatial/spatial.h"
#include <utility>

namespace sculptcore::meshlog {
using litestl::util::Vector;

/**
 * Reorder undo/redo chunk — records the five element permutations
 * (map[old] = new). A reorder is a pure bijection, so undo replays the inverse
 * permutation and redo replays the forward one, both via
 * SpatialTree::applyReorderIncremental: it relabels the existing node set in
 * place (no rebuild), so the node set + ids that existed before the reorder are
 * reproduced EXACTLY across undo/redo — simple chunks recorded in earlier steps
 * still resolve their node ids after undoing back across this chunk. (The
 * forward apply that recorded this chunk must likewise be incremental.)
 */
struct LogChunkReorder : public LogChunk {
  Vector<int> vmap, emap, cmap, lmap, fmap;
  /* Scoped (partial) compaction: the per-domain moved (live) slot sets. A scoped
   * reorder is a closed permutation over these slots, so the set is invariant
   * under the permutation AND its inverse — undo/redo replay scoped with the same
   * sets. Empty ⇒ full reorder (whole-mesh map; replay via the full path). */
  Vector<int> mv, me, mc, ml, mf;
  /* Scoped chunk: the target slots of the moved sets (vval[i] = map[mv[i]]). The
   * full map is NOT stored (it is mostly identity) — reconstructed transiently on
   * undo/redo. This makes the chunk O(moved) instead of O(capacity). */
  Vector<int> vval, eval, cval, lval, fval;
  bool scoped = false;

  LogChunkReorder() : LogChunk(LogChunkTypes::Reorder)
  {
  }

  LogChunkReorder(Vector<int> vmap_,
                  Vector<int> emap_,
                  Vector<int> cmap_,
                  Vector<int> lmap_,
                  Vector<int> fmap_)
      : LogChunk(LogChunkTypes::Reorder), vmap(std::move(vmap_)), emap(std::move(emap_)),
        cmap(std::move(cmap_)), lmap(std::move(lmap_)), fmap(std::move(fmap_))
  {
  }

  void undo(mesh::Mesh *m, spatial::SpatialTree *tree) override
  {
    /* Reconstruct the full forward bijection (from sparse moves for a scoped chunk,
     * or pad the stored full map otherwise), then replay the INVERSE via the full
     * applyReorderIncremental. Undo/redo are rare, so the full O(mesh) replay is
     * fine; the scoped forward already paid only O(region). The scoped forward
     * leaves exactly the state a full apply would, so the full inverse reverts it. */
    Vector<int> v, e, c, l, f;
    forwardMaps(m, v, e, c, l, f);

    Vector<int> iv, ie, ic, il, iff;
    invert(v, iv);
    invert(e, ie);
    invert(c, ic);
    invert(l, il);
    invert(f, iff);
    tree->applyReorderIncremental(iv, ie, ic, il, iff);
  }

  void redo(mesh::Mesh *m, spatial::SpatialTree *tree) override
  {
    Vector<int> v, e, c, l, f;
    forwardMaps(m, v, e, c, l, f);
    tree->applyReorderIncremental(v, e, c, l, f);
  }

  double memSize() override
  {
    double n =
        double(vmap.size() + emap.size() + cmap.size() + lmap.size() + fmap.size());
    n += double(mv.size() + me.size() + mc.size() + ml.size() + mf.size());
    n += double(vval.size() + eval.size() + cval.size() + lval.size() + fval.size());
    return double(sizeof(*this)) + n * sizeof(int);
  }

private:
  /* Materialize the full forward bijection per domain at the current capacity —
   * reconstructed from the sparse moves for a scoped chunk, or padded from the
   * stored full map otherwise. */
  void forwardMaps(mesh::Mesh *m,
                   Vector<int> &v,
                   Vector<int> &e,
                   Vector<int> &c,
                   Vector<int> &l,
                   Vector<int> &f)
  {
    if (scoped) {
      reconstruct(mv, vval, int(m->v.capacity()), v);
      reconstruct(me, eval, int(m->e.capacity()), e);
      reconstruct(mc, cval, int(m->c.capacity()), c);
      reconstruct(ml, lval, int(m->l.capacity()), l);
      reconstruct(mf, fval, int(m->f.capacity()), f);
    } else {
      padToCapacity(vmap, int(m->v.capacity()), v);
      padToCapacity(emap, int(m->e.capacity()), e);
      padToCapacity(cmap, int(m->c.capacity()), c);
      padToCapacity(lmap, int(m->l.capacity()), l);
      padToCapacity(fmap, int(m->f.capacity()), f);
    }
  }

  /* Build a full bijection at @p cap from the sparse move list: identity, then
   * out[from[i]] = to[i]. (from,to)=(mv,vval) gives the forward map. Slots created
   * after the reorder are free now and map to themselves, so identity-by-default
   * is the correct extension. */
  static void
  reconstruct(const Vector<int> &from, const Vector<int> &to, int cap, Vector<int> &out)
  {
    out.resize(cap);
    for (int i = 0; i < cap; i++) {
      out[i] = i;
    }
    for (int i = 0; i < int(from.size()); i++) {
      out[from[i]] = to[i];
    }
  }

  /* The map was recorded at the capacity that existed when the reorder ran. Later
   * steps grow the element arrays and undo doesn't shrink them, so by the time
   * this chunk is replayed the domain capacity can EXCEED the map. The reorder
   * only permuted [0, recorded-capacity); the slots created afterward are free now
   * (their creators are already undone) and map to themselves. Extend with
   * identity so the permutation is a full bijection over the current capacity. */
  static void padToCapacity(const Vector<int> &map, int cap, Vector<int> &out)
  {
    int n = int(map.size());
    out.resize(cap);
    for (int i = 0; i < n && i < cap; i++) {
      out[i] = map[i];
    }
    for (int i = n; i < cap; i++) {
      out[i] = i;
    }
  }

  static void invert(const Vector<int> &map, Vector<int> &out)
  {
    out.resize(map.size());
    for (int i = 0; i < int(map.size()); i++) {
      out[map[i]] = i;
    }
  }
};

} // namespace sculptcore::meshlog
