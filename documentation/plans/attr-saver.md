# Plan: flesh out `AttrSaver` + per-brush save declarations

## Goal

Replace the brush undo-capture path with a per-element, hash-free "have we
already saved this element this stroke?" gate (`meshlog::AttrSaver`), and let
each brush declare in its `.sbrush` source *which* attributes it wants saved —
instead of the hardcoded `v.co` / `v.no` / `f.no` baked into every generated
`*Pre` stage.

## Why

Two defects in the current path (`emit_cpp.cc` `…Pre`, see
`generated/draw.brush.gen.h:11`):

1. **The gate is node-keyed and invalid under dyntopo.** Every `*Pre` does
   `if (ctx.meshLog->hasSimpleChunk(node->id)) continue;` as a proxy for "undo
   already stored this stroke." `node->id` is **not** stable across a stroke —
   the spatial tree rebalances / merges / splits (`add_face_at`, deferred leaf
   rebalance) mid-stroke. A vert can move to a node with no chunk (→ re-saved,
   capturing a *mid-stroke* value as if pre-stroke → corrupt undo) or a chunk
   can cover verts never saved (→ skipped). This is the corruption the
   `repro_dyntopo_undo*.txt` reproducers target.

2. **Every brush captures the same hardcoded layers.** The `*Pre` body is
   brush-independent: it always `ensureAttr`s `co`/`no`/`f.no`. `color` and
   `mask` brushes paint a layer that is **never captured** — their undo is
   wrong (it would "restore" unchanged positions and lose the painted values).
   Confirmed in `generated/color.brush.gen.h:20-22`.

A per-element stamp attribute (`AttrSaver`) fixes (1): the "already saved?"
state rides on the element, so it survives any tree restructuring. A `save`
declaration in the DSL fixes (2): the captured set is per-brush.

## Background (as-is)

- `source/meshlog/attr_saver.h` — rough skeleton. Stamps a per-domain builtin
  int attribute `.strokeid.<domain>` (`NOCOPY|TEMP|NOINTERP`): low 16 bits =
  stroke id, high 16 bits = per-attr saved-flags. `StandardUndoAttrs`
  (`CO/NO/COLOR/MASK`, custom from `1<<CUSTOM_START`). **It does not compile**:
  the single-arg `needsData`/`updateSaved` overloads call the 4-arg versions
  with 3 args (drop `curStrokeId`, wrong arg position) and ignore the result;
  `add()` is referenced in comments but absent; the `T` template param is
  unused.
- `source/meshlog/meshlog_base.h` — `LogChunkSimple` (per-node, dense: sizes
  `ChunkElemData` to the whole node and captures the fixed attr set for every
  element, touched or not). `ChunkElemData`/`ChunkElemRow` already swap by
  `origIndex` (element id), **not** by node — so the storage layer is already
  element-keyed; only the *gate* and the *which-attrs* decision are wrong.
- `source/brush/compiler/` — sbrush front-end. `parser.cc` brush-body loop
  (line 134) dispatches `uniform`/`ctx` → `parseField`, `attr` → `parseAttrField`;
  `ir.h` `Brush` holds `Vector<Field>`; `lexer.cc` keyword table (line 18).
  `emit_cpp.cc:1078-1109` emits the `*Pre` stage (the only place undo capture
  lives — undo is CPU-only; wgsl/cuda/opencl emitters emit no `Pre`).

## Design

### 1. `AttrSaver` is the gate only, transient per dab

Keep `AttrSaver` as pure logic over (a) the persistent mesh stamp column and
(b) a registered `{AttrRef, flag}` list. **No per-attr value storage** lives in
it — capture goes to the meshlog store (§3). It can be constructed on the stack
in `*Pre` each dab: `ensure(mesh)` is an idempotent bind/create of the existing
`.strokeid.<domain>` column, so transient lifetime is fine and the only
cross-dab state is the mesh column itself.

API (per domain `ElemType`):

```cpp
template <ElemType type> struct AttrSaver {
  void ensure(mesh::Mesh &m);                 // bind/create .strokeid.<domain>
  int  add(const mesh::AttrRef &ref, int flag); // register; returns flag
  bool needsData(int e, int curStrokeId, int flag) const;
  void updateSaved(int e, int curStrokeId, int flag);
  void resetElem(int e);                       // clear stamp (element re-created)
};
```

- Fix the bit math already present (`needsData`/`updateSaved` 4-arg bodies are
  correct; delete the broken AttrRef-keyed overloads or reimplement them to loop
  the registered list and OR all flags).
- Drop the unused `T` template param.
- `add()` maps a save entry to a `StandardUndoAttrs` bit **by save-list
  position / category** (`co→CO`, `no→NO`, `color→COLOR`, `mask→MASK`, custom
  handles from `1<<CUSTOM_START` up) — **not** by the resolved mesh-layer name,
  which varies per stroke under layer overrides (§7). The stamp bit means "this
  brush's color-category capture is done this stroke"; the *bytes* are captured
  from whatever layer the override resolved to.
