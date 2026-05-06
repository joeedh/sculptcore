# Brush — High-level overview

The brush subsystem (`source/brush/`) applies sculpt operations to mesh
geometry. It is organized as a small, template-driven command system: a
`CommandExecutor` walks a set of `SpatialNode`s, builds an invocation
context, and dispatches a templated command (e.g. `draw`) that mutates
vertices through a vertex-iterator factory. The system is young — only
the `DRAW` brush is implemented today — but the type plumbing is
designed so additional brushes drop in by adding an enum entry and a
template specialization.

## File map

| File | Role |
|---|---|
| `brush.h` / `brush.cc` | `Brush` aggregate (strength, radius, invert) + property load/binding |
| `brush_command.h` / `brush_command.cc` | `CommandCtxBase`, templated `CommandCtx<TYPES>`, falloff |
| `brush_executor.h` / `brush_executor.cc` | `CommandExecutor`: builds commands and iterates nodes |
| `brush_iterators.h` | `BasicVertexIter` + `PtrHelper` proxy over node vertices |
| `brush_concepts.h` | C++20 concepts: `VertexIter`, `VertexIterFactory`, `CommandTypes`, `BrushCommand` |
| `props.h` | Brush-property templates and validation helpers (partially scaffolded) |
| `bindings.h` / `bindings.cc` | `registerBindings(BindingManager&)` for litestl/WASM/TS |
| `exec.h`, `test.h` | Empty stubs reserved for future use |
| `brushes/types.h` | `SculptBrushes` enum (currently `DRAW`) |
| `brushes/all.h` | Aggregate include of brush implementations |
| `brushes/draw.h` | `draw<TYPES>` template command |

## Core types

### `Brush` (`brush.h`)
Plain state struct holding `strength`, `radius`, `invert`, and an embedded
`props::StructProp` plus `props::DeviceInputCtx`. `loadProps()` pulls
the active values out of the property registry. `defineBindings()` /
`registerBindings` expose the struct to the litestl reflection layer.

### `CommandCtxBase` / `CommandCtx<TYPES>` (`brush_command.h`)
`CommandCtxBase` is the immutable, geometry-free part of a command
invocation: mouse position/direction, render matrix, surface position
and normal at the brush dot. `CommandCtx<TYPES>` extends it with a
reference to the active `Brush`, the current `SpatialNode`, and the
vertex-iterator factory (see below). It also computes per-vertex
falloff via `strength(co)` (`brush_command.h:50-55`):

```cpp
float t = (co - surfacePos).length() / brush.radius;
return brush.strength * t;
```

### `CommandExecutor` (`brush_executor.h`)
Holds pointers to the active `Brush` and `SpatialTree`. `execBrush(type,
nodes)` resolves a brush from the `SculptBrushes` enum, builds an
`std::function`-typed command via `createCommand()`, then iterates the
provided nodes constructing a `CommandCtx` for each and invoking the
command.

### `BasicVertexIter` / `PtrHelper` (`brush_iterators.h`)
A forward iterator that walks a `SpatialNode`'s vertices and yields a
`PtrHelper` proxy exposing `co`, `no`, `mask`, and the underlying
vertex id. The proxy holds references into the mesh attribute arrays,
so writes through `vi.co` etc. mutate the mesh in place (used in
`brushes/draw.h:12`).

## Concepts and template plumbing

`brush_concepts.h` defines four concepts that pin down the command
ABI without forcing a single concrete iterator type:

- `VertexIter` — has `co`, `no`, `mask`, `v` accessors.
- `VertexIterFactory` — invocable with a `SpatialNode&`, returns a
  range of `VertexIter`s.
- `CommandTypes` — a traits bundle that names a `vertex_iter` type and
  a `vertex_iter_factory` type.
- `BrushCommand` — invocable with `CommandCtx<TYPES>&`.

The key mechanism is `vertexIter` on `CommandCtx` (`brush_command.h:37`):

```cpp
TYPES::vertex_iter_factory &vertexIter;
```

It is a *reference to a factory*, not an iterator. The command body
calls it with the current node — `for (auto &vi : ctx.vertexIter(ctx.node))`
in `brushes/draw.h:11` — so the iteration strategy (which vertices are
considered "in the brush") can be swapped out per executor or per
command type without changing the command itself.

## Execution flow

1. Caller invokes `CommandExecutor::execBrush(SculptBrushes type, nodes)`.
2. `createCommand()` returns a `std::function<void(CommandCtx<TYPES>&)>`
   for the requested brush (today: `draw`).
3. For each `SpatialNode` in `nodes`, the executor builds a
   `CommandCtx` with the shared `CommandCtxBase`, the node, the vertex
   factory, and the `Brush` reference.
4. The command runs, mutating vertices through the iterator proxy.
5. The command marks the node dirty for downstream stages — `draw` uses
   `Spatial_UpdateNormals | Spatial_UpdateGPU | Spatial_RegenBounds`
   (`brushes/draw.h:15`).

## Strength / falloff

Falloff is centralized on `CommandCtx::strength(co)`. It is currently a
linear distance-to-radius scale times the brush strength; the `draw`
command applies it together with the per-vertex `mask` attribute:

```cpp
vi.co += ctx.surfaceNo * ctx.strength(vi.co) * vi.mask;
```

A non-linear falloff curve (referenced in `props.h`) is on the property
side but not yet wired through `strength()`.

## Bindings & external surface

`bindings.cc::registerBindings(BindingManager&)` registers the `Brush`
struct with the litestl binding manager. `Brush::defineBindings()`
declares the exposed members (`strength`, `radius`, `invert`, `props`).
The litestl binding system feeds the TypeScript generator
(`source/litestl/binding/generators/typescript.cc`), so changes to the
binding metadata propagate into the generated TS surface used by the
WASM frontend. There is no separate `c-api/` directory under
`source/brush/` — the binding system is the public surface.

## Integration dependencies

- `spatial/` — `SpatialNode`, `SpatialTree`, and update flags
  (`Spatial_UpdateNormals`, `Spatial_UpdateGPU`, `Spatial_RegenBounds`).
- `mesh/` — vertex position, normal, and mask attribute arrays accessed
  through `BasicVertexIter`.
- `props/` — `StructProp` / `DeviceInputCtx` embedded in `Brush`;
  `props.h` adds brush-specific property templates.
- `litestl/` — `math` (`float2`, `float3`, `mat4`), `util`, and
  `binding` for reflection.

CMake links `eigen`, `util`, `math`, `props`, `spatial`.

## Status & extension points

The subsystem is intentionally minimal scaffolding. Notable open ends:

- `exec.h` and `test.h` are empty placeholders.
- `SculptBrushes` only contains `DRAW`.
- `props.h` defines validation/lookup templates that aren't fully
  consumed yet.
- Falloff curve from props is not yet applied in `CommandCtx::strength`.

To add a new brush:

1. Add an entry to `SculptBrushes` (`brushes/types.h`).
2. Implement a `template <CommandTypes TYPES> static void
   myBrush(CommandCtx<TYPES>&)` in `brushes/<name>.h` and include it
   from `brushes/all.h`.
3. Dispatch it from `CommandExecutor::createCommand()` /
   `execBrush()` based on the new enum value.
4. If new state is needed, add fields to `Brush` and update
   `defineBindings()` / `loadProps()`.
