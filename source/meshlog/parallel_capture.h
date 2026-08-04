#pragma once

/**
Parallel undo capture for the generated brush *Pre stage.

The serial capture walked every element of every filtered leaf on the calling
thread (needsData stamp check + row copy), which dominated non-dyntopo dab cost
at high vert counts. The row copy is the expensive half, so this helper splits
the walk from the copy and parallelizes only the copy:

  1. serial over nodes: claim each element that still needs capture this stroke,
     flipping its saved stamp (updateSaved) as it is claimed and appending it to
     one flat buffer, with each node's start offset recorded as it begins.
     Claiming through the gate itself is what makes the per-node runs disjoint —
     a node's unique_verts() is *not* unique across nodes (a leaf-boundary vertex
     is listed by every leaf touching it), and two threads racing one vertex's
     needsData/updateSaved pair would otherwise reserve two rows and fill one;
  2. serial: reserve every row in the element store at once — the claim buffer's
     length is the total (ChunkElemData::appendRows, the only shared mutation);
  3. parallel over nodes: copy each node's run of claimed elements into its own
     disjoint row range (cpyFrom). No predicate here — phase 1 already decided.

BOOL attribute rows write shared bitset words and are not parallel-safe; a
capture set containing one falls back to a serial fill.
*/

#include "attr_saver.h"
#include "meshlog.h"
#include "spatial/node.h"

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

  // Phase 1: claim through the gate — first node to reach an element owns it.
  // One flat buffer with per-node start offsets, not a Vector per node: the
  // claim is serial, so appends already land in node order, and a leaf-count
  // worth of heap allocations per dab per domain is pure overhead.
  util::Vector<int> owned;
  util::Vector<int> starts;
  starts.resize(n + 1);
  size_t cap = 0;
  for (int i = 0; i < n; i++) {
    cap += elemsOf(nodes[i]).size();
  }
  owned.ensure_capacity(cap);
  for (int i = 0; i < n; i++) {
    starts[i] = int(owned.size());
    for (int e : elemsOf(nodes[i])) {
      if (saver.needsData(e, sid, mask)) {
        saver.updateSaved(e, sid, mask);
        owned.append(e);
      }
    }
  }
  const int total = int(owned.size());
  starts[n] = total;

  /* Phase 2: one reservation for every new row. */
  if (total == 0) {
    return;
  }
  const int base = store.data.appendRows(total);

  auto fillNode = [&](int i) {
    int row = base + starts[i];
    for (int k = starts[i]; k < starts[i + 1]; k++) {
      store.data.cpyFrom(src, owned[k], row++);
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
