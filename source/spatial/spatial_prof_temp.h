#pragma once

/* CLAUDENOTE: temporary profiling scaffolding for the deferred-split /
 * regen_gpu_node parallelization plan
 * (documentation/plans/2026-07-09-1054-parallel-deferred-split-and-regen-gpu.md).
 * Shared by spatial.cc and spatial_gpu.cc; accumulates wall-clock per phase of
 * SpatialTree::update() plus M0 sub-phase attribution inside
 * applyDeferredNodeSplit/split_node and regen_gpu_node. Summary prints at
 * process exit. Rip this whole header out in M4. */

#include <chrono>
#include <cstdio>

namespace sculptcore::spatial::prof {

struct Stat {
  long count = 0;
  double total_ms = 0.0, min_ms = 0.0, max_ms = 0.0;

  void add(double ms)
  {
    if (count == 0 || ms < min_ms) {
      min_ms = ms;
    }
    if (count == 0 || ms > max_ms) {
      max_ms = ms;
    }
    total_ms += ms;
    count++;
  }
};

struct SpatialUpdateProf {
  Stat regenGpuNode;   // per regen_gpu_node call (serial)
  Stat slicePhase;     // update_gpu_node_slice parallel pass, wall
  Stat assignGpuNodes; // per assign_gpu_nodes call (serial)
  Stat trisPhase;      // ensure_node_tris parallel pass, wall
  Stat drawBatchLoop;  // serial draw-batch rebuild loop, wall
  Stat deferredSplit;  // applyDeferredNodeSplit, wall
  Stat deferredMerge;  // applyDeferredMerge (cadenced), wall
  Stat boundsPhase;    // regenDirtyBounds, wall
  Stat normalsPhase;   // update_node_normals parallel pass, wall
  Stat updateTotal;    // whole SpatialTree::update(), wall
  long sliceItems = 0; // total slices processed by the slice phase
  long triItems = 0;   // total leaves processed by the tris phase

  /* M0.1: applyDeferredNodeSplit / split_node attribution. All Stats accumulate
   * at every recursion depth (the categories never nest); refile includes the
   * nested inline splits, so pure descent = splitRefile - splitNested. */
  Stat splitTopLevel;      // per top-level split_node call (candidate)
  Stat splitUnassignMean;  // (a) vert unassign + mean pass
  Stat splitTriangulate;   // (b) triangulateFace calls (per split, summed)
  Stat splitRefile;        // (c) add_face_intern calls incl. nested splits
  Stat splitNested;        // (c') recursive inline split_node calls, wall
  Stat splitOrphan;        // (d) orphan-vert recovery walk
  Stat splitAlloc;         // (e) alloc_node/create_data/delete_data bookkeeping
  long splitCandidates = 0;     // candidate leaves seen by applyDeferredNodeSplit
  long splitCandidatesRun = 0;  // candidates that actually split (top-level calls)
  long splitRecursive = 0;      // recursive inline splits triggered by re-file
  long splitFacesRefiled = 0;   // faces re-filed across all splits
  long splitVertsUnassigned = 0;// verts unassigned (re-owned) across all splits
  long splitOrphansRecovered = 0; // verts recovered by the orphan walk

  /* M0.2: regen_gpu_node attribution (all serial today). */
  Stat regenDispose;      // (a) old buffer/cmd dispose
  Stat regenCollect;      // (b) collect_subtree_leaves + sizing + regen_node_tris
  Stat regenCreateBuf;    // (c) createBuffer calls + srcRefs resolve
  Stat regenFillSlice;    // (d) fill_leaf_slice calls
  Stat regenFillAttr;     // (d') fill_leaf_attr calls
  Stat regenFillLoop;     // (d+e) whole fill loop incl. slice-table build
  Stat regenOwnerVerts;   // per-owner total_verts (fill-work granularity)
  long regenTrisCalls = 0; // regen_node_tris invocations from inside regen
  long regenFillJobs = 0;  // M1: regen fill jobs run through the unified pass

  ~SpatialUpdateProf() { print(); }

