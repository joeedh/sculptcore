/**
# Intro

Meshlog is the main undo/redo system for sculptcore. Its two principal
chunk types are:

* `LogChunkElems` — sparse, append-as-touched per-domain attribute-swap
  log for plain vertex-position / paint sculpting. The brush *Pre stage
  appends one row per element the first time it is touched in a step
  (gated by `AttrSaver`, so spatial-tree restructuring can't
  double-capture), and swaps the stored row with live on undo/redo.
  Which attributes a brush captures is declared per-brush via the sbrush
  `save` statement (defaults to vertex co/no + face no).

* `LogChunkTopo` — full topological log. Records every element touched
  during a step (Create / Change / Kill of verts, edges, corners,
  face-loop lists, and faces) and replays the events to either bring
  the mesh forward (redo) or backward (undo) by one step.

# LogChunkTopo: merged per-element records

Each logical element touched during a step gets at most one record in
the chunk. A record is classified by:

* **Origin**: `Existed` if the element was already alive at step-begin,
  `Created` if it was born during the step.
* **Fate**: `Live` if it survives to step-end, `Dead` if it was killed
  during the step.

A record carries up to two single-row attribute snapshots:

* **begin_body** — pre-step state, captured at first touch. Populated
  only for `Existed` records. Used for undo of `Existed && Dead`
  (alloc + writeTo) and as the swap pivot for `Existed && Live`.
* **end_body** — post-step state, captured at `finalizeStep`.
  Populated only for `Created && Live` records.

`Created && Dead` records (born and killed in the same step) are
**dropped** at kill time — the natural set-theoretic consequence of
"no net change to step-begin or step-end state", not a special
cancellation rule.

Mesh indices are reused from the freelist when an element is killed.
A log-local id generator plus an `(elem_kind, mesh_index) → log_id`
map disambiguates re-use within one step: kill of idx N unmaps it, a
subsequent create at idx N gets a fresh log_id and its own record. The
two records coexist (kill-first, create-second) and replay correctly.
*/

// #define MESHLOG_ABSEIL_HASHMAP

#pragma once

#include "attr_saver.h"
#include "binding/binding_constructor_builder.h"
#include "litestl/math/vector.h"
#include "litestl/util/map.h"
#include "litestl/util/set.h"
#include "litestl/util/span.h"
#include "litestl/util/vector.h"
#include "mesh/attribute.h"
#include "mesh/attribute_bool.h"
#include "mesh/mesh.h"
#include "mesh/mesh_callbacks.h"
#include "mesh/mesh_enums.h"
#include "mesh/mesh_path.h"
#include "mesh/ops/bevel.h"
#include "mesh/ops/extrude.h"
#include "mesh/ops/inset.h"
#include "mesh/ops/loopcut.h"
#include "mesh/ops/split.h"
#include "mesh/ops/subdivide.h"
#include "meshlog_chunk.h"
#include "meshlog_reorder.h"
#include "meshlog_row.h"
#include "meshlog_topo.h"
#include "meshlog_types.h"
#include "spatial/node.h"
#include "spatial/spatial.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace sculptcore::meshlog {
using litestl::math::float3;
using litestl::util::string;
using litestl::util::Vector;

struct MeshLog {
  /** Each field in LogEntry is processed in reverse
   * order (for undo) and order (for redo).  Undo
   * swaps with current data.
   */
  struct LogEntry {
    LogChunkTopo *topo_chunk_ = nullptr;
    bool hasTopoChunk = false;
    /** Set once the step's topo chunks have captured their end-state (see
     * finalizeStroke). Guards against a second capture from a mutated mesh —
     * stroke-end compaction finalizes early, then endStep must not re-capture. */
    bool finalized = false;

    Vector<LogChunk *> chunks;
    /** Monotonic step id assigned by beginStep; stable across history trims. */
    int id = -1;

