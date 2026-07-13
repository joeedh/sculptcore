#pragma once

/** Per-vertex incident-edge slab storage (dyntopo disk-bandwidth plan M4,
 * design: documentation/plans/2026-07-12-1520-dyntopo-disk-slab-design.md).
 * A vertex's incident edges live in one power-of-two block of a pooled int32
 * arena; a slot is `(offset, meta)` with `meta = (class << 28) | count`.
 * Entries are opaque int32 values owned by the caller (the mesh packs
 * `diskPack(edge, side)`), kept in disk order: insert appends at the tail,
 * remove shifts the tail down one — reproducing exactly the e_of_v sequence
 * of the linked disk cycles this replaces. count == 0 ⇔ no block owned. */

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <cstring>

namespace sculptcore::mesh {

struct DiskSlabArena {
  using int2 = litestl::math::int2;

  static constexpr int kMinClass = 2;  /* smallest block = 4 entries */
  static constexpr int kMaxClass = 27; /* meta packs class in the top 4 bits */

  litestl::util::Vector<int> pool;
  int free_head[kMaxClass + 1];

  DiskSlabArena()
  {
    for (int i = 0; i <= kMaxClass; i++) {
      free_head[i] = -1;
    }
  }

  static int metaCount(int meta)
  {
    return meta & 0x0fffffff;
  }
  static int metaClass(int meta)
  {
    return int(uint32_t(meta) >> 28);
  }
  static int metaMake(int cls, int count)
  {
    return (cls << 28) | count;
  }
  static int classCap(int cls)
  {
    return 1 << cls;
  }

  void clear()
  {
    pool.clear();
    for (int i = 0; i <= kMaxClass; i++) {
      free_head[i] = -1;
    }
  }

  int allocBlock(int cls)
  {
    if (free_head[cls] != -1) {
      int off = free_head[cls];
      free_head[cls] = pool[off];
      return off;
    }
    int off = int(pool.size());
    pool.resize(off + classCap(cls));
    return off;
  }

  void freeBlock(int off, int cls)
  {
    pool[off] = free_head[cls];
    free_head[cls] = off;
  }

  /* litestl Vector::data() is not const-qualified, so neither is this. */
  const int *span(const int2 &slot)
  {
    return pool.data() + slot[0];
  }
  static int count(const int2 &slot)
  {
    return metaCount(slot[1]);
  }

  /* Append `entry` at the slab tail (== insert at the old cycle tail). */
  void insert(int2 &slot, int entry)
  {
    int cnt = metaCount(slot[1]);
    if (cnt == 0) {
      int off = allocBlock(kMinClass);
      pool[off] = entry;
      slot[0] = off;
      slot[1] = metaMake(kMinClass, 1);
      return;
    }
    int cls = metaClass(slot[1]);
    if (cnt == classCap(cls)) {
      int noff = allocBlock(cls + 1);
      std::memcpy(pool.data() + noff, pool.data() + slot[0], size_t(cnt) * sizeof(int));
      freeBlock(slot[0], cls);
      slot[0] = noff;
      cls++;
    }
    pool[slot[0] + cnt] = entry;
    slot[1] = metaMake(cls, cnt + 1);
  }

  /* Remove the entry equal to `entry`, preserving the order of the rest
   * (== cycle unlink; removing slot 0 promotes slot 1 to head). Returns
   * false if the entry is not present. */
  bool remove(int2 &slot, int entry)
  {
    int cnt = metaCount(slot[1]), cls = metaClass(slot[1]);
    int *p = pool.data() + slot[0];
    for (int i = 0; i < cnt; i++) {
      if (p[i] != entry) {
        continue;
      }
      if (cnt == 1) {
        freeBlock(slot[0], cls);
        slot[0] = 0;
        slot[1] = 0;
      } else {
        std::memmove(p + i, p + i + 1, size_t(cnt - 1 - i) * sizeof(int));
        slot[1] = metaMake(cls, cnt - 1);
      }
      return true;
    }
    return false;
  }

  /* Free a vertex's block wholesale (vertex death). */
  void release(int2 &slot)
  {
    int cnt = metaCount(slot[1]);
    if (cnt > 0) {
      freeBlock(slot[0], metaClass(slot[1]));
    }
    slot[0] = 0;
    slot[1] = 0;
  }
};

} // namespace sculptcore::mesh
