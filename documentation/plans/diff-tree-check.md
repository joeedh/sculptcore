# Plan: undo-vs-rebuild diff debug check (localize the dyntopo redo bug)

## Context

Dyntopo undo/redo is broken in the no-rebuild paths: **redo hangs in the
Electron app**, and `LogChunkTopo` undo "does nothing" interactively. The
topology restore itself (mesh side) is correct — the fault is in the
**incrementally-maintained spatial tree** that `LogChunkTopo::undo`/`redo` drive
via `tree->remove_face`/`remove_vert` (pre-pass) and `tree->add_face`
(post-pass) so the tree stays current without a full rebuild (the M7.6 design
the 5M-tri dyntopo perf depends on).

The bug is invisible to tests because the script/parity harness **masks** it: the
`undo`/`redo` script verbs call `scene.tree->rebuild()` right after
`meshLog.undo/redo` (`script.cc:991`, `:1000`), reconstructing the tree from
scratch and overwriting whatever the incremental ops left behind. Electron and
the interactive debug path call undo/redo with **no** rebuild, so the corrupt
incremental tree survives and a later tree walk loops forever (the hang).

This plan wires a **debug-only consistency check** into the debug-app `undo`/`redo`
verbs that: (1) removes the masking rebuild so the incremental tree persists and
cascades exactly as in Electron; (2) after every undo/redo, verifies the
incremental tree's face/vert ownership against the live mesh; (3) on the first
divergence, runs a throwaway `tree->rebuild()` as a **positive-control oracle** to
prove the same mesh yields a consistent tree — pinning the bug to the incremental
path and naming the exact undo/redo step + element that first corrupts it. The
existing `redo 1/2/3` / `UNDO-DIAG` printfs are kept to localize a true
infinite-loop hang (the case where the check never gets to run).

## Key findings (verified, file:line)

- **Liveness predicate** (`spatial.cc` `buildAll`/`rebuild`): `for (int f : m->f)`
  / `for (int v : m->v)` iterate only LIVE elements — the `ElemData` iterator
  (`mesh/elem_data.h:48`) skips `freemap[i]` slots. Test one element with
  `m->f.freemap[f]` / `m->v.freemap[v]` (true = dead). Capacity (max index, incl.
  free) = `m->f.capacity()` / `m->v.capacity()`; live count = `m->f.count` /
  `m->v.count`.
- **Ownership arrays**: `tree->treeMesh.f.node[i]` / `tree->treeMesh.v.node[i]`
  (`spatial_attrs.h`, `BuiltinAttr<int>`), 0 = unowned else owner leaf id. Safe to
  index over `[0, capacity)` (backed by full-capacity pages; free slots may hold
  stale ids). `treeMesh` is **public** (`spatial.h:76`).
- **Per-leaf sets**: `leaf->data->unique_faces` / `unique_verts`
  (`OrderedSet<int>`, `node.h:107`). Walk leaves via `tree->leaves()`
  (`spatial.h:521`). Resolve an id with the public, bounds-checked
  `tree->node_from_id(id)` (`spatial.h:285`).
- **`rebuild()` restarts `node_idgen = 1`** and re-partitions
  (`spatial.cc` rebuild, ~`:1245`). ⇒ leaf ids and the leaf→element partition are
  **NOT comparable** across incremental vs rebuild. The diff must be
  **partition-independent**: compare owned *sets* and per-element consistency, not
  "which leaf owns what" or a literal `node_idmap` compare.
- **Reusable template**: `tests/test_spatial_dyntopo.cc:52` `validateOwnership()`
  already encodes the right invariants (leaf sets vs `f.node`/`v.node`, dead-owned,
  unowned-live, coverage == count). Adapt it into a string-returning helper. (It
  lives in a test TU not linked into debug_app, so copy the logic.)
- **Harness**: verbs dispatch through
  `bool execVerb(Scene&, verb, ArgMap& args, out_dir, std::string& err)`
  (`script.cc:210`); return `false` + set `err` → script halts, `debug_app`
  exits 1 (`debug_app.cc:153-155`). `Scene` exposes `mesh` (`Mesh*`), `tree`
  (`SpatialTree*`), `meshLog` (`MeshLog`). Arg helpers: `getBool/getInt/getArg`
  (`script.cc:100`). Pattern to mirror for reporting: `assert_aabb`/`assert_manifold`
  (`script.cc:942-978`).

## Implementation

All changes are in **`source/debug/script.cc`** (debug-only harness). No core/spatial
edits.

### 1. Helper: `firstTreeDivergence(SpatialTree* tree, Mesh* m, std::string& msg) -> bool`

Returns `true` (consistent) or `false` with `msg` describing the **first**
divergence. Partition-independent checks, in order:

1. **Stale-owned (dead element still owned)** — scan `f` in `[0, m->f.capacity())`:
   if `m->f.freemap[f] && treeMesh.f.node[f] != 0` → `"dead face F still owned by
   leaf L"`. Same for verts. *(Leading hang suspect: a killed face/vert whose
   index gets reused while the tree still owns the old slot.)*
2. **Dangling owner id** — for any owned `f`/`v`, `node_from_id(node[f])` must be a
   live leaf with `data`; else `"face F owner id N resolves to no live leaf"`.