    /* Active element per domain (vert/edge/face) captured at beginStep, swapped
     * with the live MeshLog active_* on undo/redo so box-modeling's "active
     * vertex" rides undo (the draft's "active vertex stored in meshlog"). */
    int snapActiveVert = -1;
    int snapActiveEdge = -1;
    int snapActiveFace = -1;

    LogEntry() = default;
    LogEntry(const LogEntry &b) = default;
    LogEntry(LogEntry &&b) = default;
    LogEntry &operator=(LogEntry &&b) = default;
    LogEntry &operator=(const LogEntry &b) = default;

    ~LogEntry()
    {
      for (LogChunk *chunk : chunks) {
        litestl::alloc::Delete(chunk);
      }
    }

    double memSize()
    {
      double tot = double(sizeof(*this));
      for (LogChunk *chunk : chunks) {
        tot += chunk->memSize();
      }
      return tot;
    }
  };

  static litestl::binding::types::Struct<MeshLog> *defineBindings()
  {
    using namespace litestl::binding;
    using binding::types::Struct;
    Struct<MeshLog> *st =
        new Struct<MeshLog>("sculptcore::meshlog::MeshLog", sizeof(MeshLog));

    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    // Step/chunk sequencing (beginStep/endStep/pushTopoChunk/setActiveMesh) is
    // intentionally NOT bound: the brush CommandExecutor owns the dab sequence
    // and meshlog boundaries; JS clients drive undo/redo + memory accounting only.
    BIND_STRUCT_METHOD(st, undo, MARGS("m", "tree"));
    BIND_STRUCT_METHOD(st, redo, MARGS("m", "tree"));
    BIND_STRUCT_METHOD(st, curStrokeId, MARGS());
    BIND_STRUCT_METHOD(st, lastStepId, MARGS());
    BIND_STRUCT_METHOD(st, stepMemSize, MARGS("id"));
    BIND_STRUCT_METHOD(st, totalMemSize, MARGS());
    BIND_STRUCT_METHOD(st, entryCount, MARGS());
    BIND_STRUCT_METHOD(st, freeStep, MARGS("id"));
    BIND_STRUCT_METHOD(st, hasTopoChunk, MARGS());
    BIND_STRUCT_METHOD(st, reorderForLocality, MARGS("tree"));
    BIND_STRUCT_METHOD(st, compactIfFragmented, MARGS("tree", "vertRatioThreshold"));

    // Box-modeling topology macro-ops.
    BIND_STRUCT_MEMBER(st, selectFlushPreferOpDomain);
    BIND_STRUCT_METHOD(st, extrudeRegion, MARGS("m", "outNormal"));
    BIND_STRUCT_METHOD(st, extrudeIndividual, MARGS("m", "outNormal"));
    BIND_STRUCT_METHOD(st, extrudeWireVerts, MARGS("m", "outNormal"));
    BIND_STRUCT_METHOD(st, splitFacesOff, MARGS("m", "outNormal"));
    BIND_STRUCT_METHOD(st, subdivideEdges, MARGS("m", "numCuts", "outVerts"));
    BIND_STRUCT_METHOD(st, loopCut, MARGS("m", "seedEdge", "outVerts"));
    BIND_STRUCT_METHOD(st, loopCutAtRay, MARGS("m", "tree", "origin", "dir", "outVerts"));
    BIND_STRUCT_METHOD(st, insetRegion, MARGS("m", "insetVerts", "baseCo", "tangent"));
    BIND_STRUCT_METHOD(st, bevelVerts, MARGS("m", "verts", "baseCo", "tangent"));

    // Box-modeling selection (undoable).
    BIND_STRUCT_METHOD(st, selectionBeginStep, MARGS());
    BIND_STRUCT_METHOD(st, selectionEndStep, MARGS());
    BIND_STRUCT_METHOD(st, selectOne, MARGS("m", "domain", "idx", "state"));
    BIND_STRUCT_METHOD(st, selectIndices, MARGS("m", "domain", "indices", "state"));
    BIND_STRUCT_METHOD(st, selectAllElems, MARGS("m", "domain", "state"));
    BIND_STRUCT_METHOD(st, selectShortestPath, MARGS("m", "vEnd", "state"));
    BIND_STRUCT_METHOD(st, selectLoop, MARGS("m", "seedEdge", "kind", "state"));
    BIND_STRUCT_METHOD(st,
                       selectScreenCircle,
                       MARGS("m", "tree", "co", "ray", "r1", "r2", "domain", "state"));
    BIND_STRUCT_METHOD(st,
                       selectScreenRect,
                       MARGS("m",
                             "tree",
                             "near0",
                             "near1",
                             "near2",
                             "near3",
                             "far0",
                             "far1",
                             "far2",
                             "far3",
                             "domain",
                             "state"));
    BIND_STRUCT_METHOD(st, setActiveElem, MARGS("domain", "idx"));
    BIND_STRUCT_METHOD(st, activeVert, MARGS());
    BIND_STRUCT_METHOD(st, activeEdge, MARGS());
    BIND_STRUCT_METHOD(st, activeFace, MARGS());

    return st;
  }
  Vector<LogEntry> entries;

