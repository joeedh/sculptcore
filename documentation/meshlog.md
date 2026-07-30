# Meshlog — High-level overview

The meshlog subsystem (`source/meshlog/`) is the sculpt undo/redo log.
It records mesh edits as a sequence of *steps*, each step composed of
one or more *chunks*; the chunks carry just enough state to swap the
live mesh back to its pre-step form (undo) and forward again (redo).

Three chunk types coexist in one step:

* **`LogChunkElems`** — sparse, append-as-touched per-domain attribute
  swap. The brush `*Pre` stage appends one row per element the first
  time it is touched in a step — gated by a per-element `AttrSaver`
  stamp (see `attr_saver.h`), so it captures each element's pre-step
  state exactly once even when the spatial tree restructures mid-stroke
  (dyntopo). Undo/redo swap the stored rows with live. Which attributes
  a brush captures is declared per-brush via the sbrush `save` statement
  (defaulting to vertex co/no + face no). Used by position-only and
  paint brushes where topology does not change. One chunk per domain
  per step. Resilient to attribute *reordering* between capture and
  replay (it remaps its columns to live attrs by name before each swap).

* **`LogChunkTopo`** — topological log. Driven by `mesh::MeshCallbacks`,
  it records every create / change / kill event fired by mesh ops and
  replays them in order (redo) or reverse order (undo). Used by brushes
  that add or remove geometry (split, collapse, dissolve, …) and by
  dynamic topology. A dyntopo step holds **one topo chunk per dab**, not
  one per step (see "Per-dab topo chunks" below).

* **`LogChunkReorder`** — replays the five element permutations for a
  spatial reorder. A reorder is a pure bijection; undo replays the
  inverse permutation, redo the forward one.

`MeshLog` owns the chunks, exposes a callback object that the mesh
operations fire into, and provides `beginStep` / `endStep` /
`pushTopoChunk` / `undo` / `redo` to the brush executor.

## File map

| File | Role |
|---|---|
| `meshlog.h` | Umbrella — includes `meshlog_base.h`. |
| `meshlog_base.h` | All types: `MeshLog`, `LogEntry`, `LogChunk`, `LogChunkElems`, `LogChunkTopo`, `LogChunkReorder`, `LogElem`, `detail::ChunkElemData`, `detail::ChunkElemRow`, the `LogElemKind` / `LogOrigin` / `LogFate` enums. |
| `attr_saver.h` | `AttrSaver<ElemType>` — the per-element "already saved this stroke?" gate. Drives `LogChunkElems` capture, and is *also* used directly by `MeshLog` (`vertGate_` / `faceGate_`) to fence dyntopo-moved elements out of the element store (see `stampUndoGate`). |
| `bindings.h` / `bindings.cc` | `registerBindings(BindingManager&)` — exposes `MeshLog` to the litestl reflection layer. |
| `CMakeLists.txt` | Static library `meshlog`, depends on `util`, `math`, `props`, `spatial`, `mesh`. |

## Core types

### `MeshLog`
The top-level log object. Holds a `Vector<LogEntry>` (one entry per
step) and a cursor `curStep_`. Notable members:

* `callbacks()` — returns the `mesh::MeshCallbacks*` that mesh
  topology ops must fire into so events land in the current topo
  chunk. The callback object is owned by `MeshLog`; mesh code receives
  a borrowed pointer. The forwarders early-out when no active mesh is
  set, and `onCreate`/`onChange` additionally call `stampUndoGate`.
* `setActiveMesh(mesh::Mesh*)` — must be called before logged ops run,
  because the topo chunk snapshots attributes by mesh index at the
  moment of first touch and at kill. Also binds the `vertGate_` /
  `faceGate_` stamp columns up front so `stampUndoGate` never allocates
  mid-stroke.
* `beginStep(bool hasDyntopo)` — appends a fresh `LogEntry` (stamped
  with a monotonic step id) and bumps the stroke id. If the cursor is
  mid-history (after some undos), the future entries are discarded
  first. When `hasDyntopo` is true it immediately `pushTopoChunk()`s the
  first dab's topo chunk.
