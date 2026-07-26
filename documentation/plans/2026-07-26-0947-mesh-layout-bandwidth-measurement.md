# Mesh-layout memory-bandwidth measurement

## Status

Not started. This plan produces **measurement data only** — no mesh variant is
implemented here. Its output is the evidence needed to decide whether to build
custom mesh structure variants (triangle-only; manifold-without-holes) at all,
and to justify that decision to other people.

## Motivation

We are considering mesh structure variants in sculptcore that reuse the paged
attribute system but carry different topology columns and their own mutation
operators. Two candidates:

- **Triangle-only** — LIST domain gone, corners implicit (`c = f*3 + i`),
  `f.vs` as a contiguous `int3`.
- **Manifold, no face holes** — `f.list_count` and the `l.next` hole chain gone.

The justification is **memory bandwidth, not footprint.** At 1.5–5 M elements
nothing fits in cache under any layout, so this is a streaming workload and
total bytes stored is close to irrelevant. What governs cost is bytes actually
pulled through the hierarchy per dab, which is driven by:

- **Stream count** — each distinct array touched per element visit is another
  concurrent stream competing for L2, TLB entries, and prefetcher slots.
- **Line utilization** — reading `l.size[li]` for one 4-byte int costs a full
  64-byte line. If list ids are not dense in walk order that is ~6% useful.
- **Dependent-load depth** — `c.l[cc]` → `l.f[li]` → `f.list_count[fi]` is three
  serialized round trips; latency-bound, and prefetch cannot help.

Concretely, the radial-triangle walk in dyntopo touches **eight distinct arrays**
per step (`e.c`, `c.l`, `l.size`, `l.f`, `f.list_count`, `c.next`, `c.v`,
`c.radial_next`) and is written out five times: `measureRoundQuality`
(`dyntopo.h:317-339`), `flipQuad` (`:411-436`), `smoothTangent` (`:485-512`),
`featureCollapseOk` (`:633`), `considerVertFaces` (`:747-755`). Under a
triangle-only layout the same walk touches two or three.

## Established context (not re-measured here)

The locality-sensitivity question — *does memory behavior matter in this hot
path at all* — was settled during the defrag work. `auto_defrag` is default-on
precisely because scattered slots cost real time, and the Phase-0 table in
[`defrag-scoped-compaction.md`](defrag-scoped-compaction.md) quantifies it. **We
do not re-run a fragmented-vs-compacted A/B.** This plan takes as given that the
hot path is memory-sensitive and moves on to the cross-layout question, which
fragmentation cannot answer (frag varies locality *within* a layout; it does not
vary stream count or line utilization).

## Non-goals

- No `TriMesh` / half-edge implementation, no operator work, no concept extraction.
- No footprint / total-bytes model. Deliberately dropped — wrong metric.
- No fragmentation A/B (see above).
- No changes to shipping build configs. The instrument is compile-time gated off.

## The metric

**Distinct 64-byte cache lines demanded per dab, attributed per array, with a
per-line byte-utilization mask.**

A cache line is 64 bytes; addresses sharing `addr >> 6` are fetched together.
Recording `addr >> 6` for every element read and counting distinct values gives
the lines that dab demanded. The byte mask gives what fraction of each fetched
line was actually used.

The value of this over hardware counters as the *primary* number: it is a
property of access pattern and layout, not of one machine. Same mesh plus same
stroke gives the same number on any CPU, every run — so it is a regression gate,
it is attributable per array, and it supports counterfactual derivation.

### Modeling assumption (must be stated in every report)

Resetting the line set per dab assumes an empty cache at dab start and an
infinite one during the dab. So the number is **compulsory traffic — a lower
bound** on bytes moved, not a DRAM-traffic claim. This is the right framing for a
layout comparison (layout acts directly on compulsory traffic) and it is
defensible without arguing about anyone's cache hierarchy, but it must be
labelled or readers will over-read it.

---

## Phase 1 — the instrument (`WITH_ATTR_TRACE`)

A compile-time-gated address tracer. Follows the existing option pattern
(`WITH_ASAN` at `CMakeLists.txt:121`, `WITH_MESHLOG_ABSEIL_HASHMAP` at `:16`),
plus a `local-build-options.mjs` key threaded through `make.mjs`.

**Compile-time, not runtime.** `AttrData<T>::operator[]` is the hottest accessor
in the engine; even a predictable branch there is unacceptable in shipping builds.