  MeshLog();
  ~MeshLog() = default;

  /** Pass to mesh topology ops so they fire into the current topo chunk. */
  mesh::MeshCallbacks *callbacks();

  /** Current Mesh* — must be set by the caller before issuing logged ops
   *  so the topo chunk can snapshot pre-kill attributes by index. */
  void setActiveMesh(mesh::Mesh *m);

  void beginStep(bool hasDyntopo);

  /** Current stroke id, masked to 16 bits (see AttrSaver stamp packing). Starts
   * at 1 so a freshly-stamped 0 element always reads as "not saved yet". */
  int curStrokeId() const;

  /** Id of the most recently begun step (-1 if none). Call right after
   * beginStep to key this step for stepMemSize/freeStep. */
  int lastStepId();

  bool hasOpenStepFor(const mesh::Mesh *mesh, int id) const;

  /** Estimated heap bytes retained by the step with @p id (0 if freed). */
  double stepMemSize(int id);

  double totalMemSize();

  /** Deform-pool bytes, counted once here rather than folded into each chunk's
   * elemSize * rows — a WEIGHTS cell is a 4-byte slot index, and the runs behind
   * those indices are one shared table that every step and the mesh index into.
   * The log is what keeps swept-out runs alive, so the bytes belong in the undo
   * budget even though the mesh owns the table. */
  double deformPoolMemSize();

  int entryCount();

  /** Free the committed step with @p id (undo-memory eviction from the app's
   * tool stack). Only steps strictly behind the cursor are freeable — the
   * current/redo entries stay. Returns 1 if a step was freed. */
  int freeStep(int id);

  /** Capture every topo chunk's end-state from the CURRENT mesh: the active
   * chunk's Created&&Live end_body, plus a data-column refresh of every chunk's
   * Created verts/faces. Idempotent per step (the `finalized` guard) so the
   * stroke-end auto-compaction can finalize early — it MUST, because it permutes
   * the live mesh afterwards: end_body holds TOPO connectivity by index, and
   * redo replays the topo chunks into the pre-reorder layout, so a post-reorder
   * capture corrupts the corner cycles (infinite loop in add_face). */
  void finalizeStroke();

  void endStep();

  /** Cap the retained undo history to @p n committed steps (-1 = unbounded).
   * Trims immediately so lowering the cap at runtime frees old steps now. */
  void setMaxUndoSteps(int n);
  int maxUndoSteps() const;

  bool hasTopoChunk() const;

  void pushTopoChunk();

  /** Lazily allocates a topo chunk in the current entry. */
  LogChunkTopo *getTopoChunk();

