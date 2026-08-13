# Brush — High-level overview

The brush subsystem (`source/brush/`) applies sculpt operations to mesh
geometry. Brushes are **authored once** in a small DSL (`sbrush`) and
compiled to every backend sculptcore targets — the native/WASM C++ executor
(the reference) plus WGSL, SPIR-V, CUDA, HIP, and OpenCL. At runtime a
`CommandExecutor` walks a set of `SpatialNode`s, builds an invocation
context, and dispatches the compiled command that mutates vertices through a
vertex-iterator factory.

This document covers the *runtime* (how a stroke reaches a kernel). Two
companions cover the rest:

- [`brush_dsl.md`](brush_dsl.md) — the `sbrush` language reference.
- [`brush_compute.md`](brush_compute.md) — the compiler, build wiring, and
  cross-backend verification. ([`brush_compute_dsl.md`](brush_compute_dsl.md)
  is the original design doc / deferred-work record.)

Ten brushes ship today: `DRAW`, `INFLATE`, `CLAY`, `PINCH`, `SHARP`, `MASK`,
`SMOOTH`, `KELVINLET`, `POSE`, `TEXDRAW` (`brushes/types.h`). They are no
longer hand-written — each is a `kernels/<name>.sbrush` source whose C++
output is generated into `kernels/generated/<name>.brush.gen.h`.

## File map

| File | Role |
|---|---|
| `brush.h` / `brush.cc` | `Brush` aggregate (strength, radius, invert, falloff curve/kind/shape, brush texture, stroke path) + property load/binding |
| `brush_command.h` / `.cc` | `CommandCtxBase`, templated `CommandCtx<TYPES>`, `strength`/`sampleBrushTex`, `BrushCommandDef` |
| `brush_executor.h` / `.cc` | `CommandExecutor`: `createCommand()` enum→factory dispatch, node iteration, `MeshLog` ownership, per-dab stroke-path push |
| `brush_iterators.h` | `BasicVertexIter` + `PtrHelper` proxy over node vertices |
| `brush_concepts.h` | C++20 concepts: `VertexIter`, `VertexIterFactory`, `CommandTypes`, `BrushCommand` |
| `stroke_spacing.h` | stroke-dab spacing helper |
| `props.h` | brush-property templates |
| `bindings.h` / `.cc` | `registerBindings(BindingManager&)` — exposes `Brush`, `CommandExecutor`, `Vector<SpatialNode*>` to the WASM/TS surface |
| `brushes/types.h` | `SculptBrushes` enum + its binding |
| `brushes/all.h` | aggregate include of every `kernels/generated/*.brush.gen.h` |
| `kernels/*.sbrush` | brush DSL sources |
| `kernels/generated/*.brush.gen.h` | checked-in C++ kernels (consumed directly by the WASM build) |
| `compiler/` | the `sbrushc` host tool (see [`brush_compute.md`](brush_compute.md)) |
| `exec.h`, `test.h` | reserved (currently unused) |

Each `.brush.gen.h` defines, per brush, a `create<Name>Brush(def)` factory, a
`<name>` kernel template, and a `<name>Pre` undo pre-pass. `brushes/all.h`
pulls them all in; `createCommand()` dispatches the `SculptBrushes` enum to
the matching factory.

## Core types

### `Brush` (`brush.h`)
State struct: `strength`, `radius`, `invert`, the falloff curve + `FalloffKind`
/ `FalloffShape` selectors, the bound brush texture
(`tex_width/tex_height/tex_pixels` + `coord_space`/`tex_repeat`), the stroke
`StrokePath` ring buffer, plus an embedded `props::StructProp` /
`props::DeviceInputCtx`. `loadProps()` pulls active values from the property
registry; `defineBindings()` / `registerBindings` expose it to the litestl
reflection layer.

### `CommandCtxBase` / `CommandCtx<TYPES>` (`brush_command.h`)
`CommandCtxBase` is the immutable, geometry-free part of an invocation: mouse
position/direction, render matrix, surface position and normal at the brush
dot, and the optional `co_prev` snapshot. `CommandCtx<TYPES>` adds a reference
to the active `Brush`, the current `SpatialNode`, and the vertex-iterator
factory. It computes per-vertex falloff through `strength(co)`
(`brush_command.h:63`):

```cpp
float t = 1.0f - std::min(brush.falloffDist(co - surfacePos, surfaceNo), 1.0f);
return brush.strength * brush.falloffEval(t);
```

