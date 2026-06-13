# Meshlog — High-level overview

The meshlog subsystem (`source/meshlog/`) is the sculpt undo/redo log.
It records mesh edits as a sequence of *steps*, each step composed of
one or more *chunks*; the chunks carry just enough state to swap the
live mesh back to its pre-step form (undo) and forward again (redo).

Two chunk types coexist in one step:

* **`LogChunkSimple`** — per-spatial-node attribute swap. Captures a
  fixed set of attributes for the verts/edges/corners/faces touched
  inside one `spatial::SpatialNode` once at first touch, and swaps them
  with live on each undo/redo. Used by position-only brushes where
  topology does not change.

* **`LogChunkTopo`** — full topological log. Driven by
  `mesh::MeshCallbacks`, it records every create / change / kill event
  fired by mesh ops during the step and replays them in order (redo)
  or reverse order (undo). Used by brushes that add or remove geometry
  (split, collapse, dissolve, …).

`MeshLog` owns the chunks, exposes a callback object that the mesh
operations fire into, and provides `beginStep` / `endStep` /
`undo` / `redo` to the brush executor.

## File map

| File | Role |
|---|---|
| `meshlog.h` | Umbrella — includes `meshlog_base.h`. |
| `meshlog_base.h` | All types: `MeshLog`, `LogEntry`, `LogChunk`, `LogChunkSimple`, `LogChunkTopo`, `LogElem`, `detail::ChunkElemData`, `detail::ChunkElemRow`, the `LogElemKind` / `LogOrigin` / `LogFate` enums. |
| `bindings.h` / `bindings.cc` | `registerBindings(BindingManager&)` — exposes `MeshLog` to the litestl reflection layer (default constructor + `beginStep` / `endStep` / `undo` / `redo`). |
| `CMakeLists.txt` | Static library `meshlog`, depends on `util`, `math`, `props`, `spatial`, `mesh`. |

## Core types

### `MeshLog`
The top-level log object. Holds a `Vector<LogEntry>` (one entry per
step) and a cursor `curStep_`. Notable members:

* `callbacks()` — returns the `mesh::MeshCallbacks*` that mesh
  topology ops must fire into so events land in the current topo
  chunk. The callback object is owned by `MeshLog`; mesh code receives
  a borrowed pointer.
* `setActiveMesh(mesh::Mesh*)` — must be called before logged ops run,
  because the topo chunk snapshots attributes by mesh index at the
  moment of first touch and at kill.
* `beginStep()` — appends a fresh `LogEntry` (stamped with a
  monotonic step id) and clears the cached topo chunk pointer. If the
  cursor is mid-history (after some undos), the future entries are
  discarded first.
* `lastStepId()` — id of the most recently begun step; the host reads
  it right after `beginStep` to key the step for `stepMemSize` /
  `freeStep`.
* `stepMemSize(id)` / `totalMemSize()` / `entryCount()` — undo-memory
  accounting: estimated heap bytes retained by one step (0 if freed)
  or by the whole history, and the live entry count.