  void print()
  {
    if (updateTotal.count == 0) {
      return;
    }
    std::printf("\n[spatial-prof] === SpatialTree::update() breakdown (%ld updates) ===\n",
                updateTotal.count);
    printStat("update() total     ", updateTotal);
    printStat("deferred split     ", deferredSplit);
    printStat("deferred merge     ", deferredMerge);
    printStat("ensure_node_tris   ", trisPhase);
    std::printf("[spatial-prof]     (%ld leaves total across phase runs)\n", triItems);
    printStat("bounds regen       ", boundsPhase);
    printStat("normals phase      ", normalsPhase);
    printStat("assign_gpu_nodes   ", assignGpuNodes);
    printStat("regen_gpu_node     ", regenGpuNode);
    printStat("unified fill phase ", slicePhase);
    std::printf("[spatial-prof]     (%ld slice updates + %ld regen fills across phase runs)\n",
                sliceItems, regenFillJobs);
    printStat("draw-batch loop    ", drawBatchLoop);

    if (splitTopLevel.count > 0) {
      std::printf("[spatial-prof] --- M0.1 split_node attribution ---\n");
      printStat("split top-level    ", splitTopLevel);
      printStat("  unassign+mean    ", splitUnassignMean);
      printStat("  triangulateFace  ", splitTriangulate);
      printStat("  refile (w/nested)", splitRefile);
      printStat("  nested splits    ", splitNested);
      std::printf("[spatial-prof]     refile pure descent = %.2f ms\n",
                  splitRefile.total_ms - splitNested.total_ms);
      printStat("  orphan recovery  ", splitOrphan);
      printStat("  alloc/data bkkp  ", splitAlloc);
      std::printf("[spatial-prof]     candidates=%ld run=%ld recursive=%ld "
                  "faces_refiled=%ld verts_unassigned=%ld orphans=%ld\n",
                  splitCandidates, splitCandidatesRun, splitRecursive,
                  splitFacesRefiled, splitVertsUnassigned, splitOrphansRecovered);
    }

    if (regenGpuNode.count > 0) {
      std::printf("[spatial-prof] --- M0.2 regen_gpu_node attribution ---\n");
      printStat("  dispose          ", regenDispose);
      printStat("  collect+size     ", regenCollect);
      printStat("  createBuffers    ", regenCreateBuf);
      printStat("  fill loop (all)  ", regenFillLoop);
      printStat("    fill_leaf_slice", regenFillSlice);
      printStat("    fill_leaf_attr ", regenFillAttr);
      std::printf("[spatial-prof]     regen_node_tris calls inside regen: %ld\n",
                  regenTrisCalls);
      if (regenOwnerVerts.count > 0) {
        std::printf("[spatial-prof]     per-owner total_verts: n=%ld avg %.0f min "
                    "%.0f max %.0f\n",
                    regenOwnerVerts.count,
                    regenOwnerVerts.total_ms / double(regenOwnerVerts.count),
                    regenOwnerVerts.min_ms, regenOwnerVerts.max_ms);
      }
    }
    std::fflush(stdout);
  }

  static void printStat(const char *name, const Stat &s)
  {
    if (s.count == 0) {
      std::printf("[spatial-prof]   %s (never ran)\n", name);
      return;
    }
    std::printf("[spatial-prof]   %s n=%-6ld total %9.2f  avg %8.3f  min %8.3f  max %8.3f  ms\n",
                name, s.count, s.total_ms, s.total_ms / double(s.count), s.min_ms,
                s.max_ms);
  }
};

inline SpatialUpdateProf spatialUpdateProf;

/* Nonzero while applyDeferredNodeSplit is on the stack — gates the split
 * sub-phase attribution so inline splits from ordinary add_face_at paths
 * don't pollute it. Single-threaded (split pass is serial). */
inline int inDeferredSplit = 0;
inline int splitDepth = 0;

struct Scope {
  Stat &stat;
  std::chrono::steady_clock::time_point start;

  Scope(Stat &s) : stat(s), start(std::chrono::steady_clock::now())
  {
  }
  ~Scope()
  {
    stat.add(std::chrono::duration<double, std::milli>(
                 std::chrono::steady_clock::now() - start)
                 .count());
  }
};

/* Accumulate into `stat` only when the deferred-split pass is active. */
struct SplitScope {
  Stat &stat;
  bool active;
  std::chrono::steady_clock::time_point start;

  SplitScope(Stat &s) : stat(s), active(inDeferredSplit > 0)
  {
    if (active) {
      start = std::chrono::steady_clock::now();
    }
  }
  ~SplitScope()
  {
    if (active) {
      stat.add(std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - start)
                   .count());
    }
  }
};

} // namespace sculptcore::spatial::prof