- **v1 capture granularity: all-registered-attrs-together per element.** Use one
  combined flag mask for the domain's registered set (matches today's
  "save the whole element once" semantics, one stamp read/write per element).
  Keep the per-attr flag plumbing in place for the future anchored-brush mode,
  but codegen saves the set as a unit.

### 2. Stroke id from `MeshLog`

Add a monotonically increasing stroke id to `MeshLog`, bumped when a new undo
step/entry begins (a stroke pushes one step). Expose `int MeshLog::curStrokeId()`
**masked to 16 bits — accepted by design** (only equality against the stamp
within a step matters; the 65536-stroke wrap is harmless and needs only a
one-line comment, no widening). On the **first**
dab of a step the stamp's stored id won't match → everything saves; subsequent
dabs match → skip. No explicit clear needed.

### 3. Per-step element store (replaces the dense per-node `LogChunkSimple`)

The gate decouples "should I save element e?" from "where the bytes go." Replace
the node-keyed dense capture with a **per-step, append-as-touched** element store
per domain: when `needsData` is true, append one `ChunkElemRow` (element id +
the registered attr bytes) and `updateSaved`. `ChunkElemRow`/`ChunkElemData`
already undo/redo by `origIndex`, so the existing swap machinery is reused; only
the *population* changes from "dense, sized to node" to "sparse, grown as verts
are touched."

- Add a growable element store reachable from `*Pre` via `ctx.meshLog`
  (e.g. `meshLog->elemStore(domain)` returning the current step's store,
  created lazily). Internally it can wrap `ChunkElemData` with append, or a new
  `LogChunkElems` chunk type — prefer extending `ChunkElemData` with an
  `appendFrom(src, srcIdx, registeredRefs)` to minimize new code.
- Undo/redo replay is unchanged (swap by `origIndex`). This keeps the win that
  capture is element-keyed and dyntopo-safe.

### 4. DSL: `save` declaration

Syntax (brush-body statement, repeatable):

```
save vertex co, no;
save face   no;
save vertex color;     // names a builtin layer or a declared `attr` field
```

- **Lexer** (`lexer.cc:18`, `lexer.h:29-33`, `tokKindName`): add
  `{"save", TokKind::KwSave}`.
- **IR** (`ir.h`): `struct SaveAttr { AttrDomain domain; string name; };` and
  `Vector<SaveAttr> saves;` on `Brush`.
