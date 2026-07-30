#pragma once

/**
 * Sparse deform-weight storage: the side table an AttrType::WEIGHTS column
 * indexes into.
 *
 * A vertex's weights are a short, group-sorted run of (group, weight) pairs —
 * Blender's MDeformVert, minus the per-vertex heap allocation. Runs are
 * **immutable and interned**, so identical weight sets across a region share
 * one slot, and are **refcounted**, so a slot stays alive exactly as long as
 * some attribute column (mesh or meshlog) names it.
 *
 * Immutability is what makes the rest of the engine work unchanged: a column
 * element is a plain 32-bit WeightSlot, so the meshlog's swapWith exchanges
 * values without touching a refcount, and mesh_serialize writes the column as
 * raw bytes like any other. Only duplication (MeshLog::cpyFrom) and overwrite
 * need to retain/release.
 *
 * Concurrency: sharded by slot index, one mutex per shard, everything under it.
 * Dyntopo is single-threaded per dab today but is written for a parallel caller
 * (dyntopo.h), and the meshlog's parallel_capture already fills rows from
 * several threads. The obvious next step if a shard lock ever shows up in a
 * profile is chunked (never-reallocating) storage plus atomic refcounts, which
 * would let retain and copyRun run lock-free; the API here does not change.
 */

#include "attribute_enums.h"

#include "litestl/util/hash.h"
#include "litestl/util/map.h"
#include "litestl/util/span.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace sculptcore::mesh {

using litestl::util::string;

/** One influence: a vertex group index and its weight. Mirrors MDeformWeight. */
struct DeformWeight {
  int group = 0;
  float weight = 0.0f;

  bool operator==(const DeformWeight &b) const
  {
    return group == b.group && weight == b.weight;
  }
  bool operator!=(const DeformWeight &b) const
  {
    return !(*this == b);
  }
};

/** Influences a merge is allowed to carry forward. Storage is not capped — this
 * bounds what the interpolator produces, so an unlucky chain of splits across a
 * many-group seam cannot grow a vertex's run without limit. */
static constexpr int DEFORM_MAX_INFLUENCES = 32;

/**
 * The mesh-owned pool. Slot 0 is the empty run and is immortal, so a
 * default-constructed or zero-filled WeightSlot is already valid.
 *
 * Lifetime is a **user count**, not plain mesh ownership: a meshlog chunk's rows
 * hold slot indices, and those indices are meaningless without the pool, but the
 * mesh is routinely destroyed before the log that logged it (Scene::~Scene frees
 * `mesh` in its body, and `meshLog` is a member, so it destructs afterwards).
 * Every holder takes a user through DeformPoolUser; the last one out deletes.
 */
struct DeformPool {
  // Slot index layout: (local << SHARD_BITS) | shard. A slot never migrates
  // between shards, so its owning mutex follows from the index with no lookup.
  static constexpr int SHARD_BITS = 6;
  static constexpr int SHARD_COUNT = 1 << SHARD_BITS;
  static constexpr int SHARD_MASK = SHARD_COUNT - 1;

  /** Vertex group names, indexed by DeformWeight::group. Ordered to match
   * Blender's Mesh::vertex_group_names so the mapping is identity while the two
   * lists agree; the addon reconciles by name across an enter/exit. */
  litestl::util::Vector<string> group_names;

  DeformPool();
  DeformPool(const DeformPool &b);
  DeformPool &operator=(const DeformPool &b);
  DeformPool(DeformPool &&) = delete;

  /** Users, not references-to-slots: see the struct comment. A copy starts at
   * one user of its own — the user count describes who points at *this* object,
   * which is not something a clone inherits. Prefer DeformPoolUser over calling
   * these directly. */
  void addUser();
  void removeUser();

  /** Intern `run` and return a slot the caller owns one reference to. `run` need
   * not be sorted or deduplicated; it is canonicalized (group-ascending, zero
   * and duplicate entries dropped) before hashing, so two equal weight sets
   * always land on the same slot. An empty result is slot 0. */
  WeightSlot intern(litestl::util::span<const DeformWeight> run);

  void retain(WeightSlot slot);
  void release(WeightSlot slot);

  /** Replace the value at a column element: release the old, retain the new.
   * The one call every writer of a WEIGHTS column should go through. */
  void reassign(WeightSlot &dst, WeightSlot src);

  int runSize(WeightSlot slot) const;

  /** Copy up to `max` influences of `slot` into `out`, returning how many the
   * slot has (which may exceed `max` — nothing is written past it).
   *
   * Deliberately a copy rather than a span: interning may reallocate a shard's
   * arena, so no caller may hold a pointer into it across an intern(). */
  int copyRun(WeightSlot slot, DeformWeight *out, int max) const;

  float weight(WeightSlot slot, int group) const;

  /** Reclaim slots that reached zero references and compact the arenas.
   * A safepoint operation — call between dabs, not during one. Slots named by a
   * live undo step have a reference and are never reclaimed. */
  void sweep();

  size_t liveSlotCount() const;
  size_t byteSize() const;

  /** Debug: recompute every refcount from `roots` (one entry per reference held
   * by any column, mesh or meshlog) and report how many slots disagree. Zero is
   * the only correct answer; anything else means a write bypassed the funnel. */
  int auditRefcounts(litestl::util::span<const WeightSlot> roots) const;

private:
  struct Slot {
    int start = 0; // into the shard's arena
    int count = 0;
    uint32_t refs = 0;
    litestl::hash::HashInt hash = 0;
    int next_hash = -1; // chain of local slots sharing a hash bucket
  };

  struct Shard {
    mutable std::mutex mutex;
    litestl::util::Vector<DeformWeight> arena;
    litestl::util::Vector<Slot> slots;
    litestl::util::Map<litestl::hash::HashInt, int> hash_head;
    litestl::util::Vector<int> free_slots;
    int live_count = 0;
  };

  Shard shards_[SHARD_COUNT];
  std::atomic<int> users_{1};

  static int shardOf(int index)
  {
    return index & SHARD_MASK;
  }
  static int localOf(int index)
  {
    return index >> SHARD_BITS;
  }
  static int slotIndex(int shard, int local)
  {
    return (local << SHARD_BITS) | shard;
  }

  static void copyShard(Shard &dst, const Shard &src);
  static void unlistFreeSlot(Shard &shard, int local);

  void initEmptySlot();
};

/**
 * A strong reference to a DeformPool: the mesh that created it, and every
 * meshlog store whose rows still name slots in it, hold one.
 *
 * Only ever point this at a pool created by MeshBase::deformPool() — the last
 * user deletes through litestl::alloc, so a stack-constructed pool must not
 * acquire one.
 */
struct DeformPoolUser {
  DeformPool *ptr = nullptr;

  DeformPoolUser() = default;
  DeformPoolUser(const DeformPoolUser &) = delete;
  DeformPoolUser &operator=(const DeformPoolUser &) = delete;
  ~DeformPoolUser()
  {
    reset(nullptr);
  }

  /** Idempotent: re-pointing at the pool already held does nothing. */
  void reset(DeformPool *pool);
};

} // namespace sculptcore::mesh
