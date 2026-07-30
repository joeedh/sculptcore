# Maya plugin integrating sculptcore

Feasibility report on wrapping sculptcore as an Autodesk Maya plugin (mesh
sculpting inside a Maya scene, backed by the engine that already ships in
the web app and the NW.js addon).

**Verdict:** feasible with a small, well-defined native shim. The engine's
public surface is already the right shape — a foreign-host C ABI whose
export list is pinned to the WASM ABI, an opaque-handle mesh built for
bulk import/export (Blender-layout arrays), and a stroke pipeline
explicitly designed to be driven by any host (see
[`strokeDriverGuide.md`](../strokeDriverGuide.md)). Maya integration is
new host + new UI, not new engine work.

The rest of this document surveys what exists, what has to be written on
the Maya side, and where the risks are.

---

## 1. What the engine already exposes

### 1.1 The native C ABI

Root `CMakeLists.txt` builds `sculptcore_capi` as a SHARED library
natively (Windows/macOS/Linux, clang or MSVC). Its exported symbol list
is the same `WASM_SYMBOLS` set the WASM link uses — so the native ABI
cannot drift from the WASM one, and every c-api the web app calls is
also callable from a Maya plugin.

Available c-api directories (all `extern "C"`, opaque handles):

- `source/mesh/c-api/` — mesh lifecycle, bulk array import/export
  (`Mesh_fromArrays` / `Mesh_toArrays` / `Mesh_arraySizes` in Blender
  layout: positions + corner_verts + face_offsets, so n-gons round-trip
  natively), typed attribute read/write across every domain
  (`VERTEX / EDGE / CORNER / LIST / FACE`) with `AttrUse` categories
  (UV, COLOR, POLYGROUP, SCULPT_LAYER, …), edge-boundary flags by
  vertex pair (seam/sharp), `Mesh_triangulate`, `Mesh_ngonFaceCount`,
  `Mesh_topoStamp` (monotonic edit counter — "unchanged since last
  export" means positions-only fast path), lz4 serialize/deserialize.
- `source/spatial/c-api/` — `Mesh_buildSpatialTree`, requested-attr /
  draw-shader plumbing, and `source/spatial/c-api/external_draw.h` — a
  purpose-built external-host draw provider (`sc_external_draw_register /
  _update / _enable_dynamic`, ABI version 2). Blender uses it today.
- `source/brush/c-api/gpu_brush_c_api.cc` — the GPU-compute brush seam
  (opaque `GpuBrushSession*`; upload blobs).
- `source/displace/c-api/` — sculpt-layer settings.
- `source/vdm/c-api/` — VDM texel-store splat/apply.
- `source/subdiv/c-api/` — multires level management, writeback,
  capture-to-VDM.
- `source/remesh/c-api/` — quad remesher.

**Gap for Maya:** there is no `CommandExecutor` c-api today. The web app
and the NW.js addon reach it through the litestl reflection surface
(`NapiRuntime::installExports`). A Maya plugin must either (a) add a
small `brush/c-api/brush_c_api.cc` wrapping the executor and the stroke
verbs the guide requires, or (b) embed the reflection runtime and drive
it directly from C++. (a) is the cheaper path and matches the existing
convention.

### 1.2 The stroke pipeline the plugin must drive

The engine does no symmetry, no camera, no spacing, no falloff-vs-filter
radius decisions. `documentation/strokeDriverGuide.md` is the ~500-line
host contract; a Maya plugin follows the same per-stroke skeleton every
other host uses:

```
exec.setStrokeGen(++gen); exec.setNonAccum(nonAccum); exec.setAnchoredGrab(...)
exec.beginStep(dyntopoEnabled)
  for each dab (host samples input, mirrors for symmetry):
    buildBrushProgram(prog, brushType, brush, radius, mesh)
    exec.applyDab(prog, center, normal, filterRadius, params?, seed)
    mesh.spatial.updateQueries()   // CPU half only — see §5
exec.commitPreviewDab(); exec.endDynTopoStroke(); exec.endStep()
```

Kernel-level policy is **queried, not hardcoded**: `BrushMetadata`
(`queryBrushFlags` / `queryAttrManifest` / `queriedAttrEntry`) reports
`@grabmode / @unbounded / @incremental / @relaxation / accumulable`
per brush enum value, and `filterRadiusFloor(brushType)` gives the
minimum filter radius. The Maya UI never branches on "is this the grab
brush" — it asks.

### 1.3 Undo

`meshlog::MeshLog` is attached with `exec.meshLog = log`. Every
`beginStep` opens an undo step; the host records `lastStepId()` right
after, and later maps that step onto Maya's undo queue via
`MeshLog::undo(mesh, spatial)` / `.redo(...)` and
`stepMemSize(stepId)` / `freeStep(stepId)` (guide §7 — otherwise C++
steps leak past Maya's queue).

### 1.4 Mesh model — quads/n-gons are first-class

Half-edge-style: `VERTEX / EDGE / CORNER / LIST / FACE` domains, with
n-gon face-loop lists and disk cycles. This is important for Maya
round-tripping — Maya artists work in quads, and `Mesh_toArrays` emits
`face_offsets` with per-face degree, so a sculpt session doesn't
force triangulation.

Dyntopo is triangles-only. If a stroke uses dyntopo, the plugin must
`Mesh_triangulate(mesh)` first (a one-way conversion for the duration of
that sculpt session); if it doesn't, quads/n-gons stay intact. This is
the same trade-off the web app makes.

Element indices are stable until freed (`freelist + freemap`, paged
storage). `Mesh_toArrays` optionally writes a `r_vert_map`
(engine-id → exported-index) so a plugin that keeps a Maya-side vertex
map can rebuild the Maya mesh across freelist gaps.

Attribute round-trip covers the Maya types artists expect:

| Maya           | sculptcore                                                   |
|----------------|--------------------------------------------------------------|
| UV set         | `CORNER` `FLOAT2` with `AttrUse::UV`                         |
| Color set      | `VERTEX` or `CORNER` `FLOAT4` with `AttrUse::COLOR`          |
| Face set / id  | `FACE` `INT` `"group"` with `AttrUse::POLYGROUP`             |
| Seam / sharp   | edge boundary flag via `Mesh_writeEdgeFlagsByVerts`          |
| Sculpt layer   | `VERTEX` `FLOAT3` with `AttrUse::SCULPT_LAYER`               |

### 1.5 Rendering is separable

`SpatialTree::updateQueries()` (the CPU half — split/merge, tris,
bounds, normals) is all the sculpt path needs. The GPU half
(`update(gpu)` with a `GPUManager*`) is only touched if the host wants
to piggyback on the engine's own draw path. A Maya plugin that lets
Maya render the mesh doesn't call it at all — build with
`WITH_VULKAN=OFF`, don't load `wgpu_native.dll`, treat sculptcore as a
headless mesh mutator, and push mutated positions/topology back to
Maya's viewport after each stroke via `MFnMesh`.

The `sc_external_draw_provider` API exists for the opposite choice —
render sculpt overlays through sculptcore's own GPU buffers — but that's
not the recommended starting point for Maya, where the viewport
(VP2/Viewport 2.0) has its own draw pipeline.

---

## 2. Maya-side architecture

### 2.1 Which Maya API

Maya offers three plugin surfaces:

- **C++ MPx API** — `MPxCommand`, `MPxNode`, `MPxSurfaceShape`,
  `MPxContext` / `MPxContextCommand` (tools), `MPxDrawOverride` (VP2
  draw). Full performance, direct pointer access to `MFnMesh` internals.
  Required for real-time sculpting.
- **Python API 2.0** (`maya.api.OpenMaya`) — same object model, ctypes
  the DLL for engine calls. Faster to prototype UI/settings; the sculpt
  hot path still has to be C++ because per-dab callbacks over Python
  destroy latency.
- **Bifrost graph** — not a fit for interactive sculpting.

**Recommendation:** one C++ plugin (`sculptcorePlugin.mll` /
`.bundle` / `.so`) that links `sculptcore_capi`. Python is layered on
top for menus and preferences (via the plugin's registered MEL/Python
commands), not for the stroke loop.

### 2.2 Object model

Three MPx classes cover the plugin:

- **`SculptcoreShape : MPxSurfaceShape`** — per-sculpt-object node. Owns
  one `Mesh*`, `SpatialTree*`, `MeshLog*`, and one `CommandExecutor*`.
  Draws through `MPxDrawOverride` (or `MPxSubSceneOverride`) by
  presenting the engine mesh to VP2 as a triangle list built from
  `Mesh_toArrays`. Serializes the sculpt state as an lz4 blob attribute
  (`serializeMesh`) so Maya files carry the sculpt data without a
  sidecar.
- **`SculptTool : MPxContext`** — the actual tool. Owns per-stroke state
  (spacer, symmetry, dab index, cached executor pointer from the
  active shape). Handles `doPress` / `doDrag` / `doRelease`, samples
  input into dabs (spacing runs host-side; `stroke_curve.h` in
  `newStrokeDriver.md` is on the roadmap to move this into C++, at
  which point the Maya plugin gets it for free).
- **`SculptcoreCmd : MPxCommand`** — MEL/Python surface for
  non-interactive operations (bake, apply-layer, remesh, dyntopo
  bracket, undo-metadata query). Every mutating command opens a Maya
  undo item that snapshots the `stepId` from `meshLog->lastStepId()`
  and calls `MeshLog::undo/redo` on undo.

The sculptcore mesh is the **source of truth** during a sculpt session.
Round-trip to Maya's `MFnMesh` happens on session start (import from
Maya) and on demand (bake back to Maya's DAG) — not per stroke.

### 2.3 Session boundaries

- **Enter sculpt mode on a Maya mesh**: read Maya mesh + UV sets + color
  sets + face sets + creases into an engine mesh via `Mesh_fromArrays`
  + the typed attribute writers, build spatial tree, create executor.
  Hide the Maya mesh; show the sculptcore shape.
- **During sculpt**: stroke loop mutates the engine mesh; Maya viewport
  gets refreshed frame from the `MPxDrawOverride`.
- **Exit sculpt / bake**: `Mesh_toArrays` + `Mesh_readAttr` back into a
  new `MFnMesh` (honor `r_vert_map` — engine freelist gaps mean the
  vertex-count-on-import may differ from vertex-count-on-export). Push
  onto the Maya mesh's construction history as a `sculptcoreBake` DG
  node so the operation is redoable in Maya's usual sense.

---

## 3. Data round-trip: Maya ↔ sculptcore

### 3.1 Import (Maya → engine)

`MFnMesh` gives positions (`getPoints`), face vertex counts + indices
(`getVertices`), UV sets (`getUVs` per set), color sets
(`getFaceVertexColors`), face-shading groups (`getConnectedShaders`).
Map straight onto `Mesh_fromArrays` — Maya's polygon layout already
matches Blender's `corner_verts + face_offsets` shape:

```cpp
MIntArray faceCounts, faceConnects;
fnMesh.getVertices(faceCounts, faceConnects);
// face_offsets is running-sum of faceCounts, +1 sentinel
Mesh* m = createMesh();
Mesh_fromArrays(m, positions, vertCount,
                faceConnects.data(), faceConnects.length(),
                faceOffsets.data(), faceCounts.length());
```

Then per UV set: `Mesh_writeCornerFloat2Attr(m, setName, uvs)`.
Per color set: `Mesh_writeVertFloat4Attr(m, setName, colors)`.
Face sets from Maya face-shading-group id: `Mesh_writeFaceIntAttr(m,
"group", ids)`. Seam/sharp edges from Maya crease/hard-edge flags via
`Mesh_writeEdgeFlagsByVerts`.

### 3.2 Export (engine → Maya)

`Mesh_arraySizes` first (returns vert/corner/face counts + the
`r_vert_map` size). Allocate, `Mesh_toArrays` fills them, hand to
`MFnMesh::create`. Read attributes back per-domain and rebuild the UV
sets / color sets / face-shading groups. `r_vert_map[engine_id]` gives
the exported index — cache this on the Maya-side shape so the plugin
can detect "positions-only changed" (via `Mesh_topoStamp` unchanged)
and use `MFnMesh::setPoints` alone instead of a full mesh rebuild.

### 3.3 Persistence

Sculpt state lives on the `SculptcoreShape` as a binary attribute
(`serializeMeshRaw` → base64 or `MFnBinaryData`). Loading a `.ma`/`.mb`
file calls `deserializeMesh` to reconstruct the engine mesh without
re-importing from any external source.

---

## 4. Undo integration

Maya's undo model is per-command. Every dab is *not* a Maya undo item —
that's too granular and would flood Maya's queue. Group per-stroke:

```
SculptTool::doRelease() {
    // stroke just ended
    int stepId = meshLog->lastStepId();
    MGlobal::executeCommand("sculptcoreUndoStep " + stepId);
}
```

`sculptcoreUndoStep` is a Maya command whose `doIt` is a no-op (the
mutation already happened), whose `undoIt` calls `MeshLog::undo(mesh,
spatial)`, and whose `redoIt` calls `MeshLog::redo`. On destruction of
the command (Maya's undo queue evicting it), call
`MeshLog::freeStep(stepId)` — this is the guide §7 rule; without it
sculptcore's log grows unbounded past Maya's undo horizon.

Dyntopo strokes emit one `LogChunkTopo` per dab (large); non-dyntopo
paint/position strokes emit sparse `LogChunkElems` swaps. Both are
covered by the same `undo/redo` pair.

---

## 5. Rendering in the Maya viewport

Two options. The recommended one first.

### 5.1 Recommended: draw through VP2 (Maya renders)

`MPxSubSceneOverride` builds a `MRenderItem` for the sculpt mesh; the
plugin updates it after each stroke (or each frame, coalescing dabs).
Vertex buffer comes from `Mesh_toArrays` positions; index buffer from
triangulated corners. Normals come from an engine `.n.vertex` attribute
if present, else recomputed host-side.

Cost: one `Mesh_toArrays` per frame during active sculpt. For 5 M-tri
meshes this is CPU-bound but well within interactive budget when only
the changed spatial nodes are re-uploaded — expose a
"dirty-node-bounds" query on the spatial tree (`SpatialTree` already
tracks per-node dirty flags for its own GPU half) and only re-upload
affected regions.

### 5.2 Alternative: engine-owned draw via `sc_external_draw_provider`

Register the plugin as an external draw provider (Blender's path). This
shares the engine's own aggregated VBOs and skips the per-frame
`Mesh_toArrays`. The catch is that the plugin must expose a VP2
`MPxDrawOverride` that binds engine buffer handles into VP2's command
stream — this is possible (Maya lets you pass raw device buffers into
`MHWRender::MRenderItem`) but adds a hard dependency on `wgpu_native`
being loaded in the Maya process and interoperating with Maya's D3D11 /
OpenGL / Metal backend. **Not recommended for the MVP.** Defer until
after the plain VP2 path is shipping and only revisit if the per-frame
readback shows up in a profile.

---

## 6. Threading and reentrancy

- `CommandExecutor::exec` uses `litestl::task::parallel_for` internally.
  A stroke dab may run on many worker threads. Maya's own event loop
  is on the main thread — never call MFnMesh from inside the engine's
  worker threads.
- **One executor per shape.** Concurrent calls into the same
  `CommandExecutor` are not safe. `beginStep`/`endStep` bracketing is
  not reentrant.
- **Process-global init once.** `initBindings()` populates the
  binding-manager singleton; call once from the plugin's
  `initializePlugin`. `litestl::alloc` tracks allocations globally —
  fine, but Maya has its own allocator; keep them separate (don't
  route sculptcore through `MAllocator`).

Maya's "batch mode" (`mayapy`, no UI) works out of the box — the sculpt
tool is unavailable but bake/remesh/dyntopo commands run headlessly.

---

## 7. Build and distribution

### 7.1 Build

Maya plugins are per-Maya-version, per-platform DLLs linked against the
Maya devkit for that version. The sculptcore side already builds a
shared `sculptcore_capi.{dll,dylib,so}` under clang on all three
platforms and additionally under MSVC on Windows
(`WITH_NATIVE_MSVC`) — Maya on Windows is MSVC-built, so the plugin
DLL itself must be MSVC. Sculptcore's MSVC path (dedicated
`build/native-msvc`, MSVC's `libomp.lib` + staged `libomp140.x86_64.dll`
for the LLVM-OpenMP references in the clang-built OpenBLAS/CHOLMOD deps)
is exactly the story a Maya plugin needs.

New CMake target: `sculptcoreMayaPlugin` under `source/maya/` (new
directory), links `sculptcore_capi` and Maya's `OpenMaya`, `OpenMayaUI`,
`OpenMayaAnim`, `MetaData`, `Foundation`. Gated behind a `WITH_MAYA=ON`
option that finds Maya via `${MAYA_LOCATION}`.

### 7.2 Runtime files shipped with the plugin

- `sculptcoreMayaPlugin.mll` (Windows) / `.bundle` / `.so`
- `sculptcore_capi.dll` and its deps: `wgpu_native.dll` **only if the
  external-draw-provider path is used** (§5.2). For the recommended
  VP2 path, drop wgpu-native entirely.
- OpenBLAS + CHOLMOD DLLs from `sculptcore-deps` **only if the quad
  remesher is exposed** (`source/remesh` is the only CHOLMOD linker).
  For an MVP that ships sculpt + dyntopo but not quad remesh, both DLLs
  can be omitted. `libomp140.x86_64.dll` still needed on Windows for the
  OpenBLAS/CHOLMOD open-MP references — check if MSVC's redistributable
  covers it; if not, ship it.

### 7.3 Versions

Target Maya 2024 and 2025 for MVP (Python 3.10/3.11, VP2 required, C++17
devkit — sculptcore is C++20 but the plugin can compile as C++20 against
Maya's C++17 headers with `/std:c++20` on MSVC and `-std=c++20` on clang;
this is standard for modern Maya plugins).

---

## 8. Feature scope

### 8.1 MVP (~1–2 months)

- Enter/exit sculpt mode on a Maya polygon mesh.
- Import UV sets, color sets, face sets, seams into the engine.
- Sculpt tool context with symmetry + 3-4 core brushes (draw, inflate,
  smooth, grab).
- Undo/redo mapped through `MeshLog`.
- Bake back to Maya mesh with attributes preserved.
- File save/load (sculpt state on the shape node).
- VP2 draw override (recommended path — Maya renders).

### 8.2 Phase 2

- Full brush roster (all sbrush-defined kernels — reflection makes this
  a UI-list generation, not a per-brush code change).
- Dyntopo bracketing (Maya UI toggle, triangulate-on-enter warning).
- Sculpt layers (`source/displace/`).
- Multires (`source/subdiv/`), including capture-to-VDM.

### 8.3 Phase 3

- Quad remesh (`source/remesh/`) — adds CHOLMOD/OpenBLAS runtime cost.
- VDM baking to Maya displacement shader.
- Engine-owned draw (§5.2) if the VP2 path is a bottleneck.
- GPU-compute brush path (`gpu_brush_c_api`) — needs wgpu-native
  interop with Maya's device.

---

## 9. Risks and open questions

1. **`CommandExecutor` has no c-api yet.** Either add `source/brush/c-api/
   brush_c_api.cc` (small, mirrors the shape of `mesh_c_api.cc` —
   opaque handles for `CommandExecutor`, `Brush`, `BrushProgram`,
   thin wrappers over `beginStep / applyDab / endStep / previewDab /
   setNonAccum / setAnchoredGrab / setStrokeGen`), or embed the
   litestl reflection runtime. **Do (a).** Plumbing work, not a
   redesign; needed anyway for any non-JS non-Python host.

2. **Stroke sampler duplication.** Today every host reimplements
   sampling on top of `stroke_spacing.h` (the web app in TypeScript, the
   headless debug controller in C++). `documentation/plans/newStrokeDriver.md`
   is the plan to move the sampler into C++ behind the binding system.
   A Maya plugin either implements its own sampler for MVP (~150 lines,
   ports the existing TS logic) or waits for that plan to land. Doing
   both in parallel is fine — the plugin's sampler code is disposable.

3. **Maya undo memory.** `MeshLog` step sizes are not bounded by
   Maya's undo-queue-length preference. A user with 200 undo levels and
   a 5 M-tri dyntopo stroke can consume gigabytes. The plugin should
   observe `MeshLog::stepMemSize` and expose a "sculptcore undo memory
   budget" preference that evicts oldest sculptcore steps independently
   of Maya's queue (guide §7 mentions the same trade-off).

4. **Attribute domain mismatches.** Maya stores colors either per-vertex
   or per-face-vertex, per set. sculptcore supports both. Face sets in
   Maya are more of a convention (shading groups + sets) than a
   first-class attribute; the plugin picks one and documents it. UVs
   are straightforward (per-corner both sides).

5. **Save-file portability.** `serializeMesh` is versioned but not
   guaranteed stable across every sculptcore release. Maya scenes
   outlive plugin versions. Either commit to backward compatibility on
   `deserializeMesh` (already the intent per `mesh_c_api.h`) or add a
   scene-migration path that re-imports from a stashed Maya mesh
   snapshot if the sculpt blob fails to deserialize.

6. **License and distribution.** Maya's devkit + plugin distribution
   have their own licensing story. Sculptcore's license and its
   third-party deps (OpenBLAS BSD, SuiteSparse/CHOLMOD LGPL/GPL) need a
   compatibility review before a plugin ships publicly. LGPL/GPL of
   CHOLMOD in particular is the reason Phase 3 (quad remesh) is a
   separate release stage — a Phase 1/2 build with CHOLMOD absent
   dodges the question.

7. **Threading collision with Maya's evaluator.** Maya's parallel
   evaluation manager may schedule DG evaluations off the main thread.
   The plugin must serialize all engine calls for one shape onto one
   thread (Maya's evaluation graph respects
   `MPxNode::setDoNotWrite`-style hints for this — spell it out
   explicitly on `SculptcoreShape`).

---

## 10. Summary

The engine ships the right primitives for a Maya plugin already: a
stable native C ABI pinned to the WASM ABI, a mesh format that
round-trips Maya quads and attributes without loss, a stroke pipeline
whose host contract is documented and already followed by the Blender
addon and the web app, and a rendering coupling that is optional so
Maya can keep viewport ownership. The Maya-side work is one MPxShape +
one MPxContext + one MPxCommand, plus a small `brush_c_api.cc` on the
engine side to close the last exposed gap. MVP is scoped at 1-2 months;
dyntopo, multires and remesh follow as additive phases; the GPU-compute
brush path is a distant Phase 3 that requires solving wgpu-native ↔ VP2
device interop and is not on the critical path.
