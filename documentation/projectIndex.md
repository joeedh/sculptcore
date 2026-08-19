# Sculptcore — Project Index

A C++20 sculpting/mesh engine that builds natively and to WebAssembly via Emscripten. Cross-compiled with CMake + Ninja; a Node-based dispatcher (`make.mjs`) drives configure/build.

## Top-level layout

| Path | Purpose |
|---|---|
| `CMakeLists.txt` / `CMakePresets.json` | Root CMake config. Builds `sculptcore` executable; links `util`, `mesh`, `platform`. WASM exports are collected via the `WASM_SYMBOLS` global property. |
| `make.mjs` | Node build dispatcher (replaces prior emscripten wrappers). Commands: `configure`, `build`, `test`, `clean`, `install-emsdk`; each build command takes `[wasm\|native]` (default `wasm`). WASM builds in `build/`, native in `build/native/`. |
| `configureEnv.mjs` | Environment bootstrap for emsdk / pinned toolchain; wraps every cmake/ninja/ctest invocation from `make.mjs` (with `--emsdk` for WASM targets). |
| `serv.mjs` | Dev HTTP server for the WASM/browser frontend. |
| `index.html` | Browser entry that loads the WASM module. |
| `emsdk/` | Emscripten SDK (git-cloned + pinned by `make.mjs install-emsdk`, gitignored — not a submodule). Version pinned in `emsdkVersion.txt`. |
| `build_files/` | `macros.cmake`, `WASM.cmake`, `link_wasm.py`, `emsdk_env.py`. |
| `extern/` | Vendored deps: `eigen_dist/`, `glfw/`. Vulkan headers + loader come from the system Vulkan SDK (`find_package(Vulkan)`). |
| `assets/` | Runtime assets. |
| `tests/` | GTest-style unit tests (native builds only). |
| `documentation/` | Project docs (this file). |

## Source tree (`source/`)

### `source/litestl/` — foundational library (self-contained, has own CMake/tests)

| Module | Notes |
|---|---|
| `util/` | Core containers and primitives: `vector`, `map`, `set`, `ordered_set`, `span`, `array`, `boolvector`, `binaryHeap`, `atomicLinkedList`, `function`, `callback_list`, `hash`, `index_range`, `rand`, `string`, `task` (job system), `memory`/`alloc`, `concepts`, `type_tags`, `time`, `wasm.h`. |
| `math/` | `vector`, `matrix`, `quat`, `color`, `geom`, `bspline`, `mix`, `lut`. |
| `platform/` | Platform abstraction: `win32.cc`, `linux.cc`, `common.cc`, `cpu.h`, `time.h`, `export.h`. |
| `path/` | Filesystem path utilities (`path.cc/.h`). |
| `binding/` | Reflection/binding system — `binding_base`, `binding_struct`, `binding_types`, `binding_utils`, `manager`. |
| `binding/generators/` | Codegen; currently a TypeScript generator (`typescript.cc/.h`). |
| `tests/` | litestl-local tests. |

### `source/mesh/` — mesh data structures