3. **Leaf set ↔ array agreement** — for each leaf, every `f` in `unique_faces`
   must satisfy `!freemap[f]` and `treeMesh.f.node[f] == leaf->id`; else report the
   offending `f`/leaf. Same for `unique_verts`.
4. **Coverage** — every live face (`for f : m->f`) has `treeMesh.f.node[f] != 0`
   (`"live face F unowned (dropped)"`); `sum(leaf.unique_verts.size()) == m->v.count`
   (and same face/count) else report the totals.

Report format includes kind, index, owner id, and which invariant — enough to pin
the element and the failure mode.

### 2. Helper: `checkTreeVsRebuild(Scene& scene, const char* tag, std::string& err) -> bool`

```
std::string incMsg;
if (firstTreeDivergence(scene.tree, scene.mesh, incMsg)) return true; // clean, NO rebuild
// incremental diverged -> run the oracle
scene.tree->rebuild();
std::string rebMsg;
bool rebOk = firstTreeDivergence(scene.tree, scene.mesh, rebMsg);
err = "incremental tree diverged after " + tag + ": " + incMsg +
      " | rebuild oracle: " + (rebOk ? "CONSISTENT -> bug is in incremental undo/redo"
                                     : "ALSO INCONSISTENT: " + rebMsg + " -> mesh-level");
return false;
```

On a clean step it does **not** rebuild, so the incremental tree persists and
cascades into the next undo/redo — the faithful Electron repro. The oracle rebuild
runs only at the first failure (where we halt anyway).

### 3. Rewrite the `undo` / `redo` verbs (`script.cc:979-1003`)

Drop the unconditional `scene.tree->rebuild()` (the mask). Keep `thawTopo()` and
the existing `UNDO-DIAG` / `redo 1/2/3` printfs (they localize a true hang). After
`meshLog.undo/redo`, call the check and propagate failure:

```
if (verb == "redo") {
  if (scene.mesh && scene.tree) {
    scene.mesh->thawTopo();
    scene.meshLog.redo(scene.mesh, scene.tree);   // redo 1/2/3 printfs stay
    if (!checkTreeVsRebuild(scene, "redo", err)) return false;  // replaces rebuild()
  }
  return true;
}
```
(Same shape for `undo`.) A divergence now halts the script with the first-divergence
message and exit code 1, exactly like `assert_*`.

### 4. (Optional) `check_tree` verb

A no-op-when-clean verb that runs `checkTreeVsRebuild(scene, "check_tree", err)` so a
repro can assert tree consistency at arbitrary points. Cheap to add; nice for
bisecting. Not required for the core task.

## Why not a literal leaf/`node_idmap` diff

`rebuild()` restarts `node_idgen` at 1 and re-descends the partition, so a face
legitimately owned by leaf 7 incrementally may be owned by leaf 3 after rebuild —
not a bug. Only partition-independent facts are meaningful: the *set* of owned
live elements, no dead element owned, no dangling owner id, and leaf-set ↔ array
agreement. Those are exactly the checks above; `rebuild()` is used as a
positive-control oracle, not for element-by-element id comparison.

## Verification / execution

1. **Build**: `cd C:/dev/webgl-app-framework-binder-trait/sculptcore/build/native &&
   node ../../configureEnv.mjs cmake --build . --target debug_app -- -k 0`.
2. **Run the repros** headless (confirm the `debug_app` binary path under
   `build/native/`), from `sculptcore/`:
   `<debug_app> --script repro_dyntopo_undo.txt --headless --out /tmp/undo_out`,
   then `repro_dyntopo_undo2.txt`.
3. **Interpret**:
   - *Check fires* → the first-divergence line names the undo/redo step, element
     kind+index, and failure mode; the oracle verdict confirms it's the incremental
     path. That is the bug source.
   - *Genuine hang* (no check output) → the last printed `redo 1/2/3` / `UNDO-DIAG`
     marker localizes the segment; inspect that loop. Leading suspects:
     `meshlog_base.h` redo pre-pass Created-skip asymmetry (`:842-843`) and the
     self-flagged `// XXX do we want to remove faces here?` (`:847`).
4. **Fix** the identified incremental op in `LogChunkTopo::undo`/`redo`
   (`meshlog_base.h`), re-run both repros until the check passes (no rebuild).
5. **No-regression**: `ctest` in `build/native`, plus the three integration gates
   (`sculptcore_parity`, `sculptcore_brushes`, `sculptcore_boundary` under
   `pnpm test`).
6. **Cleanup (after the fix)**: per the carried strip-list, remove the diag
   scaffolding (`redo 1/2/3`, `UNDO-DIAG`, `SPATIAL-DIAG`, `CK`/`check_all`,
   `getSimpleChunk` printf, etc.). Decide: keep `checkTreeVsRebuild` behind a debug
   flag as a permanent regression net, **or** promote the invariant into the
   integration tests via the existing `validateOwnership` (follow-up — fold the
   check into the parity boot path so `pnpm test` catches a future regression
   without the masking rebuild).

## Follow-ups (out of scope here, noted)

- Decouple `applyDynTopoDab`'s `log` flag (callback-wiring vs step-lifecycle) so
  **interactive** records topology into the stroke's one step (fixes "undo does
  nothing" interactively); merge the script's two-steps-per-stroke into one.
- Promote the consistency check into `pnpm test` (remove the masking rebuild from
  the parity path) so this class of bug can't regress unnoticed.