* `lastStepId()` — id of the most recently begun step; the host reads
  it right after `beginStep` to key the step for `stepMemSize` /
  `freeStep`.
* `curStrokeId()` — the current step's stroke id, masked to 16 bits
  (bumped per `beginStep`, starting at 1). `AttrSaver` stamps elements
  with it so the first dab of a step captures and later dabs skip; the
  65536-stroke wrap is harmless (only equality within a step matters).
* `hasTopoChunk()` — whether the current step has ever pushed a topo
  chunk (sticky for the life of the step, even across dabs).
* `pushTopoChunk()` — finalizes the currently-active topo chunk (if any)
  then allocates a fresh one and makes it active. Called once per dyntopo
  dab by the host (after `applyDynTopoDab`). See "Per-dab topo chunks".
* `getTopoChunk()` — returns the active topo chunk, lazily pushing one
  if none is active. The topology callbacks route through this, so any
  logged topology op auto-creates a chunk even outside dyntopo.
* `elemStore(domain)` — lazily allocates (or returns the existing)
  `LogChunkElems` for the given element domain in the current step. The
  brush `*Pre` stage appends touched-element rows into it.
* `pushReorderChunk(vmap, emap, cmap, lmap, fmap)` — append a
  `LogChunkReorder` capturing the five permutations to the current step.
  The caller applies the reorder itself (via `SpatialTree::applyReorder`);
  the chunk only stores the maps for later undo/redo.
* `stepMemSize(id)` / `totalMemSize()` / `entryCount()` — undo-memory
  accounting: estimated heap bytes retained by one step (0 if freed)
  or by the whole history, and the live entry count.