- **Parser**: brush-body loop (`parser.cc:134`) dispatches `KwSave` →
  `parseSaveDecl`: parse domain keyword (reuse the vertex/face/edge/corner match
  from `parseAttrField`), then a comma list of names, `;`. Name→AttrRef
  resolution (builtin `co/no/color/mask` vs an `attr` field's bound layer) is
  deferred to codegen.
- **Default / back-compat:** when `saves` is empty, emit the current default
  (`vertex co`, `vertex no`, `face no`) so every existing brush is byte-identical
  after regen. Brushes needing more (color, mask) add explicit `save` lines.

### 5. Codegen (`emit_cpp.cc` `*Pre`)

Rewrite `emit_cpp.cc:1084-1108`. Remove the `hasSimpleChunk` gate and the dense
`getSimpleChunk`/`ensureAttr`/`cpyFrom` block. Emit instead:

```cpp
if (ctx.meshLog) {
  meshlog::AttrSaver<ElemType::VERTEX> vsaver; vsaver.ensure(*m);
  int vmask = 0;
  vmask |= vsaver.add(m->v.co, meshlog::CO);   // one per resolved vertex SaveAttr
  vmask |= vsaver.add(m->v.no, meshlog::NO);
  auto *vstore = ctx.meshLog->elemStore(ElemType::VERTEX);
  int sid = ctx.meshLog->curStrokeId();
  for (auto *node : nodes)
    for (int v : node->unique_verts())
      if (vsaver.needsData(v, sid, vmask)) { vstore->appendFrom(m->v.attrs, v, /*refs*/); vsaver.updateSaved(v, sid, vmask); }
  // …same for the FACE domain when face saves are present…
}
```

- Resolve each `SaveAttr` (§6 is load-bearing here):
  - **builtin bundle fields** (`co`, `no`, `mask` — accessed as `Vertex`/`Face`
    members, not declared `attr` fields) → `m->v.co` / `m->v.no` / `m->v.mask`
    directly. These have no layer override.
  - **declared `attr`-field handles** (e.g. `color`) → the **resolved bound
    ref** via `ctx.boundAttr<T>(name)`, reusing the resolution already emitted
    for the vertex stage (`emit_cpp.cc:1164-1178`). This ref already reflects
    any `BrushAttrLayerOverride` because `exec()` populates `ctx.attrBindings`
    before calling `execPre` (§6). Capturing by `m->v.<name>` or by the declared
    layer name would snapshot the **wrong layer** when an override is active.
  - The store (`appendFrom`) records the resolved ref's real layer **name**, so
    undo replay (swap-by-`origIndex`-and-name) targets the same overridden
    layer.
- Only `emit_cpp` changes. The wgsl/cuda/opencl emitters ignore `saves`
  (document: undo capture is CPU-only). Add an assertion/skip if a brush sets
  `saves` but those backends are asked to emit — no-op is fine.

### 6. `BrushAttrLayerOverride` semantics (load-bearing)

`brush_executor.h` lets the TS attribute manager redirect a declared `attr`
handle to the user-selected "active" layer of its category (color / poly-group /
UV) — `BrushAttrLayerOverride{attrIdx, layerIndex}`, applied in `exec()`
(`brush_executor.h:440-457`). Critically, `exec()` resolves these into
`ctx.attrBindings` (handle → resolved `AttrRef`) **before** it calls
`cmd.execPre(ctx, nodes)` (`brush_executor.h:486-491`). So by the time the
`*Pre` undo capture runs, the override is already baked into what
`ctx.boundAttr<T>(handle)` returns.

Requirements this imposes on the save path:

1. **Capture the resolved bound ref, not the declared one.** A `save vertex
   color;` must snapshot whatever layer the override resolved `color` to (e.g.
   the active "Col.002"), not a layer literally named `color`. Use
   `ctx.boundAttr<T>(name)` for `attr`-field saves; never `m->v.<name>` or
   `grp->ensure(type, declaredName)` for an override-eligible handle.
2. **Stamp bit is per-category, layer-name-independent.** `AttrSaver::add`
   assigns the `COLOR`/custom bit by handle/category (§1), so "saved this
   stroke?" is correct even though the underlying layer name differs per stroke.
   Within a single stroke the override is stable (resolved per dab, identical
   across the stroke's dabs), so one bit per category is sound.
3. **The store records the resolved layer's name**, so undo/redo
   (`ChunkElemRow` swap by `origIndex` keyed on attr name) restores the
   overridden layer, not a same-typed sibling.
4. **Mismatch fallback parity.** When an override index is stale/wrong-typed,
   `exec()` falls through to the default by-name binding (`brush_executor.h:447`).
   The save path inherits this automatically by reading `ctx.attrBindings` rather
   than re-resolving the override itself — do **not** duplicate the override
   lookup in generated code.

### 7. Hand-written mirror

`source/debug/gpu_stroke.cc:541` `snapshotNode` uses the same `hasSimpleChunk`
node gate. Port it to the element-keyed `AttrSaver` + `elemStore` path (or route
it through a shared helper extracted from the generated code) so the GPU-stroke
debug path and the generated CPU path agree.

## Phasing

1. **AttrSaver core** — make `attr_saver.h` compile; unit test the stamp/flag/
   stroke-increment logic (`tests/test_attr_saver.cc`). No codegen impact.
2. **MeshLog stroke id + element store** — add `curStrokeId()`, bump on new
   step; add `elemStore(domain)` + `ChunkElemData::appendFrom`. Keep the old
   `*Pre` working against it (or behind a flag) to isolate the change.
3. **DSL `save`** — lexer/parser/IR; empty-`saves` default reproduces co/no/f.no
   exactly. Regenerate (`node make.mjs codegen`) → zero diff in generated files.
4. **Rewrite `*Pre` codegen** — drive `AttrSaver`+`elemStore` from `saves`;
   delete the `hasSimpleChunk` gate; regen + rebuild; port `gpu_stroke.cc`.
5. **Port brushes** — add `save vertex color;` to `color.sbrush`,
   `save vertex mask;` to `mask.sbrush`, etc. Verify color/mask undo now works
   (it does not today).
6. **Cleanup** — **fully retire `LogChunkSimple`** (decided): delete the
   struct, `hasSimpleChunk`/`getSimpleChunk`, and `LogChunkTypes::Simple`; the
   per-step element store (§3) is its sole replacement. The dyntopo topo-chunk
   overlap path (`meshlog_base.h:1283-1289`) must be re-expressed against the
   element store — verify the "topo chunk holds the true pre-step position,
   simple chunk a mid-stroke one, replay newest-first" ordering still holds when
   the simple chunk becomes the flat element store (the store is created after
   the topo chunk within a step, so reverse-creation replay order is preserved).
   Strip `CLAUDENOTE:`s; update `documentation/dynamic-topology.md` undo notes.

## Tests / gates

- `test_attr_saver.cc` (new): stamp packing, per-attr flags, stroke increment
  clears, `resetElem`.
- Dyntopo + undo: the `repro_dyntopo_undo*.txt` scenarios must restore
  pre-stroke positions with the tree restructured mid-stroke.
- `tests/integration/sculptcore_boundary.test.ts` (boundary invariance under
  dyntopo + both undo stacks) and `sculptcore_parity.test.ts` must stay green.
- New: a color/mask paint-then-undo behavior test (proves the §Why-2 fix).
- `node make.mjs codegen` after each DSL/emit change; `node make.mjs test`
  (ctest) for the native gates.

## Decisions

- **16-bit stroke id: accepted.** No widening; equality-within-step only, wrap
  harmless (§2).
- **`LogChunkSimple` is fully retired**, not kept. The per-step element store
  (§3) replaces it outright, including the dyntopo topo-chunk overlap path —
  Phase 6 re-expresses that overlap against the element store and verifies the
  reverse-creation replay order.

## Open questions

- Edge/corner save domains: wire the keywords now (cheap) but no current brush
  needs them — leave codegen for vertex/face only until a consumer appears.
