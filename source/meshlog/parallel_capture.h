#pragma once

/**
Parallel undo capture for the generated brush *Pre stage.

The serial capture walked every element of every filtered leaf on the calling
thread (needsData stamp check + row append), which dominated non-dyntopo dab
cost at high vert counts. This helper runs the same capture in three phases:

  1. parallel over nodes: count the elements that still need capture this
     stroke (leaf element ownership is unique, so the per-element stamp reads
     touch disjoint elements across threads);
  2. serial: prefix-sum the counts and reserve every new row in the element
     store at once (ChunkElemData::appendRows — the only shared-state
     mutation);
  3. parallel over nodes: re-walk each node with the same predicate (the
     stamps are untouched between the phases, so the selection is identical),
     filling the node's disjoint row range (cpyFrom) and stamping its elements
     saved (updateSaved — disjoint again).

Flat count/base vectors only — no nested containers in the hot path. BOOL
attribute rows write shared bitset words and are not parallel-safe; a capture
set containing one falls back to a serial fill.
*/

#include "attr_saver.h"
#include "meshlog.h"
#include "spatial/node.h"

#include "litestl/util/set.h"
#include "litestl/util/task.h"
#include "litestl/util/vector.h"

#include <span>

namespace sculptcore::meshlog {

template <mesh::ElemType Domain, typename Saver>
void parallelCapture(LogChunkElems &store,
                     const mesh::AttrGroup &src,
                     std::span<spatial::SpatialNode *> nodes,
                     Saver &saver,
                     litestl::util::span<const mesh::AttrRef> refs,
                     int sid,
                     int mask)
{
  using namespace litestl;

  if (nodes.empty() || refs.size() == 0) {
    return;
  }

  // Register columns once, up front (idempotent) — the parallel fill must not
  // mutate the store's column set.
  bool hasBool = false;
  for (const mesh::AttrRef &ref : refs) {
    store.data.ensureAttr(src, ref);
    hasBool |= ref.type == mesh::AttrType::BOOL;
  }

  const int n = int(nodes.size());

  auto elemsOf = [](spatial::SpatialNode *node) -> util::OrderedSet<int> & {
    if constexpr (Domain == mesh::ElemType::VERTEX) {
      return node->unique_verts();
    } else {
      return node->unique_faces();
    }
  };

  /* Face ownership is unique per node, but a node's unique_verts() is not —
   * a vertex on a leaf boundary is listed by every leaf touching it. Claim
   * each element to exactly one node (first node in `nodes` wins) up front
   * so phases 1/3 below iterate disjoint per-node subsets; without this, a
   * shared element's needsData()/updateSaved() pair races across the
   * parallel_for threads below and produces either duplicate rows (both
   * threads see needsData() true) or unfilled rows (one thread's row is
   * reserved in phase 2's count but its phase-3 needsData() check then sees
   * the other thread already claimed it). */
  util::Vector<util::Vector<int>> owned;
  owned.resize(n);
  {
    util::Set<int> claimed;
    for (int i = 0; i < n; i++) {
      for (int e : elemsOf(nodes[i])) {
        if (claimed.add(e)) {
          owned[i].append(e);
        }
      }
    }
  }

  /* Phase 1: per-node counts of elements needing capture. */
  util::Vector<int> counts;
  counts.resize(n);
  task::parallel_for(util::IndexRange(n), [&](util::IndexRange range) {
    for (int i : range) {
      int c = 0;
      for (int e : owned[i]) {
        if (saver.needsData(e, sid, mask)) {
          c++;
        }
      }
      counts[i] = c;
    }
  });

  /* Phase 2: one reservation for every new row. */
  int total = 0;
  for (int i = 0; i < n; i++) {
    int c = counts[i];
    counts[i] = total; /* becomes the node's base offset */
    total += c;
  }
  if (total == 0) {
    return;
  }
  const int base = store.data.appendRows(total);

  auto fillNode = [&](int i) {
    int row = base + counts[i];
    for (int e : owned[i]) {
      if (saver.needsData(e, sid, mask)) {
        store.data.cpyFrom(src, e, row++);
        saver.updateSaved(e, sid, mask);
      }
    }
  };

  if (hasBool) {
    for (int i = 0; i < n; i++) {
      fillNode(i);
    }
    return;
  }

  /* Phase 3: disjoint row ranges, disjoint element stamps. */
  task::parallel_for(util::IndexRange(n), [&](util::IndexRange range) {
    for (int i : range) {
      fillNode(i);
    }
  });
}

} // namespace sculptcore::meshlog