### 1.1 Hook sites

Three separate storage paths carry hot reads. All three need hooks or the
picture is wrong:

| Path | File | Note |
|---|---|---|
| `AttrData<T>::operator[]` | `mesh/attribute.h:212` (mutable), `:217` (const) | The bulk. Instrument both; see 1.4. |
| `BoolAttrView` | `mesh/attribute_bool.h` | Bit-packed, shared buffer per domain — attribution is per *block*, not per attribute. |
| `ElemData::freemap` (`util::BoolVector`) | `mesh/elem_data.h` | Read constantly in liveness guards (5+ sites in dyntopo alone). Easy to forget. |

Shape at the main hook:

```cpp
T &operator[](int idx)
{
  T *p = &pages[idx >> ATTR_PAGESHIFT].data[idx & ATTR_PAGEMASK];
#ifdef WITH_ATTR_TRACE
  traceRead(p, sizeof(T), this);
#endif
  return *p;
}
```

`this` (an `AttrDataBase*`) is the attribution key and already carries `name`,
`type`, and `elemSize` (`attribute_base.h:47`) — reporting needs no side table.

### 1.2 Why real addresses rather than modeled ones

The awkward parts fall out for free and are not open to dispute:

- **Paging** — `AttrData` stores in 4096-element pages, each a separate
  `alloc::alloc`. Adjacent indices across a page boundary genuinely land in
  unrelated lines.
- **Bit-packed bools** — `PackedBoolAttrs` shares one buffer across every bool
  attribute in a domain; the trace sees the real reads.
- **Straddling** — `float3` is 12 bytes and 64 is not a multiple of 12, so
  `v.co` elements routinely span two lines. `traceRead` walks
  `a >> 6 .. (a + size - 1) >> 6`, so this is captured, not assumed away.

### 1.3 Storage and threading

- Per-array, per-thread `Map<uint64_t /*line*/, uint64_t /*byte mask*/>`.
- `thread_local` (precedent: the `GenSet` scratch sets at `dyntopo.h:252-271`),
  merged at dab end. dyntopo and the brush loop run under `parallel_for`; a
  shared map would both serialize and corrupt.
- Reset and report **per dab**. Strokes vary too much to average usefully.

### 1.4 Reads vs writes

The mutable `operator[]` at `:212` is used for both reads and writes, so the two
are indistinguishable at that hook. Splitting them (writes carry
read-for-ownership traffic) means instrumenting the const overload at `:217`
separately and auditing which call sites bind which. **Do the combined number
first**; treat the read/write split as an optional refinement, and say in the
report that the headline number does not separate them.

### 1.5 Correctness checks

- A synthetic array with a known stride produces the arithmetically predicted
  line count.
- Traced and untraced runs produce byte-identical mesh output (the tracer must
  not perturb behavior).
- Same scene twice ⇒ identical counts. Determinism is the whole premise; if it
  does not reproduce exactly, stop and fix it before measuring anything.

### 1.6 Cost expectation

10–50× slowdown. Fine, and deliberately so: the counter is deterministic, so a
slow run yields the same number as a fast one. **Never take timings from a
traced build** — add a banner to the report output saying so.

**Exit criterion:** deterministic per-dab, per-array line counts and utilization
percentages on a real mesh.

---

## Phase 2 — baseline on the current layout

Run the instrument over representative work and publish the current-layout table.

- Scenes: a dyntopo stroke and a non-dyntopo stroke at ~1.5 M, using the
  existing `debug_app` script harness and `bench_dyntopo`.
- Report per dab, per array: lines touched, bytes demanded, utilization %.
- Roll up per domain (V / E / C / L / F) and rank arrays by lines touched.

The expected headline is that the corner and list columns dominate line count
while showing poor utilization. If they do not, that is a real finding and the
variant thesis weakens — report it either way.

**Exit criterion:** a ranked per-array table that identifies where hot-path
bandwidth actually goes.

---

## Phase 3 — counterfactual derivation

Two candidate layouts, derived from the Phase 2 trace.

### 3.1 Triangle-only

- **Delete side (exact).** Subtract lines attributed to `c.l`, `c.next`,
  `c.prev`, the whole LIST domain (`l.size`, `l.f`, `l.c`, `l.next`, plus
  `l.freemap`), `f.list_count`, and `f.l`. These reads do not happen at all
  under the variant, so this side needs no modeling.