Core: `mesh.cc/.h`, `mesh_base.h`, `mesh_types.cc/.h`, `mesh_shapes.cc/.h`, `mesh_proxy.h`, `mesh_iter.h`, `mesh_enums.h`.
Attributes: `attribute.cc/.h`, `attribute_base.h`, `attribute_bool.h`, `attribute_builtin.h`, `attribute_enums.h`, `elem_data.h`.
Merge policy: `attr_merge.cc/.h` — how a layer produces the value of an element a split/collapse creates.
Vertex-group weights: `attr_weights.cc/.h` (`WeightsRef`, the reference discipline over an `AttrType::WEIGHTS` column) + `deform_pool.cc/.h` (interned, refcounted, sharded weight runs — Blender's `MDeformVert` as an attribute type).
Serialization: `mesh_serialize.cc/.h` — versioned lz4hc blobs, with a migration chain.
ID map: `idmap.cc/.h`.
Bindings: `bindings.cc/.h` — module-level `registerBindings(BindingManager&)`.
Utils: `utils/triangulate.h`, `utils/delaunay.h`, `utils/edge_collapse.h`.
GPU bridge: `gpu/mesh_drawbatch.cc/.h` — builds a `sculptcore::gpu::DrawBatch` from a mesh.
C API: `c-api/mesh_c_api.cc/.h` — external surface for WASM/JS.

See `documentation/mesh.md` for a detailed overview.

### `source/displace/` — sculpt-layer compositor + frame provider

`compositor.cc/.h` — evaluates the sculpt-layer stack (`AttrUse::SCULPT_LAYER`
FLOAT3 vertex attrs + the `Mesh::sculptLayers` settings sidecar) into `v.co`;
evaluated positions are authoritative, the base is implicit (`co − Σ w·d`).
V2 (plans/sculptLayersV2.md): `setActiveEditLayer` makes a layer the edit
target — sculpting co IS editing it; its delta derives from the TEMP
`.slayer.rest` snapshot and `foldActiveLayer` (`Mesh::foldActiveSculptLayer`,
mesh-side so `writeMeshRaw` folds on save) rewrites the column on demand.
`LayerEditScope` is the region-scoped bracket the brush executor wraps around
layer-writing dabs; `setLayerWeight/Enabled/Frozen`/`removeLayer` mutate
settings while keeping co current (mutating the target ends the edit first;
other-layer mutations mirror into the rest snapshot). `frames.cc/.h` — the
frame provider (displacement plan F3): smoothed per-vertex normal + 4-RoSy
cross-field tangent (`.frames.v.*`, persistent NOINTERP), deterministic
Gauss-Seidel. See `documentation/plans/displacementAndSubSurf.md`.

### `source/subdiv/` — Catmull-Clark refiner (subsurf/multires track)

`subdiv.cc/.h` — uniform CC `Refiner` over `mesh::Mesh` (n-gon→quad first
step, `EDGE_SHARP`/boundary crease rules), emitting per level: the
materialized level mesh, Ptex-style per-cage-corner grid tables, and a cached
`StencilTable` whose row evaluation *is* the canonical position arithmetic
(`evalFromCage` is bit-identical to re-refining). `grids.cc/.h` — `GridsStore`,
the multires data carrier: per-quadrant grids of per-level frame-relative
displacement + custom float channels, implicit topology (4 transpose seam
links, O(1) `neighbor()`, `seamMates()` replica enumeration), whole-grid
chunking with offset-table-headed lz4 serialization, and compressed level
eviction (X5: `evictLevel` lz4s a level's chunks per channel; `elem()`
self-heals; `Multires::storeBudgetBytes` drives the finest-first policy).
`multires.cc/.h` — `Multires`: materializes the active level's `mesh::Mesh` +
`SpatialTree` from the stencil chain + stored displacement (`parametricFrames()`
on the smoothed base — the lattice-derived frame that replaced the F3 provider
here, for both frame stability and cost; see `CLAUDE.md` § *Subdivision and
multires*), LRU-cached with eviction; `writeback()` re-expresses edits as
store deltas, skipping bit-identical verts so edit-free switches are lossless;
`downRefit()` (CG least-squares level fit), `captureDetailToVdm()` (X4:
grids disp -> Ptex VDM texels; refuses while layer channels contribute),
grid-chart UV synthesis (`assignGridUVs`) and the X3 stencil/topology export
seam for the app's GPU-amplified render tier. Sculpt layers on the stack
(sculptLayersV2 M3) are per-layer FLOAT3 store channels keyed by settings-only
cage rows: levels composite `ch0 + Σ w·enabled·ch`, `writeback` lands in the
edit target's channel (else 0), and the bound layer surface
(`layerAdd/Remove/Set*`, `setEditTarget`, `layerTableOut/Restore`) folds the
active level first, then invalidates + rematerializes.
Store channels carry two independent flags: `persist` is a pure host contract
(does the embedder save it — the engine never branches on it) and
`GridLevelRule` decides level transitions. `Delta` channels (`disp`, sculpt
layers) hold a per-level correction, so a new finest level starts blank and a
dropped one takes its correction with it; `Authored` channels (mask, face sets,
colour, kernel session layers) prolong up through `seedLevelFromBelow` and
restrict back down by injection (`restrictLevelToBelow`), which is that
prolongation's exact left inverse and does no float math, so it is legal on an
INT-typed channel. C API: `c-api/subdiv_c_api.cc` (levels, positions, writeback,
grid attrs) and `c-api/grid_channel_c_api.cc/.h` — the whole interface an
embedding host has to a multires-domain attribute: enumerate/describe/ensure a
channel and read or write one level a grid range at a time. `buildFromCage` is
unconditionally destructive and is the store's LOAD boundary, so a host restores
through that same `Ensure` + `Write` pair.
Stencil rows evaluate as an fma chain (std::fma), bit-shared with the S5 GPU
SpMV (`source/webgpu/wgpu_stencil.{h,cc}`). Displacement plan S1–S5; see
`documentation/plans/displacementAndSubSurf.md`.

### `source/vdm/` — vector-displacement carrier (displacement track V/X)

`vdm_store.cc/.h` — `VdmStore`: sparse tiled float3 texel store behind one
`sample(face, u, v)` seam with two backends (`VdmStoreParams.backend`): UV
**atlas** and **Ptex** (per-grid `R_g x R_g` lattices with one-texel guard
rings copied through the grids' transpose adjacency — `syncGridSkirts`);
tile-delta undo bracket (`beginDelta`/`endDelta`, self-inverse swaps),
canonical-key-order v2 serialization, GPU residency packing (stable atlas
slots, page/Ptex-offset tables, dirty-slot drain). `vdm_splat.cc/.h` — the
brush splatter: per-face UV rasterization, world falloff from the displaced
point, tangent inversion through the F3 frame, fold-bound clamp
(`alpha * rho_min`). `vdm_bake.cc/.h` — X4 cross-carrier bakes:
`applyToVerts` (VDM -> geometry, bake = render). `vdm_promote.cc/.h` — V4
fold/overhang promotion to real geometry. `vdm_undo.h` — `VdmLogChunk` /
`VdmEdgeFlagLogChunk` MeshLog chunks. C API: `c-api/vdm_c_api.cc` (raw +
logged splats, apply, store blobs incl. in-place restore). Design:
`documentation/final-displacement-architecture.md`; plan:
`documentation/plans/displacementAndSubSurf.md`.

### `source/meshlog/` — sculpt undo/redo log

`meshlog.h` (umbrella), `meshlog_base.h` (`MeshLog`, `LogEntry`, `LogChunk`, `LogChunkElems`, `LogChunkTopo`, `LogChunkReorder`, `LogElem`, `detail::ChunkElemData`, `detail::ChunkElemRow`), `attr_saver.h` (`AttrSaver` per-element save gate), `bindings.cc/.h`. Two principal chunk types: `LogChunkElems` stores sparse append-as-touched per-domain attribute swaps for plain position / paint sculpting (gated per-element by `AttrSaver`, so it survives mid-stroke dyntopo restructuring; captured attrs declared per-brush via the sbrush `save` statement); `LogChunkTopo` stores merged per-element create/change/kill records driven by `mesh::MeshCallbacks` for full topological undo. Integrated with `brush::CommandExecutor` (`meshLog` member).

See `documentation/meshlog.md` for a detailed overview.

### `source/brush/` — sculpt brushes

Core: `brush.cc/.h` (Brush state + props), `brush_command.cc/.h` (`CommandCtxBase`, `CommandCtx<TYPES>`, falloff), `brush_executor.cc/.h` (`CommandExecutor`: builds + dispatches commands, owns `MeshLog` pointer), `brush_iterators.h` (`BasicVertexIter`, `PtrHelper`), `brush_concepts.h` (C++20 concepts pinning the command ABI).
Brushes: `brushes/types.h` (`SculptBrushes` enum), `brushes/tools.txt` (the item names in id order — the id authority codegen reads), `brushes/generated/` (generated id→factory dispatch + `Binder` item list), `brushes/all.h`, `brushes/draw.h`.
Textures: `texture_program.cc/.h` (runtime `.stex` compile — tcc CPU JIT via `texture_jit.cc/.h`, WGSL stroke-begin splice for the wgpu dispatcher), `texture_registry.cc/.h` (precompiled `.stex` unit table), `host_sampler.cc/.h` (host/builtin sampler registry; builtin `vnoise`).
Compiler: `compiler/` (`sbrushc_core` static lib + thin CLI — the sbrush/`.stex` parser and every backend emitter; host tool at build time, linked into the engine for runtime texture compilation).
Bindings: `bindings.cc/.h`.
Misc: `props.h` (brush-property templates), `exec.h` / `test.h` (reserved).

See `documentation/brush.md` for a detailed overview and `documentation/textureScripts.md` for the texture-script system.

### `source/spatial/` — spatial acceleration

`spatial.cc/.h`, `spatial_base.h`, `node.cc/.h`, `spatial_attrs.h`, `spatial_enums.h`, `spatial_gpu.cc`. Bindings: `bindings.cc/.h`. C API: `c-api/spatial_c_api.cc` (build/free `SpatialTree`, `getSpatialShaders`). GPU shaders: `shaders/`. Incremental dyntopo currency (M7.6) lives here: `add_face_at` (O(1) anchor placement), `applyDeferredNodeSplit`, `applyDeferredMerge`/`merge_node`, `free_node`.

### `source/dyntopo/` — dynamic-topology remesh

`dyntopo.h` (header-only). `runDyntopoRemesh(...)` runs the per-dab remesh as independent-set rounds of split/collapse/flip/smooth with a graded target and a per-dab split budget. Spatial/brush/meshlog-free — the caller threads `MeshCallbacks`. Design + plan: `documentation/dynamic-topology.md`, `documentation/plans/dyntopo-m7-cascade.md`.

### `source/props/` — property/reflection system

`props.cc/.h`, `prop_base.h`, `prop_struct.cc/.h`, `prop_curve.h`, `curve_cache.cc`, `prop_coerce.h`, `prop_dynamics.h`, `prop_enums.h`, `prop_types.h`, `prop_inherit.cc`. Bindings: `bindings.cc/.h`.

### `source/gpu/` — GPU abstraction

Frontend: `batch.h`, `command.h`, `pipeline.h`, `shader.cc/.h`, `texture.h`, `vbo.cc/.h`, `types.h`, `standard_attrs.h`, `manager.cc/.h`, `uniform_link.cc/.h`. Bindings: `bindings.h`.
Native backend: `source/vulkan/` — `vk_context`, `vk_backend`, `vk_overlay`, `vk_screenshot`. WGSL shaders under `source/spatial/shaders/*.wgsl` are compiled to SPIR-V at build time via `naga` (`tools/wgsl-to-spirv.mjs`).

See `documentation/rendering.md` for the batch/command object model and the uniform-block link pass.

### `source/core/` — aggregate bindings

`bindings.cc` — `extern "C" initBindings()` invokes every module's `registerBindings()` and registers the primitive `util::Vector<T>` instantiations exposed to JS. The single WASM-exported entry point for reflection setup.

### `source/io/` — serialization

Currently empty (placeholder).

### `source/window/` — windowing

`window.cc/.h` (GLFW-based, native only).

### `source/wasm/` — WASM glue

`jslib.js` — Emscripten JS library layer. `wasmManager.cc/.h` — C++-side WASM manager singleton (`sculptcore::wasm::manager`).

### `source/app/` — application entry

`app.cc`, `app.h` (currently stub), `CMakeLists.txt`.

## Tests (`tests/`)

GTest-style C++ tests: `test_binding`, `test_brush`, `test_delaunay`, `test_edge_collapse`, `test_mesh`, `test_props`. Helpers: `mesh_dump.h`, `test_util.h`. TS-side: `wasmTest.ts` (browser harness), `testViewer3D/`. litestl-internal tests live under `source/litestl/tests/`.

## Build targets

- Native: standard CMake + Ninja. Tests enabled when `BUILD_WASM` is OFF.
- WASM: `BUILD_WASM=ON`; exported symbols aggregated via CMake `WASM_SYMBOLS` global property; linked with `-sMODULARIZE=1 --bind`.

## Entry points

- Browser: `index.html` → loads `build/sculptcore.js` (Emscripten modularized).
- Native executable: `sculptcore` target (root `CMakeLists.txt`).
- Dev loop (WASM): `node make.mjs configure && node make.mjs build`, then `node serv.mjs`.
- Native loop: `node make.mjs configure native && node make.mjs build native && node make.mjs test native`.
- One-time setup: `node make.mjs install-emsdk` (git-clones `emsdk` at its pinned commit, installs pinned emsdk + cmake + ninja).
