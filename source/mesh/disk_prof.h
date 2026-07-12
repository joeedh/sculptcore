#pragma once

// CLAUDENOTE: M0 throwaway profiling scaffolding for the dyntopo disk-bandwidth
// plan (documentation/plans/2026-07-12-1324-dyntopo-disk-bandwidth.md). Measures
// the disk-link share of a dyntopo dab: e_of_v / radial step counts,
// disk_insert/disk_remove counts, and nesting-aware timing buckets that separate
// topo splice vs attr interpolation vs meshlog/spatial callbacks vs the
// EdgeOfVertIter walk sites in dyntopo.h. Enabled by debug_app --profile;
// inert (one predictable branch per counter site) otherwise. The whole file and
// every DISKPROF call site are ripped out in plan M5.

#include "mesh_callbacks.h"

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace sculptcore::mesh::diskprof {

enum Bucket {
  B_OPS = 0,        /* runDyntopoRemesh whole call (self = loop/frontier misc) */
  B_TRI_PASS,       /* n-gon triangulate pre-pass */
  B_SCAN,           /* candidate build (self = consider() body minus children) */
  B_SCAN_WALK,      /* considerVertEdges e_of_v walks (includes consider body) */
  B_MIS,            /* independent-set selection (self = loop minus guards) */
  B_GUARD,          /* lockSplit/lockCollapse/splitFree/collapseFree walks */
  B_FEATURE,        /* isFeatureVert / featureCollapseOk walks */
  B_SPLIT,          /* splitEdge calls (self = topo splice + bookkeeping) */
  B_COLLAPSE,       /* collapseEdge calls (self = topo splice + bookkeeping) */
  B_FLIP_COLLECT,   /* flip-candidate collection walks */
  B_FLIP_APPLY,     /* flipQuad/flipShortens + flipEdge (self = splice) */
  B_SMOOTH,         /* smooth phase (self = write-back + loop) */
  B_SMOOTH_GATHER,  /* smoothTangent ring gathers */
  B_ATTR,           /* attr_interp.h snapshot/restore/interp helpers */
  B_CB_MESHLOG,     /* meshlog MeshCallbacks bodies */
  B_CB_SPATIAL,     /* spatial-tree MeshCallbacks bodies */
  B_COUNT
};

inline const char *bucketName(int b)
{
  static const char *names[B_COUNT] = {
      "ops(self)",     "tri_pass",    "scan(self)",   "scan_walk",
      "mis(self)",     "guard_walk",  "feature_walk", "split(self)",
      "collapse(self)","flip_collect","flip_apply",   "smooth(self)",
      "smooth_gather", "attr_interp", "cb_meshlog",   "cb_spatial"};
  return names[b];
}

struct DiskProf {
  bool enabled = false;

  double self_ms[B_COUNT] = {};   /* exclusive time per bucket */
  double total_ms[B_COUNT] = {};  /* inclusive time per bucket */
  long calls[B_COUNT] = {};

  uint64_t e_of_v_steps = 0;
  uint64_t radial_steps = 0;
  uint64_t disk_inserts = 0;
  uint64_t disk_removes = 0;
  long dabs = 0;

  void reset()
  {
    for (int i = 0; i < B_COUNT; i++) {
      self_ms[i] = total_ms[i] = 0.0;
      calls[i] = 0;
    }
    e_of_v_steps = radial_steps = disk_inserts = disk_removes = 0;
    dabs = 0;
  }

  void print(const char *tag) const
  {
    if (!enabled) {
      return;
    }
    std::printf("[disk-prof] === %s: %ld dabs ===\n", tag, dabs);
    double d = dabs > 0 ? double(dabs) : 1.0;
    std::printf("[disk-prof] counts: e_of_v_steps=%llu (%.0f/dab)  "
                "radial_steps=%llu (%.0f/dab)  disk_insert=%llu (%.0f/dab)  "
                "disk_remove=%llu (%.0f/dab)\n",
                (unsigned long long)e_of_v_steps, double(e_of_v_steps) / d,
                (unsigned long long)radial_steps, double(radial_steps) / d,
                (unsigned long long)disk_inserts, double(disk_inserts) / d,
                (unsigned long long)disk_removes, double(disk_removes) / d);
    /* ~20 B per e_of_v step (vs 8B + one 16B disk int4 line), ~4-8 B per radial
     * step, ~3 dirty lines per splice — sanity cross-check vs the time buckets. */
    double mb = (double(e_of_v_steps) * 20.0 + double(radial_steps) * 8.0 +
                 double(disk_inserts + disk_removes) * 3.0 * 64.0) /
                (1024.0 * 1024.0);
    std::printf("[disk-prof] est bytes touched by link chases+splices: %.1f MB "
                "(%.2f MB/dab)\n", mb, mb / d);
    std::printf("[disk-prof] %-14s %10s %12s %12s %12s\n", "bucket", "calls",
                "self_ms", "total_ms", "self_ms/dab");
    for (int i = 0; i < B_COUNT; i++) {
      if (calls[i] == 0) {
        continue;
      }
      std::printf("[disk-prof] %-14s %10ld %12.2f %12.2f %12.4f\n", bucketName(i),
                  calls[i], self_ms[i], total_ms[i], self_ms[i] / d);
    }
    std::fflush(stdout);
  }
};

inline DiskProf &get()
{
  static thread_local DiskProf p;
  return p;
}

/* Nesting-aware scoped timer: on exit adds elapsed-minus-children to its
 * bucket's self_ms and elapsed to total_ms, and reports elapsed to the parent
 * timer so outer buckets stay exclusive. No-op when profiling is disabled. */
struct ProfTimer {
  using Clock = std::chrono::steady_clock;

  static ProfTimer *&current()
  {
    static thread_local ProfTimer *cur = nullptr;
    return cur;
  }

  int bucket;
  bool active;
  Clock::time_point t0;
  ProfTimer *parent = nullptr;
  double child_ms = 0.0;

  explicit ProfTimer(int b) : bucket(b), active(get().enabled)
  {
    if (!active) {
      return;
    }
    parent = current();
    current() = this;
    t0 = Clock::now();
  }
  ~ProfTimer()
  {
    if (!active) {
      return;
    }
    double dt = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    DiskProf &p = get();
    p.self_ms[bucket] += dt - child_ms;
    p.total_ms[bucket] += dt;
    p.calls[bucket]++;
    if (parent) {
      parent->child_ms += dt;
    }
    current() = parent;
  }
  ProfTimer(const ProfTimer &) = delete;
  ProfTimer &operator=(const ProfTimer &) = delete;
};

/* Wrap every set callback in `in` with a ProfTimer on `bucket` so callback
 * bodies (meshlog vs spatial) are timed and excluded from splice self-time. */
inline MeshCallbacks wrapTimed(const MeshCallbacks &in, int bucket)
{
  MeshCallbacks out;
  auto wrap = [bucket](const litestl::util::function<void(int)> &f)
      -> litestl::util::function<void(int)> {
    if (!f) {
      return f;
    }
    return [bucket, f](int id) {
      ProfTimer t(bucket);
      f(id);
    };
  };
  out.onVertCreate = wrap(in.onVertCreate);
  out.onVertKill = wrap(in.onVertKill);
  out.onVertChange = wrap(in.onVertChange);
  out.onEdgeCreate = wrap(in.onEdgeCreate);
  out.onEdgeKill = wrap(in.onEdgeKill);
  out.onEdgeChange = wrap(in.onEdgeChange);
  out.onCornerCreate = wrap(in.onCornerCreate);
  out.onCornerKill = wrap(in.onCornerKill);
  out.onCornerChange = wrap(in.onCornerChange);
  out.onListCreate = wrap(in.onListCreate);
  out.onListKill = wrap(in.onListKill);
  out.onListChange = wrap(in.onListChange);
  out.onFaceCreate = wrap(in.onFaceCreate);
  out.onFaceKill = wrap(in.onFaceKill);
  out.onFaceChange = wrap(in.onFaceChange);
  return out;
}

} // namespace sculptcore::mesh::diskprof