  /* ------------- Live-preview mid-step rollback (Anchored / Drag Dot) -------
   * A preview dab is a real applyDab() call issued while the pointer is still
   * moving, whose effect must vanish the instant the NEXT preview (or the
   * final) dab is applied — never compound. The step-wide chunks above have no
   * per-dab granularity for this (LogChunkElems captures once per domain per
   * WHOLE step, not per dab), so rollback is a hybrid: pop-and-undo whatever
   * LogChunkTopo chunk(s) the preview dab pushed (applyDab already seals one
   * fresh topo chunk per dab, so "chunks appended since beginPreviewDab" IS
   * exactly this dab's topology), plus a raw vertex row snapshot/restore for
   * the non-topological position/attribute deform, captured and rolled back
   * outside the log entirely — mirroring the debug-app save_pos/assert_pos
   * pattern. See documentation/plans/anchored-drag-dot-stroke-2026-07-16.md
   * step 2a. */
  struct PreviewState {
    bool active = false;
    size_t chunkBaseline = 0;
    detail::RowLayout vertLayout;
    Vector<int> vertIdx;
    Vector<detail::ChunkElemRow> vertRows;
    /* Elements whose undo gate (vertGate_/faceGate_) was freshly stamped by a
     * TOPOLOGY touch (stampUndoGate) while this preview dab was active — must
     * be un-stamped on rollback since the chunk that stamped them is deleted.
     * Populated by stampUndoGate itself (not the element store's own capture,
     * which stamps the same gate but owns a row that outlives rollback). */
    Vector<int> gatedVert;
    Vector<int> gatedFace;
    Vector<int> gatedCorner;
    /* Dedup set across every region captured this preview session (begin +
     * any extend) so a vert shared by two mirror images is only snapshotted
     * once, at its first-seen (pre-dab) state. */
    util::Set<int> seenIdx;
  };
  PreviewState preview_;

  bool previewActive() const;

  /** Commit the pending preview dab: its mesh edits and log chunks stay applied
   * exactly as-is, and the snapshot bookkeeping is simply dropped (no rollback,
   * no mesh touch). Call once at the true end of a stroke — beginStep() does
   * not itself clear preview_, so an un-committed preview_.active would
   * otherwise survive into the next stroke's fresh LogEntry and cause the next
   * stroke's first preview dab to roll back against the wrong entry/snapshot.
   * No-op if no preview is pending. */
  void commitPreviewDab();

  /** Snapshot every unseen vertex within `radius` of `center` into the
   * CURRENT preview session (dedup via preview_.seenIdx) — shared by
   * beginPreviewDab (fresh session) and extendPreviewDab (add a region to an
   * already-open one). Assumes preview_.vertLayout is already built.
   *
   * Also force-claims the step-wide undo gate for each vertex right here,
   * before any dab in this group runs. A dab can move a vertex outside its
   * own brush-kernel iteration set (e.g. a dyntopo-side-effect position
   * write on a vertex that isn't part of the deform's node-filtered
   * region) without going through the brush's AttrSaver-gated Pre-stage
   * capture at all; the only other capture path (a topology-touch callback
   * via stampUndoGate/fwd) fires lazily, sometime after such a write has
   * already happened, and ends up capturing the mutated value instead of
   * the original. Claiming the gate — and seeding the element store — here
   * makes this pre-group snapshot the oldest, and thus authoritative, undo
   * body for the vertex regardless of what fires later this step. */
  void capturePreviewRegion(mesh::Mesh *m,
                            spatial::SpatialTree *tree,
                            float3 center,
                            float radius)
  {
    Vector<spatial::SpatialNode *> nodes;
    nodes.ensure_capacity(64); // one alloc rather than growing 4 -> 8 -> ... per dab
    tree->filterNodes(center, radius, nodes);
    capturePreviewNodes(m, {nodes.data(), nodes.size()});
  }

  /** Extend the current preview with the executor's evaluated region union. */
  void capturePreviewNodes(mesh::Mesh *m, std::span<spatial::SpatialNode *> nodes);

  /** Snapshot every vertex within `radius` of `center` — a generous superset
   * of what one dab at this location can touch — and remember the step's
   * current chunk count. Call immediately before issuing a preview-only
   * applyDab(); pair with rollbackPreviewDab() before the next preview dab.
   * Replaces any prior un-rolled-back snapshot. Under symmetry, this starts
   * the group (the primary dab); each mirror image adds its own region via
   * extendPreviewDab() so the whole group rolls back as one unit. */
  void
  beginPreviewDab(mesh::Mesh *m, spatial::SpatialTree *tree, float3 center, float radius);

