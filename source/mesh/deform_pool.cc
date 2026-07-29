#include "deform_pool.h"

#include <algorithm>
#include <cstring>

namespace sculptcore::mesh {

using litestl::hash::HashInt;
using litestl::util::span;
using litestl::util::Vector;

namespace {

/**
 * Reduce a run to its one canonical form: group-ascending, duplicate groups
 * summed, zero weights dropped. Interning is only correct because every
 * spelling of a weight set arrives here as the same bytes.
 */
void canonicalize(span<const DeformWeight> run, Vector<DeformWeight> &out)
{
  out.clear();
  for (const DeformWeight &w : run) {
    if (w.weight == 0.0f) {
      continue;
    }
    out.append(w);
  }
  if (out.size() < 2) {
    return;
  }

  // Comparator returns negative/zero/positive, not a bool.
  out.sort([](const DeformWeight &a, const DeformWeight &b) { return a.group - b.group; });

  int write = 0;
  for (int i = 1; i < int(out.size()); i++) {
    if (out[i].group == out[write].group) {
      out[write].weight += out[i].weight;
    } else {
      out[++write] = out[i];
    }
  }
  out.resize(write + 1);

  // Summing duplicates can cancel back to zero.
  write = 0;
  for (int i = 0; i < int(out.size()); i++) {
    if (out[i].weight != 0.0f) {
      out[write++] = out[i];
    }
  }
  out.resize(write);
}

/**
 * FNV-1a over the exact bits of a canonicalized run. Hashing float bits rather
 * than values is what makes hash-equal follow from compare-equal here: the
 * weights are copied, never recomputed, so bitwise and numeric equality agree.
 */
HashInt hashRun(span<const DeformWeight> run)
{
  HashInt h = 0xcbf29ce484222325ull;
  for (const DeformWeight &w : run) {
    uint32_t bits;
    std::memcpy(&bits, &w.weight, sizeof(bits));
    h = (h ^ HashInt(uint32_t(w.group))) * 0x100000001b3ull;
    h = (h ^ HashInt(bits)) * 0x100000001b3ull;
  }
  return h;
}

bool runsEqual(span<const DeformWeight> a, const DeformWeight *b, int b_count)
{
  if (int(a.size()) != b_count) {
    return false;
  }
  for (int i = 0; i < b_count; i++) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

} // namespace

DeformPool::DeformPool()
{
  initEmptySlot();
}

DeformPool::DeformPool(const DeformPool &b) : group_names(b.group_names)
{
  // Slot indices are preserved, so the clone inherits b's refcounts verbatim:
  // every reference b's columns hold, the copied columns hold too.
  for (int i = 0; i < SHARD_COUNT; i++) {
    std::lock_guard<std::mutex> lock(b.shards_[i].mutex);
    copyShard(shards_[i], b.shards_[i]);
  }
}

DeformPool &DeformPool::operator=(const DeformPool &b)
{
  if (this == &b) {
    return *this;
  }
  group_names = b.group_names;
  for (int i = 0; i < SHARD_COUNT; i++) {
    std::scoped_lock lock(shards_[i].mutex, b.shards_[i].mutex);
    copyShard(shards_[i], b.shards_[i]);
  }
  return *this;
}

void DeformPool::copyShard(Shard &dst, const Shard &src)
{
  dst.arena = src.arena;
  dst.slots = src.slots;
  dst.hash_head = src.hash_head;
  dst.free_slots = src.free_slots;
  dst.live_count = src.live_count;
}

void DeformPool::initEmptySlot()
{
  // Slot 0 is shard 0, local 0, and immortal, so a zero-filled column element
  // is a valid empty run that nobody had to intern.
  Shard &shard = shards_[0];
  Slot empty;
  empty.refs = 1;
  shard.slots.append(empty);
  shard.live_count = 1;
}

WeightSlot DeformPool::intern(span<const DeformWeight> run)
{
  Vector<DeformWeight> canon;
  canonicalize(run, canon);
  if (canon.size() == 0) {
    return WeightSlot(0);
  }

  span<const DeformWeight> key(canon.data(), canon.size());
  const HashInt h = hashRun(key);
  const int shard_i = int(h & HashInt(SHARD_MASK));
  Shard &shard = shards_[shard_i];

  std::lock_guard<std::mutex> lock(shard.mutex);

  int *head = shard.hash_head.lookup_ptr(h);
  for (int local = head ? *head : -1; local != -1;) {
    Slot &slot = shard.slots[local];
    if (runsEqual(key, &shard.arena[slot.start], slot.count)) {
      if (slot.refs == 0) {
        // Still interned, just pending reclamation — resurrect it in place.
        // This is why release-to-zero and intern share one lock.
        unlistFreeSlot(shard, local);
        shard.live_count++;
      }
      slot.refs++;
      return WeightSlot(slotIndex(shard_i, local));
    }
    local = slot.next_hash;
  }

  Slot slot;
  slot.start = int(shard.arena.size());
  slot.count = int(key.size());
  slot.refs = 1;
  slot.hash = h;
  slot.next_hash = head ? *head : -1;
  for (const DeformWeight &w : key) {
    shard.arena.append(w);
  }

  const int local = int(shard.slots.size());
  shard.slots.append(slot);
  shard.live_count++;
  if (head) {
    *head = local;
  } else {
    shard.hash_head.add(h, local);
  }

  return WeightSlot(slotIndex(shard_i, local));
}

void DeformPool::unlistFreeSlot(Shard &shard, int local)
{
  for (int i = 0; i < int(shard.free_slots.size()); i++) {
    if (shard.free_slots[i] == local) {
      shard.free_slots[i] = shard.free_slots[shard.free_slots.size() - 1];
      shard.free_slots.resize(shard.free_slots.size() - 1);
      return;
    }
  }
}

void DeformPool::retain(WeightSlot slot)
{
  if (slot.index == 0) {
    return;
  }
  Shard &shard = shards_[shardOf(slot.index)];
  std::lock_guard<std::mutex> lock(shard.mutex);
  shard.slots[localOf(slot.index)].refs++;
}

void DeformPool::release(WeightSlot slot)
{
  if (slot.index == 0) {
    return;
  }
  Shard &shard = shards_[shardOf(slot.index)];
  const int local = localOf(slot.index);

  std::lock_guard<std::mutex> lock(shard.mutex);
  Slot &s = shard.slots[local];
  if (s.refs == 0) {
    return;
  }
  if (--s.refs == 0) {
    // Stays interned and addressable until sweep(); intern() may resurrect it.
    shard.free_slots.append(local);
    shard.live_count--;
  }
}

void DeformPool::reassign(WeightSlot &dst, WeightSlot src)
{
  if (dst.index == src.index) {
    return;
  }
  retain(src);
  release(dst);
  dst = src;
}

int DeformPool::runSize(WeightSlot slot) const
{
  if (slot.index == 0) {
    return 0;
  }
  const Shard &shard = shards_[shardOf(slot.index)];
  std::lock_guard<std::mutex> lock(shard.mutex);
  return shard.slots[localOf(slot.index)].count;
}

int DeformPool::copyRun(WeightSlot slot, DeformWeight *out, int max) const
{
  if (slot.index == 0) {
    return 0;
  }
  const Shard &shard = shards_[shardOf(slot.index)];
  std::lock_guard<std::mutex> lock(shard.mutex);

  const Slot &s = shard.slots[localOf(slot.index)];
  const int n = std::min(s.count, max);
  for (int i = 0; i < n; i++) {
    out[i] = shard.arena[s.start + i];
  }
  return s.count;
}

float DeformPool::weight(WeightSlot slot, int group) const
{
  if (slot.index == 0) {
    return 0.0f;
  }
  const Shard &shard = shards_[shardOf(slot.index)];
  std::lock_guard<std::mutex> lock(shard.mutex);

  const Slot &s = shard.slots[localOf(slot.index)];
  for (int i = 0; i < s.count; i++) {
    const DeformWeight &w = shard.arena[s.start + i];
    if (w.group == group) {
      return w.weight;
    }
    if (w.group > group) {
      break; // runs are group-ascending
    }
  }
  return 0.0f;
}

void DeformPool::sweep()
{
  for (int shard_i = 0; shard_i < SHARD_COUNT; shard_i++) {
    Shard &shard = shards_[shard_i];
    std::lock_guard<std::mutex> lock(shard.mutex);

    if (shard.free_slots.size() == 0) {
      continue;
    }

    // Local slot indices stay put — columns name them. Only arena ranges move,
    // and a reclaimed slot becomes an empty, reusable table entry.
    Vector<DeformWeight> arena;
    for (int local = 0; local < int(shard.slots.size()); local++) {
      Slot &s = shard.slots[local];
      if (s.refs == 0) {
        s.start = 0;
        s.count = 0;
        s.hash = 0;
        s.next_hash = -1;
        continue;
      }
      const int start = int(arena.size());
      for (int i = 0; i < s.count; i++) {
        arena.append(shard.arena[s.start + i]);
      }
      s.start = start;
    }
    shard.arena = std::move(arena);

    // Rebuilding the chains from the survivors is cheaper and harder to get
    // wrong than unlinking each dead slot from its bucket.
    shard.hash_head.clear();
    for (int local = int(shard.slots.size()) - 1; local >= 0; local--) {
      Slot &s = shard.slots[local];
      if (s.refs == 0 || s.count == 0) {
        continue;
      }
      int *head = shard.hash_head.lookup_ptr(s.hash);
      s.next_hash = head ? *head : -1;
      if (head) {
        *head = local;
      } else {
        shard.hash_head.add(s.hash, local);
      }
    }

    shard.free_slots.clear();
  }
}

size_t DeformPool::liveSlotCount() const
{
  size_t total = 0;
  for (int i = 0; i < SHARD_COUNT; i++) {
    std::lock_guard<std::mutex> lock(shards_[i].mutex);
    total += size_t(shards_[i].live_count);
  }
  return total;
}

size_t DeformPool::byteSize() const
{
  size_t total = 0;
  for (int i = 0; i < SHARD_COUNT; i++) {
    std::lock_guard<std::mutex> lock(shards_[i].mutex);
    total += shards_[i].arena.size() * sizeof(DeformWeight);
    total += shards_[i].slots.size() * sizeof(Slot);
  }
  return total;
}

int DeformPool::auditRefcounts(span<const WeightSlot> roots) const
{
  Vector<uint32_t> counted[SHARD_COUNT];

  for (int i = 0; i < SHARD_COUNT; i++) {
    std::lock_guard<std::mutex> lock(shards_[i].mutex);
    counted[i].resize(shards_[i].slots.size());
    for (int j = 0; j < int(counted[i].size()); j++) {
      counted[i][j] = 0;
    }
  }

  for (const WeightSlot &slot : roots) {
    const int shard_i = shardOf(slot.index);
    const int local = localOf(slot.index);
    if (local < int(counted[shard_i].size())) {
      counted[shard_i][local]++;
    }
  }

  int mismatches = 0;
  for (int i = 0; i < SHARD_COUNT; i++) {
    std::lock_guard<std::mutex> lock(shards_[i].mutex);
    for (int j = 0; j < int(shards_[i].slots.size()); j++) {
      // Slot 0 carries one immortal reference on top of whatever roots hold.
      const uint32_t expect = counted[i][j] + ((i == 0 && j == 0) ? 1u : 0u);
      if (shards_[i].slots[j].refs != expect) {
        mismatches++;
      }
    }
  }
  return mismatches;
}

} // namespace sculptcore::mesh