`falloffDist` applies the spatial metric (`FalloffShape`:
spherical / cube / linear / box — box is the stroke-aligned oriented cuboid,
built from `surfaceNo` + `falloff_dir`) and `falloffEval` the curve shape
(`FalloffKind`:
smoothstep / linear / gaussian / curve-LUT). Both enums are bound directly as
`Brush` members (`falloff_shape`/`falloff_kind`, via `Binder<FalloffKind>` /
`Binder<FalloffShape>` in `bindings.cc`), so the TS bridge sets them as plain
properties rather than through int-taking setter methods; a custom LUT can be
written per-entry through `setFalloffCurveEntry(i, f)` (sized by the bound
`falloffCurveSize`) — see [`brush_compute_dsl.md`](brush_compute_dsl.md#falloffs)
for the full authoring-vs-direct-write picture. Note `strength(co)` is
`strength · falloff · texture` — radius is *not* baked in here. A kernel that
wants radius-proportional displacement multiplies by its own `radius` uniform
(`draw`/`inflate`/`pinch` use `… * radius * 0.5`; `smooth`/`sharp`/`mask` are
relative or radius-independent; the `plane` family scales through its
`planeoff · radius` offset). `sampleBrushTex(co, no)` maps the
point to UV per `brush.coord_space` (Global / ViewPlane / ViewRepeat /
StrokeCurved / Projected) and samples; it returns `1.0` with no texture bound.
`strength` multiplies it in, so every kernel that calls `strength` is
texture-modulated; only `@unbounded` kernels (which may not call `strength`)
sample it explicitly.
These two methods back the `strength` and `sampleBrushTex` DSL intrinsics — see
[`brush_compute_dsl.md`](brush_compute_dsl.md) for the falloff and coord-space
details.

### `CommandExecutor` (`brush_executor.h`)
Holds pointers to the active `Brush`, `SpatialTree`, an optional
`meshlog::MeshLog*`, and a shared `CommandCtxBase`. `execBrush(brushType,
nodes, origin, normal)` resolves the brush via `createCommand()` (returns a
`BrushCommandDef`), then `exec()` runs:

1. `cmd.execPre(ctx, nodes)` — bulk pre-pass (e.g. pushing undo data into
   `meshLog` for each node before any vertices move).
2. Per-node: build `CommandCtx<CommandExecutor>` from the shared base, the
   node, the vertex-iterator factory, and the `Brush`, then `cmd.exec`.
3. `cmd.execPost(ctx, nodes)` — bulk post-pass.

`beginStep()` / `endStep()` bracket a stroke and forward to the `MeshLog`;
`beginStep` also resets the brush `StrokePath`, and `execBrush` pushes each
dab center onto it. `isFirstOfStep` lets commands record undo state lazily;
`clearIsFirstOfStep()` is exposed to JS.

### `BasicVertexIter` / `PtrHelper` (`brush_iterators.h`)
A forward iterator over a `SpatialNode`'s vertices yielding a `PtrHelper`
proxy that exposes `co`, `no`, `mask`, and the vertex id. The proxy holds
references into the mesh attribute arrays, so writes through `vi.co` mutate
the mesh in place.

## Concepts and template plumbing

`brush_concepts.h` defines four concepts that pin the command ABI without
fixing a concrete iterator type: `VertexIter` (`co`/`no`/`mask`/`v`),
`VertexIterFactory` (invocable with a `SpatialNode&`, returns a range),
`CommandTypes` (names a `vertex_iter` + `vertex_iter_factory`), and
`BrushCommand` (invocable with `CommandCtx<TYPES>&`).

The key mechanism is `vertexIter` on `CommandCtx` (`brush_command.h:50`):

```cpp
TYPES::vertex_iter_factory &vertexIter;
```

It is a *reference to a factory*, not an iterator. The generated kernel calls
it with the current node — `for (auto &v : ctx.vertexIter(ctx.node))` — so the
iteration strategy can be swapped per executor without changing the kernel.

## Execution flow

1. Caller invokes `CommandExecutor::execBrush(type, nodes, origin, normal)`.
2. `createCommand()` returns a `BrushCommandDef` with `execPre` / `exec` /
   `execPost` wired to the generated factory for the requested brush.
3. `execPre` runs once over all nodes — typically pushing undo state into
   `meshlog::MeshLog` on the first step.
4. Per `SpatialNode`, the executor builds a `CommandCtx` and runs `exec`.
5. The kernel mutates vertices through the iterator proxy and marks the node
   dirty for downstream stages (`Spatial_UpdateNormals | Spatial_UpdateGPU |
   Spatial_RegenBounds`).
6. `execPost` runs once after all nodes.

## GPU / cross-backend dispatch

The C++ kernels above are the reference path. The same `.sbrush` sources also
compile to GPU backends; `runBrushStrokeGPU` (`source/debug/script.cc`) loads
a brush's SPIR-V and dispatches it as a compute kernel, and `make.mjs
sbrush-verify` / `webgpu-verify` assert the GPU output matches the C++
reference bit-for-bit modulo fp. The whole compile-and-verify stack is
documented in [`brush_compute.md`](brush_compute.md).

## Stroke methods (Path / Anchored / Drag Dot)

How the TS `BrushStrokeDriver` (`scripts/editors/view3d/tools/stroke_driver.ts`)
turns pointer input into a sequence of dabs is a per-brush property,
`Brush.strokeMethod` (`scripts/brush/brush_base.ts`, `StrokeMethod` enum;
data-API path `brush.strokeMethod`, defaulting to `Path` for every brush
preset — only Grab and Kelvinlet opt into `Anchored`; Snake Hook stays on
`Path`, tracking the live raycast surface each dab):

- **`PATH`** (default) — the existing arc-length Catmull-Rom/Bezier walk:
  every pointer move that crosses the spacing threshold emits a new,
  permanently-committed dab.
- **`ANCHORED`** — the dab origin is fixed on the first input; every
  subsequent input re-derives a live radius or angle from the drag vector
  (`AnchoredLiveMode`: `RADIUS` or `ANGLE`, per-brush). Replaces the old
  bespoke `grabAnchor` mechanism and unifies Grab/Kelvinlet/Snake Hook onto
  one anchor implementation.
- **`DRAG_DOT`** — follows the live cursor, emitting one dab per pointer
  move, Blender-style: only the final, released position stays applied.

Anchored and Drag Dot share a **live-mutating preview** requirement: every
dab but the stroke's last must vanish the instant the next one lands, so a
long drag never leaves a trail of committed partial dabs. `CommandExecutor`
has no per-dab undo granularity for this (a step's `LogChunkElems` captures
once per domain, not once per dab), so `meshlog::MeshLog` exposes a
dedicated primitive pair for it (`source/meshlog/meshlog_base.h`):

- `beginPreviewDab(mesh, tree, center, radius)` — snapshots every vertex
  row within `radius` of `center` (a superset of one dab's footprint) and
  remembers the step's current chunk count.
- `rollbackPreviewDab(mesh, tree)` — pops and undoes every `LogChunkTopo`
  chunk pushed since the paired `beginPreviewDab` (dyntopo restructuring),
  then restores the snapshotted vertex rows directly, bypassing the log.
  It must **not** touch a `LogChunkElems` chunk that happens to be created
  in the same range (the step's first dab commonly creates the step's only
  element-store capture) — that chunk owns the pre-*step* baseline the real
  `MeshLog::undo()` needs later and has to survive every preview rollback
  in the stroke, not just one.

`SculptPaintOp` (`scripts/editors/view3d/tools/sculptcore_ops.ts`) drives
the pair once per dab — roll back the previous preview (if any), begin a
new one, `applyDab` — and is scoped to the no-symmetry case: preview state
is a single global slot with one `(center, radius)` region, so it can't
safely cover several independently-positioned mirror dabs at once.
Symmetric Anchored/Drag Dot strokes fall back to committing every dab
immediately, same as Path. The GPU stroke path bypasses this preview
machinery entirely (non-shadow GPU dabs commit straight to the kernel
dispatch); this is safe today because the only GPU-eligible brushes
(Grab/Kelvinlet) use an idempotent from-orig kernel (`AccumOrigGrab` in
`source/brush/accum_mode.h`) that recomputes an absolute
result from each vertex's stroke-start position every dab, so committing
without rollback cannot compound — a brush that needed live-preview
correctness on the GPU (not just as a memory-bloat optimization) would
need to plumb the pair through the GPU dispatch too. See
[`documentation/plans/anchored-drag-dot-stroke-2026-07-16.md`](plans/anchored-drag-dot-stroke-2026-07-16.md)
for the full design and rollout notes.

## Bindings & external surface

`registerBindings(BindingManager&)` (`bindings.cc`) registers `Brush`,
`CommandExecutor`, and `util::Vector<SpatialNode*>`. `Brush::defineBindings()`
declares the exposed members; `CommandExecutor::defineBindings()` exposes a
`(SpatialTree*, Brush*)` constructor, the `brush`/`tree`/`meshLog` members, and
`execBrush` / `clearIsFirstOfStep`. The litestl binding system feeds the
TypeScript generator (`source/litestl/binding/generators/typescript.cc`), so
binding changes propagate into the generated TS surface. There is no
`c-api/` under `source/brush/` — the binding system is the public surface.

## Integration dependencies

- `spatial/` — `SpatialNode`, `SpatialTree`, update flags.
- `mesh/` — vertex position/normal/mask arrays accessed via `BasicVertexIter`;
  one-ring connectivity for `for_neighbor` brushes (smooth).
- `meshlog/` — `CommandExecutor` holds a `meshlog::MeshLog*`; commands record
  per-node undo chunks via `execPre` on the first step.
- `props/` — `StructProp` / `DeviceInputCtx` / `CurveGen` (falloff curve)
  embedded in `Brush`.
- `litestl/` — `math`, `util`, `binding`.

## Adding a new brush

1. Write `kernels/<name>.sbrush` (see [`brush_dsl.md`](brush_dsl.md)).
2. `node make.mjs codegen` to emit `kernels/generated/<name>.brush.gen.h`.
3. Add its include to `brushes/all.h`, an entry to `SculptBrushes`
   (`brushes/types.h`), and a `createCommand()` dispatch case.
4. If new brush state is needed, add fields to `Brush` and update
   `defineBindings()` / `loadProps()`.
5. For GPU dispatch + verification, add a `runBrushStrokeGPU` case and a
   `tests/scripts/brush_backends/<name>_ab.txt` script.