  /** Add another (center, radius) region to the CURRENT preview session
   * without resetting its chunk baseline or snapshot — for symmetry, where
   * one driver tick applies a primary dab plus one per active mirror axis,
   * all of which must roll back together as a single unit before the next
   * tick's dabs land. Verts already captured (shared across mirror images,
   * e.g. on the mirror plane) are left at their pre-dab snapshot, not
   * re-captured mid-group. No-op fallback: if called with no session open
   * (beginPreviewDab wasn't called first), behaves as beginPreviewDab. */
  void
  extendPreviewDab(mesh::Mesh *m, spatial::SpatialTree *tree, float3 center, float radius);

  /** Undo the effect of the most recent preview dab: pop and undo every chunk
   * pushed since the paired beginPreviewDab(), then restore the snapshotted
   * vertex rows directly (bypassing the log). No-op if no snapshot is
   * pending. Leaves the step open — this is NOT MeshLog::undo(), which closes
   * a step and moves the history cursor. */
  void rollbackPreviewDab(mesh::Mesh *m, spatial::SpatialTree *tree);

  /** Find-or-create the current step's per-domain element store (the
   * append-as-touched undo capture for AttrSaver-gated brush deformation). */
  LogChunkElems *elemStore(mesh::ElemType domain);

  /** Append a reorder chunk capturing the five permutations to the current
   * step. Caller applies the reorder itself (via SpatialTree::applyReorder);
   * the chunk only stores the maps for later undo/redo. */
  LogChunkReorder *pushReorderChunk(Vector<int> vmap,
                                    Vector<int> emap,
                                    Vector<int> cmap,
                                    Vector<int> lmap,
                                    Vector<int> fmap);

  /** Append a caller-built chunk (LogChunkTypes::External subclasses) to the
   * open step; MeshLog takes ownership and drives it purely through the
   * undo/redo virtuals. Aborts when no step is open (mirrors pushReorderChunk). */
  void appendChunk(LogChunk *chunk);

  /** Atomic reorder step: open a step, record the five permutations, close it.
   * The one sanctioned way for non-brush code (Scene locality reorder) to push
   * an undo step without driving beginStep/endStep itself. */
  LogChunkReorder *pushReorderStep(Vector<int> vmap,
                                   Vector<int> emap,
                                   Vector<int> cmap,
                                   Vector<int> lmap,
                                   Vector<int> fmap);

  /** TS-app entry point for the "optimize mesh layout" button: compute locality
   * permutations from the tree, record an undoable reorder step, then apply it.
   * Mirrors debug Scene::reorderForLocality so the app path is fully
   * meshlog-aware. Pushes the maps (copied) before applying, matching the debug
   * ordering. No-op without a tree. */
  void reorderForLocality(spatial::SpatialTree *tree);

  /** Stroke-boundary auto-compaction (mechanism B). MUST be called with the
   * stroke's undo step still OPEN (before endStep): if the tree's vert page-
   * spread exceeds @p vertRatioThreshold (1.0 = perfectly compact), append an
   * incremental reorder CHUNK to the current step and apply it. Folding the
   * reorder into the stroke's step means one undo reverts stroke + compaction
   * together AND keeps the stroke's id-based chunks valid (the reorder chunk,
   * being last, is inverted FIRST on undo, restoring the pre-compaction ids the
   * earlier chunks expect). Returns true if it compacted. Cheap to call every
   * stroke — fragmentationStats is O(elements) and the gate skips the work until
   * churn has actually scattered the layout. */
  bool compactIfFragmented(spatial::SpatialTree *tree, double vertRatioThreshold = 3.0);