- **Replacement side (modeled).** `f.vs` as a contiguous `int3`, 12 bytes per
  face. Computable from the same trace, since it already records which face ids
  each dab visited. **This is a derivation and must be labelled as one in the
  report.**

### 3.2 Manifold, no holes

Subtract `f.list_count` and `l.next`. Smaller delta, and the honest framing is
stream removal from the hot walk rather than bytes saved.

**Exit criterion:** a three-column table (current / no-holes / triangle-only) of
lower-bound bytes demanded per dab, with the exact and modeled portions visually
distinguished.

---

## Phase 4 — content and code-surface scan

Two cheap numbers that carry the holes argument, which bandwidth alone will not.

1. **Content.** Scan `examples/*.wproj` and any imported production meshes for
   faces with `list_count > 1`. If real assets are essentially hole-free, that is
   decisive on its own and does not depend on any bandwidth result.
2. **Code surface, hotness-weighted.** 26 `list_count` sites across 13 files and
   29 `l.next` sites. Split them into *handles holes* vs *refuses holes* (many
   are `!= 1` rejection guards in `dyntopo.h`, `edge_flip.h`, `edge_split.h`),
   and weight by whether the site is on a hot path. A flat count of 26 is not an
   argument; "N hot-path sites exist only to refuse a case no asset contains" is.

**Exit criterion:** hole frequency across the asset corpus, plus the
handles/refuses split.

---

## Phase 5 — hardware validation

The line counter cannot see prefetching, associativity conflicts, TLB pressure,
or **dependent-load latency** — a chain of three dependent loads and three
independent loads score identically, and the dependent chain is one of the three
arguments for the triangle layout. So the model needs an independent check.

- AMD uProf or Intel VTune against the native build (`WITH_NATIVE_MSVC` makes
  this straightforward), untraced, on the same scenes as Phase 2.
- Compare measured L2/L3 miss counts and DRAM bytes per stroke against the
  Phase 2 lower bound. They should be *above* it and correlated across scenes.
- Separately capture stalled-cycles-on-memory for the radial-walk functions, to
  size the latency component the counter is blind to.

**Exit criterion:** either the counter is corroborated, or the discrepancy is
explained. If the hardware run contradicts the model, the model loses.

---

## Phase 6 — write-up

`sculptcore/documentation/research/` (per the repo research convention), aimed at
a reader who has not followed this thread:

1. The metric and its lower-bound assumption, stated up front.
2. Current-layout baseline: where hot-path bandwidth goes today.
3. Counterfactual table, exact vs modeled portions marked.
4. Hole frequency in real content + the hot-path handles/refuses split.
5. Hardware corroboration and the latency component.
6. A recommendation with an explicit "what would change our mind."

---

## Decision gates

- **After Phase 2:** if corner and list columns are not a large share of demanded
  lines, stop. The bandwidth case for a triangle variant does not exist and the
  remaining phases are not worth running.
- **After Phase 3:** if the triangle-only lower bound is within noise of current,
  stop. Publish the negative result — it is genuinely useful.
- **After Phase 4:** hole frequency alone may decide the manifold question
  independently of any bandwidth number.

## Risks

- **Tracer perturbs behavior.** Mitigated by the byte-identical-output check in
  1.5. If it ever fails, every number downstream is void.
- **Missing a hook path.** The three-way split in 1.1 is the known set;
  `BoolAttrView` and `freemap` are the ones most likely to be skipped, and both
  are hot. Audit against the Phase 2 array ranking — a domain with suspiciously
  few touches is the tell.
- **Over-reading the lower bound.** Structural, not fixable by care; handled by
  labelling and by Phase 5.
- **Replacement-side modeling drift.** The `f.vs` derivation is the softest part
  of the whole plan. Keep it to one clearly-marked cell in the table and do not
  build further inference on top of it.

## Adjacent work (not measurement)

Three items alongside this plan. None blocks the phases above; A2 and A3 gate any
*implementation* plan that follows it.

### A1 — `trisOfEdge` / `faceTri(f) → int3` helper

The five near-verbatim copies of the radial-triangle walk listed in Motivation
are duplication in the tree *today*, variant or not. Collapsing them is a
strictly-good refactor against the existing `Mesh`, and it happens to be exactly
the abstraction boundary a variant would need. Independent of this plan; worth
doing regardless of the outcome.

### A2 — meshlog event vocabulary (narrowed 2026-07-26)