* `freeStep(id)` — evicts a committed step (undo-memory limit
  enforcement from the app's tool stack). Only steps strictly behind
  the cursor are freeable; returns 1 if a step was freed.
* `setMaxUndoSteps(n)` / `maxUndoSteps()` — cap the retained history to
  `n` committed steps (`-1` = unbounded). Lowering the cap trims
  immediately. `trimHistory` drops oldest steps with `pop_front`, never
  past `curStep_` (so it never discards the current step or pending
  redo); it runs at `endStep` and whenever the cap is set.
* `endStep()` — finalizes the still-active topo chunk (earlier per-dab
  chunks were already finalized at deactivation by `pushTopoChunk`),
  clears the active-chunk pointer, advances the cursor, then trims
  history to the cap.
* `undo(Mesh*, SpatialTree*)` / `redo(Mesh*, SpatialTree*)` — undo
  decrements the cursor then replays the now-current entry's chunks in
  **reverse** creation order; redo replays the current entry forward
  then increments the cursor. The chunks themselves decide swap
  direction. Both first call `thawForTopoChunks` (see pitfalls). Reverse
  order on undo matters for a folded sculpt step (per-dab topo chunks
  created first, then the brush's `LogChunkElems` store): the topo
  chunks hold the authoritative pre-step bodies for dyntopo-moved verts,
  and `stampUndoGate` keeps those verts *out* of the element store, so
  the two no longer overlap.

### `LogEntry`
A step's container. Owns `Vector<LogChunk*>` and `Delete`s the chunks
in its destructor. Carries a monotonic `id` (stable across history
trims), a sticky `hasTopoChunk` flag, and `topo_chunk_` — a cached
pointer to the *currently-active* topo chunk (the dab being recorded),
reset to null at `endStep` and re-pointed by each `pushTopoChunk`. The
chunk vector is replayed in creation order (reverse for undo).

### `LogChunk`
Polymorphic base with `type` tag, virtual `undo` / `redo` / `memSize`.
Concrete subclasses: `LogChunkElems`, `LogChunkTopo`, `LogChunkReorder`.

#### `LogChunkElems`
Holds one `detail::ChunkElemData` block for a single element `domain`,
grown by `appendFrom` as the brush touches elements (sparse — only
touched elements get a row). Each row stores the brush's declared
attribute snapshot plus a built-in `origIndex` mapping it back to its
mesh index. `undo()` / `redo()` swap the block with the live
`AttrGroup` and request a GPU / bounds refresh on the owning spatial
node of each touched vertex/face (`update_nodes`; edge/corner/list
domains carry no node attr and are skipped). Both directions use the
same swap, since pre-step and post-step data exchange roles on every
flip. Before swapping, `ChunkElemData::swap` calls `updateSrcAttrMap`,
which re-resolves each stored column to its live mesh attribute **by
name** — so the element store survives attribute index changes between
capture and replay (a missing attr maps to `-1` and is skipped).

#### `LogChunkTopo`
Holds `Vector<LogElem*>` plus pools backing the elements and their
body snapshots, and two lookup maps:

* `idx_to_log_id` keyed by `(elem_kind << 32) | mesh_index` — used to
  detect repeat touches within the chunk. Removed at kill time so that
  reuse of a freed mesh index in the same chunk creates a *new*
  `LogElem` rather than colliding.
* `by_log_id` keyed by the log-local id, used by `dropRecord` and for
  general identity.

Replay walks the records **sorted by `log_id`** (`getSortedRecords`),
which is producer-event order — a valid replay order because mesh
topology operations fire their create/kill events in dependency-cascade
order (`kill_vertex` fires kill events for incident corners/edges before
releasing the vert; `make_face` fires create events for corners after
the underlying verts/edges already exist). Undo walks reverse, redo
forward. The records pool's iteration order is not relied upon — hence
the explicit sort.

**Spatial-tree ownership maintenance.** Restore uses raw
`ElemData::alloc` / `release` plus `ChunkElemRow` memcpys, bypassing
`make_face` / `kill_face` — so the tree's incremental face/vert
ownership (the `.spatial.{v,f}.node` TEMP attrs, which `ChunkElemRow`
does *not* log) is never updated by the restore itself. `undo`/`redo`
therefore bracket the element loop with tree passes (no-op when called
with no tree, e.g. the isolated operator tests):

* A **pre-pass** (run while the mesh is still in its pre-restore state,
  so connectivity is valid) calls `tree->remove_face` / `remove_vert`
  to drop ownership of faces/verts about to be released or rewired —
  otherwise a leaf keeps a dangling `unique_verts` ref that an
  index-reusing recreate would resurrect into a double-owned vert.
* A **post-pass** (run after the mesh is in its target state) calls
  `tree->add_face` to re-own faces that came back or were rewired,
  re-deriving the leaf and flagging it for tris/bounds/GPU regen. It
  guards with `f.node[...] == 0` so already-owned faces aren't
  double-added.

### `LogElem`
One record per logical element touched in a step. Classified by:

* **origin** — `Existed` (alive at step-begin) or `Created` (born
  during the step).
* **fate** — `Live` (survives to step-end) or `Dead` (killed during
  the step).

Carries up to two body snapshots:

* `begin_body` — pre-step state, captured at first touch. Populated
  for `Existed` records. Used to restore an `Existed && Dead` element
  during undo, and as the swap pivot for `Existed && Live`.
* `end_body` — post-step state, captured at `finalizeStep`. Populated
  for `Created && Live` records. Its connectivity columns freeze at the
  chunk's deactivation (so per-chunk redo can't reference geometry a
  later chunk creates), but `endStep` **refreshes the data columns**
  (co/no/…, every non-TOPO/non-NOCOPY attr) of still-live created verts
  from the final mesh (`refreshCreatedVertData`). Without this, a vert
  born in an early dab and then only brush-deformed (no connectivity
  touch) by later dabs would lose that displacement on redo — the brush
  save-gate keeps created verts out of the element store, so the topo
  chunk is their sole authority and its early-frozen `end_body` was
  stale. Dead/reused indices are skipped (their record is killed on
  redo anyway).

`Created && Dead` records (born and killed in the same chunk) are
**dropped at kill time** — they have no net effect on either
chunk-begin or chunk-end mesh state, so storing them would just be
work to undo later.

The four meaningful (origin, fate) cases drive `LogChunkTopo::undo` /
`redo`:

| origin / fate | undo | redo |
|---|---|---|
| Created && Live | `release(end_mesh_index)` | `alloc(end_mesh_index)` + `end_body->writeTo` |
| Existed && Live | `begin_body->swapWith(live)` | `begin_body->swapWith(live)` |
| Existed && Dead | `alloc(begin_mesh_index)` + `begin_body->writeTo` | `release(begin_mesh_index)` |
| Created && Dead | *(record dropped at kill time)* | *(record dropped at kill time)* |

### `detail::ChunkElemData`
Stores a fixed-size snapshot of a selected set of attributes for a
known number of rows, plus a built-in `origIndex` mapping each row
back to the mesh index it was captured from. Drives the swap used by
`LogChunkElems`. Handles `AttrType::BOOL` separately (bit-packed via
`BoolAttrView`); all other types are blitted with `memcpy` using
`elemSize` queried from the live `AttrData`. `updateSrcAttrMap` rebinds
the stored columns to live mesh attributes by name on every swap, so a
mid-history attribute reorder (or a vanished attr) is handled
gracefully — unlike `ChunkElemRow`.

### `detail::ChunkElemRow`
Single-row snapshot of *every* attribute (typed + bool, including
TOPO-flagged ones) in an `AttrGroup` for a given index. Used by
`LogChunkTopo`. The byte layout is computed once on `captureFrom` from
the live `AttrGroup`. **`NOCOPY` (TEMP) attributes are skipped** on
capture/restore — the `.spatial.*.node` ownership columns are derived
state owned by the spatial tree, not authoritative undo data, so
logging them would let incremental tree updates during a step taint
replay. `writeTo` / `swapWith` bound their loops to `count_` (the number
of attrs present at capture), so an `AttrGroup` that has attrs
*appended* between capture and replay stays safe — the new trailing
columns are simply not restored. **Reordering is still unsupported.** A
null `getElemData` (an unmaterialized / frozen-topo page) is warned and
skipped rather than memcpy'd through null.

## Per-dab topo chunks

A plain (non-dyntopo) topology step has a single topo chunk: the first
logged create/change/kill lazily pushes it via `getTopoChunk` and every
later event lands in the same chunk.

A **dyntopo** step is different — it holds **one `LogChunkTopo` per
dab**. The reason is end-state capture: `finalizeStep` snapshots each
`Created && Live` record's `end_body` from the *current* mesh. If all
dabs shared one chunk and we captured at end-of-step, a vert created by
dab 1 but rewired by dab 3 would be snapshotted with dab-3 connectivity
that references geometry only dab 3 created — so per-chunk redo would
re-own a face before its verts exist. Sealing each dab's chunk at the
dab boundary captures end-state while that dab's connectivity is still
the live truth.

The host drives this:

```
beginStep(hasDyntopo=true)     // pushes dab-0 topo chunk
  applyDynTopoDab(...)         // fires callbacks into the active chunk
  pushTopoChunk()              // finalize dab-0 chunk, open dab-1 chunk
  applyDynTopoDab(...)
  pushTopoChunk()              // finalize dab-1 chunk, open dab-2 chunk
  ...
endStep()                      // finalize the last still-active chunk
```

(`SculptPaintOp.applyDab` in the TS sculpt path and `script.cc` /
`brush_executor.h` on the debug-harness path both follow this; the
final dab's chunk is finalized by `endStep`, not a trailing
`pushTopoChunk`.)

On undo, the per-dab chunks replay newest-first; on redo, oldest-first,
each chunk fully applied before the next, so a face rewired across dabs
is re-owned only after its dab's chunk has recreated the verts it now
references.

## Step lifecycle

```
beginStep(hasDyntopo)
  ├── (hasDyntopo) pushTopoChunk()              → dab-0 topo chunk
  │
  ├── per dab:
  │     ├── mesh / dyntopo ops fire MeshCallbacks
  │     │     ├── onVertCreate / onEdgeCreate / … → LogChunkTopo::onCreate (+ stampUndoGate)
  │     │     ├── onVertChange / onEdgeChange / … → LogChunkTopo::onChange (begin-snapshot first touch, + stampUndoGate)
  │     │     └── onVertKill   / onEdgeKill   / … → LogChunkTopo::onKill
  │     ├── brush deform / paint passes (*Pre stage)
  │     │     └── AttrSaver-gated per-element append → LogChunkElems
  │     │         (dyntopo-moved verts are gated OUT by stampUndoGate)
  │     └── pushTopoChunk()                      → finalize this dab, open next
  │
endStep()
  └── finalize the last topo chunk (Created && Live end_body) → advance cursor → trimHistory
```

`undo()` decrements the cursor first, then replays the chunks of the
*now-current* entry backward. `redo()` replays the current entry
forward, then increments the cursor. Both thaw a frozen mesh first if
the entry holds a topo chunk.

## Integration with the brush executor

`brush::CommandExecutor` holds a `MeshLog*` and forwards
`beginStep(hasDyntopo)` / `endStep` to it. Deform/paint brushes' `*Pre`
stage stamps each touched element through an `AttrSaver` and appends its
pre-step row into `elemStore(domain)` — element-keyed, so it is correct
even when the spatial tree restructures mid-stroke. Topology / dyntopo
brushes must additionally:

1. Plug `meshLog->callbacks()` into the mesh operations they invoke
   (or call them on a mesh wired with that callback set).
2. Have `meshLog->setActiveMesh(mesh)` set so the topo chunk can
   snapshot attributes by index when create/change/kill fires.
3. Call `meshLog->pushTopoChunk()` after each dyntopo dab (the host
   sculpt op / debug harness does this; see "Per-dab topo chunks").

`beginStep` / `endStep` bracket the whole brush stroke (one step per
stroke).

## Design notes / pitfalls

* **`AttrType::WEIGHTS` rows hold references, not values.** A weights cell is a
  32-bit slot index into the mesh's `DeformPool` (`mesh/deform_pool.h`), so the
  raw byte copies this log is built on work unchanged — but a captured row keeps
  a slot *alive*. `RowLayout` therefore records the byte offsets of the weights
  cells (`weight_cells`, empty for the overwhelmingly common weightless mesh)
  and takes a `DeformPoolUser`; a row retains/releases through those offsets
  without re-walking a live `AttrGroup`, which it has none of at destruction
  time. `swapWith` needs no special case
  (the row and the mesh merely exchange slot indices, and the total reference
  count is unchanged). The pool outliving the mesh is the reason the reference
  is a *user* count rather than plain mesh ownership: a `Scene` frees its mesh
  in its body and destructs its log afterwards, so rows would otherwise be left
  naming a freed pool. `MeshLog::deformPoolMemSize` counts the pool into the
  undo budget once, rather than folding it into each chunk's `elemSize * rows`:
  the runs are one shared table, and the log is what keeps swept-out ones alive.

* **Active-mesh requirement.** Without `setActiveMesh`, the topo
  callbacks early-out (they have no way to read the mesh by index).
  Logged topology ops fired in that state will appear to undo as
  no-ops.
* **Element-store / topo-chunk overlap on dyntopo (the corruption
  bug).** A vert dyntopo moves is captured by the topo chunk (true
  pre-step body, on first callback touch) and *would* also be captured
  by the brush deform that runs after the remesh — but the deform's row
  holds the post-dyntopo value. Because the element store is older than
  later dabs' topo chunks, it would win the newest-first undo and
  re-corrupt the vert. `MeshLog::stampUndoGate` (called from the
  create/change callbacks) stamps those verts/faces into the brush's own
  `AttrSaver` gate (`vertGate_` / `faceGate_`, full flag mask) so the
  deform treats them as already-saved and skips them, leaving the topo
  chunk the sole authority. The gate columns are bound in
  `setActiveMesh` so stamping never allocates mid-stroke.
* **Attribute-group stability inside a step.** `ChunkElemRow`
  (topo chunk) captures a flat byte layout from the live `AttrGroup` at
  first touch. *Appending* attributes mid-step is safe (restore loops
  bound to `count_`); *reordering* them invalidates the layout. The
  `LogChunkElems` store is more robust — it remaps columns by name on
  every swap (`updateSrcAttrMap`) and so survives reordering too.
* **TEMP / NOCOPY attributes are not logged.** `ChunkElemRow` skips
  `NOCOPY`-flagged attrs (e.g. `.spatial.*.node`, `.strokeid.*`); they
  are derived state, and the spatial-tree ownership is instead
  maintained explicitly by the `add_face`/`remove_face`/`remove_vert`
  passes in `LogChunkTopo::undo`/`redo`.
* **Mesh index reuse.** Mesh freelists can hand back a killed index
  immediately. `LogChunkTopo` disambiguates by removing the
  `idx_to_log_id` mapping on kill; a subsequent create at the same
  index allocates a *new* `LogElem`. The two coexist in the records
  vector (kill-first, create-second) and replay correctly because
  their ops touch distinct logical elements.
* **Created && Dead dropping.** This is the set-theoretic
  consequence of "no net change to chunk-begin or chunk-end state", not
  an optimisation. The record-merging invariants depend on it.
* **Frozen-topology meshes.** `Mesh::freezeTopo` frees the live TOPO
  link-attr pages (`getElemData` returns null for them). Topo chunks
  restore elements with raw `alloc`/`release` plus `ChunkElemRow`
  memcpys, bypassing the auto-thawing topology mutators — so
  `undo`/`redo` call `thawForTopoChunks` first when the entry holds a
  topo chunk. This matters in practice: the brush executor re-freezes
  per-dab after `endDynTopoStroke()`, so undoing a *non-newest* dyntopo
  step always hits a frozen mesh. `ChunkElemRow` additionally warns and
  skips (rather than memcpying through null) if it ever sees an
  unmaterialized page. `LogChunkElems` only swaps brush-captured
  non-TOPO attributes and is safe on a frozen mesh, so plain-stroke
  undo never pays the O(mesh) thaw.
* **History cap evicts from the front.** `setMaxUndoSteps` /
  `trimHistory` `pop_front` the oldest committed steps, decrementing
  `curStep_` to match, and stop at `curStep_ == 0` so they never discard
  the current step or a pending redo. `freeStep` is the orthogonal,
  by-id eviction path used by the app's undo-memory limiter and only
  frees steps strictly behind the cursor.
* **Pool ownership.** `LogChunkTopo::records_pool` /
  `bodies_pool` own all `LogElem` and `ChunkElemRow` storage; the
  chunk's destructor relies on pool teardown rather than walking
  records individually.

## Bindings

`bindings::registerBindings` adds `MeshLog` to the binding manager.
`MeshLog::defineBindings()` exposes the default constructor plus:

* History driving — `beginStep(hasDyntopo)` / `endStep` / `undo` /
  `redo`, `pushTopoChunk` / `hasTopoChunk` (per-dab topo chunks), and
  `curStrokeId` (read by the brush deform's `AttrSaver`).
* Undo-memory accounting — `lastStepId` / `stepMemSize` / `totalMemSize`
  / `entryCount` / `freeStep`, consumed by the app tool stack's
  memory-limit enforcement.

`setActiveMesh`, `elemStore`, `getTopoChunk`, `pushReorderChunk`,
`setMaxUndoSteps`, and the chunk / `LogElem` types are intentionally not
exposed — they are internal scaffolding driven from C++ (the brush
executor and host sculpt op).