  /* -------------------- Box-modeling topology macro-ops --------------------
   * Each brackets one MeshLog step (so it's one undo press), sets the active
   * mesh so the create/change/kill callbacks fire into the topo chunk, runs the
   * Euler-op composition (mesh / ops / *), and leaves the new movable region
   * selected. `outNormal` receives the op's averaged normal (3 floats) for the
   * chained transform's default constraint axis. The spatial tree is rebuilt by
   * the TS op afterward (topology changed wholesale). */

  /** selectFlush op-domain preference (mesh/utils/select_derive.h). Bound so the
   * TS ops mirror the sculptcore.select_flush_prefer_op_domain feature flag into
   * it before each macro-op (the auto_defrag pattern: flag read TS-side). */
  bool selectFlushPreferOpDomain = true;

  void extrudeRegion(mesh::Mesh *m, util::Vector<float> &outNormal);

  void extrudeIndividual(mesh::Mesh *m, util::Vector<float> &outNormal);

  void extrudeWireVerts(mesh::Mesh *m, util::Vector<float> &outNormal);

  void splitFacesOff(mesh::Mesh *m, util::Vector<float> &outNormal);

  /* Subdivide the selected edges (or the selected faces' edges) with `numCuts`
   * cuts each (immediate; self-brackets a step). Outputs the created cut verts. */
  void subdivideEdges(mesh::Mesh *m, int numCuts, util::Vector<int> &outVerts);

  /* Loop-cut the quad strip through `seedEdge` (immediate; self-brackets). Outputs
   * the new loop's midpoint verts (left selected). */
  void loopCut(mesh::Mesh *m, int seedEdge, util::Vector<int> &outVerts);

  /* Loop-cut at a cursor ray: cast against `tree`, seed from the hit face's edge
   * nearest the hit point, then cut. Outputs the new loop's verts (selected). */
  void loopCutAtRay(mesh::Mesh *m,
                    spatial::SpatialTree *tree,
                    const math::float3 &origin,
                    const math::float3 &dir,
                    util::Vector<int> &outVerts);

  /* Build the inset ring (parametric modal). Unlike the extrude wrappers this
   * does NOT bracket the step — the modal op holds one step open across the drag
   * (selectionBeginStep -> insetRegion -> drag setVertCo -> selectionEndStep), so
   * the created inset verts capture their final dragged positions at finalizeStep.
   * Outputs the inset vert indices + base coords + inward tangents (flat). */
  void insetRegion(mesh::Mesh *m,
                   util::Vector<int> &insetVerts,
                   util::Vector<float> &baseCo,
                   util::Vector<float> &tangent);

  /* Bevel the selected verts (parametric modal; does NOT self-bracket, like
   * insetRegion). Outputs the offset verts + base coords + edge tangents. */
  void bevelVerts(mesh::Mesh *m,
                  util::Vector<int> &verts,
                  util::Vector<float> &baseCo,
                  util::Vector<float> &tangent);

  /* -------------------- Box-modeling selection (undoable) --------------------
   * The per-element `select` bool is a normal (non-TOPO, non-NOCOPY) data column,
   * so snapshotting a changed element into the step's topo chunk via onChange (a
   * full-row capture) makes undo/redo swap `select` back exactly like positions.
   * onChange snapshots an Existed element only on first touch, so repeated writes
   * across a modal drag accumulate into a single undo step. The step bracketing is
   * split (begin/end) so a circle-brush drag owns one step; `domain` is 0 = vertex,
   * 1 = edge, 2 = face (a code, not an ElemType/SelMask flag — those disagree on
   * FACE). These are the sanctioned non-brush selection entry, like
   * reorderForLocality is for the locality reorder. */

  /** Open a selection step. Snapshots active elements for undo (see beginStep). */
  void selectionBeginStep();

  /** Close the current selection step. */
  void selectionEndStep();

  static LogElemKind selectDomainKind(int domain);

  /** Snapshot then set one element's select bool. Caller is inside a step. */
  void selectOne(mesh::Mesh *m, int domain, int idx, bool state);

  /** Snapshot + set select for a list of element indices (reuses a spatial
   * query's bound out-vector as input). Caller is inside a step. */
  void selectIndices(mesh::Mesh *m, int domain, util::Vector<int> &indices, int state);