* `freeStep(id)` — evicts a committed step (undo-memory limit
  enforcement from the app's tool stack). Only steps strictly behind
  the cursor are freeable; returns 1 if a step was freed.
* `endStep()` — calls `LogChunkTopo::finalizeStep` to take end-state
  snapshots for newly-created live elements, then advances the cursor.
* `getSimpleChunk(nodeId, vcount, ecount, ccount, fcount)` —
  lazily allocates (or returns the existing) `LogChunkSimple` for the
  given spatial node in the current step.
* `getTopoChunk()` — lazily allocates (or returns) the one topo chunk
  for the current step.
* `undo(Mesh*, SpatialTree*)` / `redo(Mesh*, SpatialTree*)` — walk the
  current entry's chunks in their natural order; the chunks themselves
  decide direction. If the entry holds a topo chunk and the mesh is
  topology-frozen, the mesh is thawed first (see pitfalls below).

### `LogEntry`
A step's container. Owns `Vector<LogChunk*>` and `Delete`s the chunks
in its destructor. Currently a flat vector; chunks within an entry are
independent and order-insensitive.

### `LogChunk`
Polymorphic base with `type` tag, virtual `undo` / `redo`. Two
concrete subclasses:

#### `LogChunkSimple`
Holds four `detail::ChunkElemData` blocks (one per element kind:
verts, edges, corners, faces) and a `nodeId`. Each block stores a
snapshot of a fixed attribute selection plus a built-in `.sculpt.origIndex`
that maps each chunk-local row back to its mesh index. `undo()` /
`redo()` swap each block with the live `AttrGroup` and request a GPU /
bounds refresh on the spatial node. Both directions use the same
swap, since pre-step and post-step data exchange roles on every flip.

#### `LogChunkTopo`
Holds `Vector<LogElem*>` plus pools backing the elements and their
body snapshots, and two lookup maps:

* `idx_to_log_id` keyed by `(elem_kind << 32) | mesh_index` — used to
  detect repeat touches within the step. Removed at kill time so that
  reuse of a freed mesh index in the same step creates a *new*
  `LogElem` rather than colliding.
* `by_log_id` keyed by the log-local id, used by `dropRecord` and for
  general identity.

The replay order is *producer-event order*: append-as-they-arrive.
This is a valid replay order because mesh topology operations fire
their create/kill events in dependency-cascade order
(`kill_vertex` fires kill events for incident corners/edges before
releasing the vert; `make_face` fires create events for corners
after the underlying verts/edges already exist). Undo walks the
records in reverse; redo walks them forward.

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
  for `Created && Live` records.

`Created && Dead` records (born and killed in the same step) are
**dropped at kill time** — they have no net effect on either
step-begin or step-end mesh state, so storing them would just be
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
`LogChunkSimple`. Handles `AttrType::BOOL` separately (bit-packed via
`BoolAttrView`); all other types are blitted with `memcpy` using
`elemSize` queried from the live `AttrData`.

### `detail::ChunkElemRow`
Single-row snapshot of *every* attribute (typed + bool, including
TOPO-flagged ones) in an `AttrGroup` for a given index. Used by
`LogChunkTopo`. The byte layout is computed once on `captureFrom`
from the live `AttrGroup`; subsequent `writeTo` / `swapWith` assume
the group has not had attrs added or reordered in between.

## Step lifecycle

```
beginStep()
  ├── mesh ops fire MeshCallbacks
  │     ├── onVertCreate / onEdgeCreate / …    → LogChunkTopo::onCreate
  │     ├── onVertChange / onEdgeChange / …    → LogChunkTopo::onChange  (begin-snapshot first touch)
  │     └── onVertKill   / onEdgeKill   / …    → LogChunkTopo::onKill
  │
  ├── brush position passes
  │     └── per-node attribute swap            → LogChunkSimple
  │
endStep()
  └── LogChunkTopo::finalizeStep — capture end_body for Created && Live records
```

`undo()` decrements the cursor first, then replays the chunks of the
*now-current* entry backward (via each chunk's own ordering). `redo()`
replays the current entry forward, then increments the cursor.

## Integration with the brush executor

`brush::CommandExecutor` holds a `MeshLog*`. Position brushes call
`getSimpleChunk(nodeId, …)` per touched spatial node and let
`LogChunkSimple` snapshot the affected attributes. Topology brushes
must additionally:

1. Plug `meshLog->callbacks()` into the mesh operations they invoke
   (or call them on a mesh wired with that callback set).
2. Have `meshLog->setActiveMesh(mesh)` set so the topo chunk can
   snapshot attributes by index when create/change/kill fires.

Both happen during step setup; `beginStep` / `endStep` bracket the
brush stroke.

## Design notes / pitfalls

* **Active-mesh requirement.** Without `setActiveMesh`, the topo
  callbacks early-out (they have no way to read the mesh by index).
  Logged topology ops fired in that state will appear to undo as
  no-ops.
* **Attribute-group stability inside a step.** `ChunkElemRow`
  captures a flat byte layout from the live `AttrGroup` at first
  touch. Adding or reordering attributes mid-step invalidates the
  layout. Logged ops assume attribute schemas are constant for the
  duration of a step.
* **Mesh index reuse.** Mesh freelists can hand back a killed index
  immediately. `LogChunkTopo` disambiguates by removing the
  `idx_to_log_id` mapping on kill; a subsequent create at the same
  index allocates a *new* `LogElem`. The two coexist in the records
  vector (kill-first, create-second) and replay correctly because
  their ops touch distinct logical elements.
* **Created && Dead dropping.** This is the set-theoretic
  consequence of "no net change to begin- or end-state", not an
  optimisation. The record-merging invariants depend on it.
* **Frozen-topology meshes.** `Mesh::freezeTopo` frees the live TOPO
  link-attr pages (`getElemData` returns null for them). Topo chunks
  restore elements with raw `alloc`/`release` plus `ChunkElemRow`
  memcpys, bypassing the auto-thawing topology mutators — so
  `undo`/`redo` call `thawForTopoChunks` first when the entry holds a
  topo chunk. This matters in practice: the brush executor re-freezes
  per-dab after `endDynTopoStroke()`, so undoing a *non-newest* dyntopo
  step always hits a frozen mesh. `ChunkElemRow` additionally warns and
  skips (rather than memcpying through null) if it ever sees an
  unmaterialized page. `LogChunkSimple` only swaps brush-captured
  non-TOPO attributes and is safe on a frozen mesh, so plain-stroke
  undo never pays the O(mesh) thaw.
* **Pool ownership.** `LogChunkTopo::records_pool` /
  `bodies_pool` own all `LogElem` and `ChunkElemRow` storage; the
  chunk's destructor relies on pool teardown rather than walking
  records individually.

## Bindings

`bindings::registerBindings` adds `MeshLog` to the binding manager.
`MeshLog::defineBindings()` exposes the default constructor plus
`undo` / `redo` / `beginStep` / `endStep` (driving sculpt history) and
`lastStepId` / `stepMemSize` / `totalMemSize` / `entryCount` /
`freeStep` (undo-memory accounting, consumed by the app tool stack's
memory-limit enforcement). Chunks and `LogElem` are intentionally not
exposed — they are internal scaffolding.