The operators fire `onCornerCreate` / `onListCreate` / `onListKill` per domain
(`mesh/mesh_callbacks.h`); that is how spatial and meshlog stay current. A
triangle-only mesh has no LIST domain and implicit corners, so the vocabulary
changes.

**spatial is fine.** It takes only `onFaceCreate/Kill/Change`, `onVertKill`, and
one `onCornerKill` (`spatial.h:709-717`) — and that last one exists purely
because "onFaceKill fires after the corners are released, so the face's verts are
unreadable there." With `f.vs` as an int3 the verts *are* readable at face-kill
time and the workaround dissolves. spatial ports nearly free and gets simpler.

**meshlog is the real item.** It subscribes to all five domains
(`meshlog_base.h:2813-2829`) and is bound to that vocabulary in four places:
`LogElemKind`; the `idx_to_log_id` key `(kind << 32) | mesh_index`; the per-domain
`rowLayout(kind, grp)`; and the replay ordering invariant, which the `LogChunkTopo`
doc comment derives explicitly from the general Euler operators' cascade
semantics. A different operator set needs that invariant re-derived, not assumed.

Two options, and only one is viable:

- **Synthesize the general vocabulary** — fire `onListCreate` with ids for
  elements that do not exist. Requires retaining a phantom LIST domain purely to
  feed meshlog, i.e. not removing it. Self-defeating.
- **Second encoding** — drop `List` from the variant's kind set and fold corner
  payload into the face record. Note the win is *not* fewer attribute rows:
  corner data (UVs, split normals) still must be logged and restored. What goes
  away is per-corner and per-list **identity bookkeeping** — `LogElem`
  allocation, `idx_to_log_id` insertion, `by_log_id` entry, per-element
  `rowLayout` lookup — plus the corner payload becoming contiguous within the
  face row. Against the census in
  [`2026-07-13-2046-meshlog-capture-cost.md`](2026-07-13-2046-meshlog-capture-cost.md)
  (~33 k first-touch + ~13 k created + ~4.6 k kill rows per 480 k dab, capture
  ~18% of a dab pre-P1) that is a real but bounded target.

**What is no longer a problem.** The earlier framing assumed promotion would force
the log to carry two vocabularies with a switch marker. It does not. There is no
full-copy chunk type in meshlog (`_LogChunkTypes` is `{Topo, Reorder, Elems,
External}`); the whole-mesh undo used by triangulate / symmetrize / quad-remesh
lives *above* it as a ToolOp `_undoBlob` plus `_replaceMesh` handle swap
(`litemesh_ops.ts:472,499`; `litemesh.ts:1316`, whose comment reads "undo
restoring a pre-triangulate snapshot"). A handle swap frees the old mesh and
rebuilds spatial, so **no topo replay crosses the boundary at all** — promotion
would change representation exactly where topological continuity is already
severed by design. Deserialization-time promotion is likewise free: it precedes
any log step for that mesh.

**Surviving constraint:** promotion must never happen implicitly inside a dab.
dyntopo triangulates n-gons mid-dab with callbacks threaded (`dyntopo.h:732`);
that must remain a topology edit *within* the current representation, never a
promotion, or a representation switch lands inside an open chunk with no
`_replaceMesh` boundary to hide behind.

### A3 — id-continuity trace across a whole-mesh blob restore

Small, focused, and worth doing before any design leans on A2's conclusion.

`SculptPaintOp.meshLog` is a **static singleton** for the app session
(`sculptcore_ops.ts:150-152`), not per-mesh, and `_teardownTreeState` does not
touch it — there is no `reset()` on `MeshLog`, only a per-element
`clear(kind, idx)`. So steps recorded before a `_replaceMesh` remain in the log
holding element ids of a mesh that has since been freed. Whether they still line
up after a blob restore depends on id continuity, and `Mesh_deserialize` does not
obviously provide it: `mesh_serialize.h:19` says dead slots are compacted away and
live elements renumbered.

Trace how the TS undo stack orders a meshlog step against a ToolOp blob restore,
and whether anything upstream invalidates or rebases the log. Two outcomes, both
useful:

- **Already handled** — promotion inherits that protection for free and the
  conversion risk in A2 drops to near zero.
- **Not handled** — this is a pre-existing latent bug in the symmetrize /
  quad-remesh undo path today, independent of any variant work, and should be
  filed on its own merits.