  /** Snapshot + set select for every live element in `domain`. */
  void selectAllElems(mesh::Mesh *m, int domain, int state);

  /** Select the shortest edge-path from the active vertex to `vEnd`; `vEnd`
   * becomes the new active vertex (the draft's path-select). Returns the number
   * of path verts (0 if unreachable, but active still advances). Inside a step. */
  int selectShortestPath(mesh::Mesh *m, int vEnd, int state);

  /** Select the edge loop (kind 0), edge ring (kind 1), or face loop (kind 2)
   * seeded at `seedEdge` (the ctrl / ctrl-shift click select). A select of an
   * already fully-selected loop DESELECTS it instead (loop toggle). Pure
   * selection; caller brackets the step. Returns the element count walked,
   * negated when the toggle deselected. */
  int selectLoop(mesh::Mesh *m, int seedEdge, int kind, int state);

  /* Select elements from a spatial query's collected face/vert sets, by domain.
   * vert → the collected verts; face → the collected faces; edge → edges whose
   * BOTH endpoints were collected (the natural "edge inside the region" rule).
   * Caller is inside a step. */
  void selectFromSets(mesh::Mesh *m,
                      int domain,
                      util::Vector<int> &faces,
                      util::Vector<int> &verts,
                      bool state);

  /* Cone (circle/brush) select: run the spatial cone query and select the hits
   * in `domain`. Pick + select happen entirely in C++ so no index array crosses
   * the binding. Caller brackets the step (one step per drag for the brush). */
  void selectScreenCircle(mesh::Mesh *m,
                          spatial::SpatialTree *tree,
                          const math::float3 &co,
                          const math::float3 &ray,
                          float r1,
                          float r2,
                          int domain,
                          int state);

  /* Box select: run the spatial frustum query (8 object-local corners, like
   * SpatialTree::castScreenRect) and select the hits in `domain`. */
  void selectScreenRect(mesh::Mesh *m,
                        spatial::SpatialTree *tree,
                        const math::float3 &near0,
                        const math::float3 &near1,
                        const math::float3 &near2,
                        const math::float3 &near3,
                        const math::float3 &far0,
                        const math::float3 &far1,
                        const math::float3 &far2,
                        const math::float3 &far3,
                        int domain,
                        int state);

  /** Set the active element for a domain (0/1/2). Inside a step so it rides undo. */
  void setActiveElem(int domain, int idx);

  int activeVert() const;
  int activeEdge() const;
  int activeFace() const;

  LogEntry &curEntry();

  const LogEntry &curEntry() const;

  void undo(mesh::Mesh *m, spatial::SpatialTree *tree);

  void redo(mesh::Mesh *m, spatial::SpatialTree *tree);

private:
  /* Topo chunks replay faces with raw alloc/release, bypassing make_face /
   * kill_face — the choke points maintaining `Mesh::n_ngon_faces`. Rescan after
   * a topo replay so dyntopo's triangulate-prepass gate stays exact (a stale 0
   * on a restored n-gon mesh silently refuses every subsequent split). */
  void resyncNgonCount(mesh::Mesh *m, LogEntry &e);

  /* Swap the live active elements with this step's snapshot. Symmetric: undo
   * swaps live(post-step)↔snap(pre-step) → live becomes pre-step; redo swaps
   * again → live becomes post-step. */
  void swapActiveElems(LogEntry &e);

  /* Topo chunks restore elements with raw alloc/release + attr memcpys,
   * bypassing the auto-thawing topology mutators. On a frozen mesh the live
   * TOPO link pages are freed (getElemData == null), so thaw first. */
  void thawForTopoChunks(mesh::Mesh *m);

  /** Drop oldest committed steps until at most maxUndoSteps_ remain. The popped
   * LogEntry is destroyed by value, so ~LogEntry frees its chunks. Stops at
   * curStep_ == 0 so it never discards the current step or pending redo. */
  void trimHistory();

  /* Stamp the brush's per-element save-gate (`.strokeid.<domain>`) so the brush
   * deform — which runs AFTER dyntopo each dab — treats this element as already
   * saved and skips appending it to the per-step element store. The topo chunk
   * captured this element's true pre-step body on first touch, so its restore is
   * authoritative; an element-store row would hold a stale post-dyntopo value
   * and, being older than later dabs' topo chunks, would win the newest-first
   * undo and re-corrupt the element. Only the brush-gated domains (vert co/no,
   * face no) need stamping. Stamp the full flag set so any brush save mask is
   * covered. */
  void stampUndoGate(LogElemKind kind, int idx);

  /* Per-domain generation-stamped element sets (dense read instead of the
   * makeKey/hash lookup). Two tiers below: per-CHUNK ("already recorded in
   * the active chunk" — the no-op fast path, plan 2026-07-12-2141 M1;
   * see the P2 NOTE below for why there is no step-scoped tier). Stamps are set
   * only right after the chunk records the element, cleared by onKill
   * (index reuse must re-record), and invalidated wholesale by a generation
   * bump — so a stale hit is impossible by construction. */
  struct ElemStampTier {
    litestl::util::Vector<uint32_t> stamp[5];
    uint32_t gen = 1;

    void bump()
    {
      if (++gen == 0) {
        for (int k = 0; k < 5; k++) {
          stamp[k].clear();
        }
        gen = 1;
      }
    }
    bool hit(LogElemKind kind, int idx) const
    {
      const litestl::util::Vector<uint32_t> &s = stamp[int(kind)];
      return uint32_t(idx) < uint32_t(s.size()) && s[idx] == gen;
    }
    void set(LogElemKind kind, int idx)
    {
      litestl::util::Vector<uint32_t> &s = stamp[int(kind)];
      if (uint32_t(idx) >= uint32_t(s.size())) {
        litestl::alloc::PermanentGuard guard; /* persistent buffer: not a leak */
        int old = int(s.size());
        s.resize(idx + 1);
        for (int i = old; i <= idx; i++) {
          s[i] = 0;
        }
      }
      s[idx] = gen;
    }
    void clear(LogElemKind kind, int idx)
    {
      litestl::util::Vector<uint32_t> &s = stamp[int(kind)];
      if (uint32_t(idx) < uint32_t(s.size())) {
        s[idx] = 0;
      }
    }
  };

  /* No step-scoped tier: cross-chunk Existed&&Live dedup yields the right
   * final state, but each chunk's replay tree passes walk corner loops
   * mid-undo and hang on mixed-era links (plan 2026-07-13-2046 P2). */
  ElemStampTier chunk_stamp_;

  void bumpChunkStampGen();

  void installCallbacks();

  int curStep_;
  mesh::MeshCallbacks cb_;
  mesh::Mesh *active_mesh_ = nullptr;
  /* The active mesh's deform pool, if it has one. A strong reference: chunks
   * hold their own too, but this one also survives a step being freed. */
  mesh::DeformPoolUser deform_pool_;
  int maxUndoSteps_ = -1; // -1 = unbounded
  int nextStepId_ = 0;
  int strokeId_ = 0; // bumped to 1 on the first beginStep (see curStrokeId)
  /* Box-modeling active element per domain (vert/edge/face index, -1 = none).
   * Snapshotted per step in LogEntry; see swapActiveElems / setActiveElem. */
  int active_vert_ = -1;
  int active_edge_ = -1;
  int active_face_ = -1;
  /* Brush save-gate stampers, shared (by attribute name) with the brush kernels'
   * own AttrSavers — see stampUndoGate. The corner gate is shared with the UV
   * slide-reprojection capture (CommandExecutor::reprojectUvsWithCapture). */
  AttrSaver<mesh::ElemType::VERTEX> vertGate_;
  AttrSaver<mesh::ElemType::FACE> faceGate_;
  AttrSaver<mesh::ElemType::CORNER> cornerGate_;
};

} // namespace sculptcore::meshlog
