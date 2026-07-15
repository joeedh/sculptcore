import {
  INeededWasm,
  pointer,
  BindingManager as WasmBindingManager,
  createWasmHelpers,
  IWasmBase,
  int,
  createWasmMemory,
} from '@litestl/typescript-runtime'
import type {AllBoundTypes, float2, float3, GPUManager, Mesh, Multires, SpatialTree, VdmStore} from '../index'

import {BindingManager} from './manager'
import {loadNativeAddon, nativeBackendRequested} from './nativeBackend'
import {buildNativeManager, makeNativeInterface} from './nativeManager'

interface IWasmMethods extends IWasmBase {
  getBindingManager(): pointer
  initBindings(): void
  /** create a gridded cube with `dimen` x `dimen` quads on each of the six cubic faces.*/
  Mesh_createCube(dimen: int, size: number, sphereFac: number): Mesh
  /** create an all-quad UV sphere: `rings` latitudinal bands, `segs` longitudinal
   * segments, `radius`. Poles are the only singularities — the remesh-friendly
   * primitive the quad-remesh parity test drives. */
  Mesh_makeUVSphere(rings: int, segs: int, radius: number): Mesh
  /** flat XY quad grid facing +Z: nx*ny verts spanning [-size/2, size/2] (add-plane primitive). */
  Mesh_makeGrid(nx: int, ny: int, size: number): Mesh
  /** build a coarse BVH over `mesh`'s faces; pass leafLimit<=0 to keep the default. */
  Mesh_buildSpatialTree(mesh: Mesh, leafLimit: int, depthLimit: int, gpuTriTarget: int): SpatialTree
  SpatialTree_free(tree: SpatialTree): void
  getSpatialShaders(): pointer

  // Raw mesh-serialization exports (mesh/c-api/mesh_c_api.cc). Pointer-level;
  // the `Mesh_serialize`/`Mesh_deserialize`/`Mesh_free` helpers on
  // IWasmInterface wrap these with heap marshalling.
  /** serialize `mesh` into a fresh malloc'd blob; writes the byte count to `outSizePtr` (int*). Free the returned ptr with `freeMeshBuffer`. */
  serializeMesh(mesh: pointer, outSizePtr: pointer): pointer
  /** like `serializeMesh` but writes only the uncompressed column payload (autosave worker compresses off-thread). */
  serializeMeshRaw(mesh: pointer, outSizePtr: pointer): pointer
  /** reconstruct a Mesh from a `serializeMesh` blob; returns a `Mesh*`. */
  deserializeMesh(dataPtr: pointer, size: int): pointer
  /** free a Mesh (`alloc::Delete`) — created by `Mesh_createCube`/`deserializeMesh`. */
  freeMesh(mesh: pointer): void
  /** free a blob returned by `serializeMesh`. */
  freeMeshBuffer(buf: pointer): void
  /** fan-triangulate every n-gon of `mesh` in place (n_ngon_faces -> 0). The
   * arg is a `Mesh*` (the high-level helper unwraps it); rebuild any spatial
   * tree built over the mesh afterwards. */
  Mesh_triangulate(mesh: Mesh): void
  /** live n-gon (>3-sided face) count; 0 == all-triangles. Gates the
   * triangulate button / dyntopo tip with no face scan. */
  Mesh_ngonFaceCount(mesh: Mesh): number
  /** quad-remesh `mesh` into a fresh all-quad Mesh (input untouched); `params`
   * is a bound `sculptcore::remesh::RemeshParams`. Returns `undefined` on a
   * clean failure (Gauss-Bonnet-infeasible field / >10% folded faces). Free the
   * result with `Mesh_free`. The high-level helper unwraps both handles per
   * backend (WASM → numeric `.ptr`; native → the wrapper). */
  Mesh_quadRemesh(mesh: Mesh, params: SculptHandle): Mesh | undefined

  // VDM engine seam (vdm/c-api/vdm_c_api.cc; displacementAndSubSurf.md V3).
  // Pointer-level C exports; the same-named IWasmInterface helpers wrap them
  // with handle unwrapping so both backends stay drop-ins.
  /** fresh VdmStore (sparse tiled float3 texel store); pass <= 0 to keep a default. */
  VdmStore_new(resolution: int, tileSize: int): VdmStore
  /** free a VdmStore created by `VdmStore_new`. */
  VdmStore_free(store: VdmStore): void
  /** splat one VDM dab (tangent-space texels only, no vertex moves); returns texels touched. */
  Mesh_vdmSplatDab(
    mesh: Mesh,
    tree: SpatialTree,
    store: VdmStore,
    cx: number,
    cy: number,
    cz: number,
    nx: number,
    ny: number,
    nz: number,
    radius: number,
    strength: number,
    alpha: number,
    invert: int
  ): int
  /** texelsClamped of the most recent Mesh_vdmSplatDab (the X1 add-a-level prompt signal). */
  Vdm_lastSplatClamped(): int
  /** logged splat: the tile-delta rides `meshLog`'s open MeshLog step as a VdmLogChunk. */
  Mesh_vdmSplatDabLogged(
    mesh: Mesh,
    tree: SpatialTree,
    store: VdmStore,
    meshLog: SculptHandle,
    cx: number,
    cy: number,
    cz: number,
    nx: number,
    ny: number,
    nz: number,
    radius: number,
    strength: number,
    alpha: number,
    invert: int
  ): int
  /** VDM -> geometry extraction (X4): displace verts by the store's field, optionally clear it; returns verts moved. */
  Mesh_vdmApplyToVerts(mesh: Mesh, store: VdmStore, clearStore: int): int
  /** raw store serialize (v2 container): malloc'd blob + byte count to `outSizePtr`; wrapped by `VdmStore_serializeBlob`. */
  VdmStore_serialize(store: pointer, outSizePtr: pointer): pointer
  /** raw store rebuild from a blob; wrapped by `VdmStore_deserializeBlob`. */
  VdmStore_deserialize(dataPtr: pointer, size: int): pointer
  /** raw in-place store refill; wrapped by `VdmStore_restoreFromBlob`. */
  VdmStore_restoreBlob(store: pointer, dataPtr: pointer, size: int): int
  /** tag every live face's `.detail.carrier` (0 = GEOM, 1 = VDM). */
  SpatialTree_fillDetailCarrier(tree: SpatialTree, carrier: int): void
  /** recompute vertex normals + the F3 frames — required before splatting. */
  Mesh_updateFrames(mesh: Mesh): void

  // Sculpt-layer settings mutators (displace/c-api/displace_c_api.cc; V5).
  // Each keeps evaluated v.co current; re-applying the previous value is the
  // undo. Reads go through the bound Mesh sculptLayer* methods.
  /** set layer `li`'s weight (co += Δw·d over all live verts). */
  Mesh_layerSetWeight(mesh: Mesh, li: int, weight: number): void
  /** enable/disable layer `li` (its contribution is added/subtracted from co). */
  Mesh_layerSetEnabled(mesh: Mesh, li: int, enabled: int): void
  /** freeze/unfreeze layer `li` (excluded from brush writes, still composited). */
  Mesh_layerSetFrozen(mesh: Mesh, li: int, frozen: int): void
  /** remove layer `li`: subtract its contribution, drop settings row + column. */
  Mesh_layerRemove(mesh: Mesh, li: int): void
  /** make layer `li` the edit target (V2; -1 clears). Enables + pins weight 1;
   * returns the resulting target index (-1 when cleared/invalid/frozen). */
  Mesh_setActiveEditLayer(mesh: Mesh, li: int): int
  /** fold the edit target's delta from evaluated positions (idempotent no-op
   * without a target). Read the target via the bound sculptLayerEditTarget. */
  Mesh_layerFold(mesh: Mesh): void

  // Multires seam (subdiv/c-api/subdiv_c_api.cc; displacementAndSubSurf S).
  // Pointer-level C exports; the same-named IWasmInterface helpers wrap them
  // with handle unwrapping so both backends stay drop-ins.
  /** build a multires stack over `cage` (not owned; must outlive the stack); tree params 0 = defaults. */
  Multires_new(cage: Mesh, levels: int, leafLimit: int, depthLimit: int, gpuTriTarget: int): Multires
  /** free a stack created by `Multires_new` (never frees the cage). */
  Multires_free(mr: Multires): void
  /** write back the outgoing level, activate `level` (clamped); returns the active level. */
  Multires_setActiveLevel(mr: Multires, level: int): int
  /** the active level's mesh — a NON-owning view of the stack's slot (never free). */
  Multires_activeMesh(mr: Multires): Mesh | undefined
  /** the active level's spatial tree — a NON-owning view (never free). */
  Multires_activeTree(mr: Multires): SpatialTree | undefined
  /** fold the level's resident edits into the grids store; returns changed verts. */
  Multires_writeback(mr: Multires, level: int): int
  /** least-squares refit of level−1 to `level`'s surface; returns changed level−1 verts. */
  Multires_downRefit(mr: Multires, level: int): int
  /** geometry -> VDM capture: move `level`'s grids disp into the Ptex store's texels; returns texels written. */
  Multires_captureToVdm(mr: Multires, vstore: VdmStore, level: int): int
  /** raw grids-store serialize: malloc'd blob + byte count to `outSizePtr`; wrapped by `Multires_storeBlob`. */
  Multires_serializeStore(mr: pointer, outSizePtr: pointer): pointer
  /** raw grids-store restore; wrapped by `Multires_restoreStoreBlob`. */
  Multires_restoreStore(mr: pointer, dataPtr: pointer, size: int): int

  // M5 requested-attribute bridge (spatial/c-api/spatial_c_api.cc). Pointer-level
  // C exports; the `SpatialTree_setRequestedAttrs`/`setDrawShader`/
  // `getMissingAttrSlots` helpers on IWasmInterface wrap these with heap
  // marshalling. Strings + JS arrays can't cross the generic method binding, so
  // the requested set routes through these dedicated calls (like the Mesh_*
  // factories) rather than `tree.setRequestedAttrs(...)`.
  /** install the requested attr set: `count` entries; names '\n'-joined in `namesPtr`; each int array (`count` ints) in slot order. */
  setTreeRequestedAttrs(
    tree: pointer,
    count: int,
    namesPtr: pointer,
    srcTypesPtr: pointer,
    elemSizesPtr: pointer,
    slotsPtr: pointer,
    domainsPtr: pointer,
    defaultKindsPtr: pointer
  ): void
  /** set the material WGSL for the requested-attr draw shader. */
  setTreeDrawShader(tree: pointer, wgslPtr: pointer): void
  /** copy the advisory missing-slot list into `outPtr` (cap `maxOut`); returns the full count. Pass (0,0) to query the count. */
  getTreeMissingAttrSlots(tree: pointer, outPtr: pointer, maxOut: int): int
  /** force a per-attribute buffer rebuild against current mesh layers (layer add/remove with byte-identical descriptors). */
  refreshTreeRequestedAttrs(tree: pointer): void
}

/**
 * One geometry attribute a material's shader reads, in the backend-agnostic
 * shape the `SpatialTree_setRequestedAttrs` bridge marshals. Built by the
 * renderengine (M6) from the shader graph's `RequestedAttrDesc`; sculptcore
 * builds one vertex buffer per entry, default-filling absent layers.
 */
export interface RequestedAttrBridge {
  /** Source attribute name on the mesh (e.g. `uv`, `color`). */
  name: string
  /** sculptcore `AttrType` bitflag (FLOAT2=2, FLOAT3=4, FLOAT4=8). */
  srcType: number
  /** Component count (2 / 3 / 4). */
  elemSize: number
  /** Vertex `@location` slot (2 + index; after position@0 / normal@1). */
  slot: number
  /** `AttrDomain` flag the layer lives on: VERTEX=1, EDGE=2, CORNER=4, FACE=16. */
  domain: number
  /** Missing-layer fill: 0 = zero (default), 1 = white. */
  defaultKind?: number
}
/**
 * An opaque sculptcore object reference. The WASM backend represents it as a
 * numeric heap pointer; the native (N-API) backend as a wrapped C++ object.
 * App code must treat it as opaque — never read `.ptr` as a number — and pass
 * it back through the backend-agnostic helpers on `IWasmInterface`
 * (`getBoundVector`, the `Mesh_*`/`SpatialTree_free` factories, …) so neither
 * backend's pointer representation leaks across the boundary.
 */
export type SculptHandle = object

/** GpuBrush_info selectors — hand-mirror of GpuBrushInfoWhich in
 * source/brush/gpu_brush_session.h; keep the two in sync. */
export const GpuBrushInfo = {
  ELEM_COUNT         : 0,
  NEEDS_NEIGHBORS    : 1,
  WRITES_MASK        : 2,
  WRITES_COLOR       : 3,
  ACCUMULABLE        : 4,
  READS_VCLASS       : 5,
  FACE_MODE          : 6,
  IS_GLOBAL          : 7,
  /** builds the normal topology on first query */
  TRI_COUNT          : 8,
  UVERTS_CHANGED     : 9,
  NODE_COUNT         : 10,
  UNIQUE_COUNT       : 11,
  STROKE_SAMPLE_COUNT: 12,
  DAB_GEN            : 13,
  /** scatter-table cache key (SpatialTree::gpuLayoutGen); builds scatter meta */
  GPU_LAYOUT_GEN     : 14,
  SCATTER_NODE_COUNT : 15,
} as const

/** GpuBrush_data selectors — hand-mirror of GpuBrushDataWhich in
 * source/brush/gpu_brush_session.h; keep the two in sync. The blobs are
 * already in GPU layout per compute_layout.h — upload verbatim. */
export const GpuBrushData = {
  CO            : 0,
  NO            : 1,
  MASK          : 2,
  NBR_META      : 3,
  NBR_VERTS     : 4,
  TRI_VERTS     : 5,
  VERT_TRI_META : 6,
  VERT_TRI_LIST : 7,
  UVERTS        : 8,
  NODE_META     : 9,
  BRUSH_UNIFORMS: 10,
  CTX_UNIFORMS  : 11,
  FALLOFF_LUT   : 12,
  STROKE_PATH   : 13,
  /** live mesh positions, re-packed per query (shadow-verify) */
  LIVE_CO       : 14,
  /** u32×6 per GPU node: pos/nor buffer keys (lo,hi) + corner offset,count */
  SCATTER_META  : 15,
  /** u32 per render corner: global vert id, in fill_leaf order (lazy build) */
  SCATTER_MAP   : 16,
  /** u32 meta indices of owners hit by the last marshalDab */
  TOUCHED_OWNERS: 17,
  /** f32 per vertex: cavity automask factor (binding 24); identity 1.0 when off */
  AUTOMASK      : 18,
} as const

/** Raw pointer-level view of the GpuBrush_* C exports (WASM only; unprefixed
 * onto the module by createWasmHelpers). The backend-agnostic wrappers on
 * IWasmInterface are the surface app code uses. */
interface IGpuBrushRaw {
  GpuBrush_beginStroke(mesh: number, tree: number, brush: number, meshLog: number, tool: number): number
  GpuBrush_free(session: number): void
  GpuBrush_kernelName(session: number): number
  GpuBrush_info(session: number, which: number): number
  GpuBrush_marshalDab(
    session: number,
    cx: number,
    cy: number,
    cz: number,
    nx: number,
    ny: number,
    nz: number,
    radius: number,
    filterRadius: number,
    mirrorIdx: number,
    nonaccum: number
  ): number
  GpuBrush_dataSize(session: number, which: number): number
  GpuBrush_dataPtr(session: number, which: number): number
  GpuBrush_applyCo(session: number, co: number, elemCount: number): void
  GpuBrush_endStroke(session: number, co: number, no: number, elemCount: number): void
}

export interface IWasmInterface extends INeededWasm, IWasmMethods {
  manager: BindingManager
  gpu: GPUManager

  /**
   * Wrap a bound `litestl::util::Vector` as an array-like (`.length`, numeric
   * index, iterator). Backend-agnostic: pass the bound vector handle itself,
   * not its `.ptr` — WASM unwraps the numeric pointer internally, native
   * forwards the wrapper.
   */
  getBoundVector(vecTypeName: string, bound: SculptHandle): unknown

  /**
   * Serialize a mesh to a versioned, lz4hc-compressed blob (the C++
   * `serial::writeMesh` format). Backend-agnostic: WASM copies out of the
   * linear-memory heap, native copies out of a sandbox-internal ArrayBuffer.
   */
  Mesh_serialize(mesh: Mesh): Uint8Array
  /**
   * Serialize a mesh to the *uncompressed* column payload only (no lz4 step, no
   * `SCULPT00` header). The autosave worker compresses + frames this off the
   * main thread via the JS lz4 codec (`scripts/util/lz4.ts`), reproducing the
   * `Mesh_serialize` container byte-for-byte. Backend-agnostic like the above.
   */
  Mesh_serializeRaw(mesh: Mesh): Uint8Array
  /** Reconstruct a mesh from a `Mesh_serialize` blob. */
  Mesh_deserialize(bytes: Uint8Array): Mesh

  /** Geometry -> VDM capture (X4): transfer `level`'s grids-store disp into
   * the Ptex VDM texels (added, same frame space), zero the disp, drop the
   * surface onto the smooth base. Returns texels written; caller owns undo
   * snapshots + the spatial refresh. */
  Multires_captureToVdm(mr: Multires, vstore: VdmStore, level: int): int
  /** Grids-store blob of a multires stack (undo seam for down-refit / stack
   * delete). Backend-agnostic copy semantics like `Mesh_serialize`. */
  Multires_storeBlob(mr: Multires): Uint8Array
  /** Replace a stack's grids store from a `Multires_storeBlob` blob (same cage
   * topology); invalidates every level — re-set the active level after.
   * Returns success. */
  Multires_restoreStoreBlob(mr: Multires, bytes: Uint8Array): boolean

  /** The interactive VDM splat: like `Mesh_vdmSplatDab`, but the tile-delta is
   * appended to `meshLog`'s OPEN step as a VdmLogChunk, so the stroke's undo
   * press reverts the dab's texels. Returns texels touched. */
  Mesh_vdmSplatDabLogged(
    mesh: Mesh,
    tree: SpatialTree,
    store: VdmStore,
    meshLog: SculptHandle,
    cx: number,
    cy: number,
    cz: number,
    nx: number,
    ny: number,
    nz: number,
    radius: number,
    strength: number,
    alpha: number,
    invert: int
  ): int
  /** VDM -> geometry extraction (X4 bake): refresh frames, displace every
   * vertex by the store's field at its own param (bake = render), optionally
   * clear the store. Returns verts moved; the caller owns undo snapshots and
   * the spatial/tree refresh. */
  Mesh_vdmApplyToVerts(mesh: Mesh, store: VdmStore, clearStore: int): int
  /** VdmStore blob (v2 container; params + Ptex tables ride it). Undo seam for
   * the app's store-delete op. Empty result = failure. */
  VdmStore_serializeBlob(store: VdmStore): Uint8Array
  /** Rebuild a store from a `VdmStore_serializeBlob` blob (fresh handle the
   * caller owns; free with `VdmStore_free`). */
  VdmStore_deserializeBlob(bytes: Uint8Array): VdmStore | undefined
  /** Refill an EXISTING store from a blob (instance kept — MeshLog chunks
   * hold non-owning pointers into it). Returns success. */
  VdmStore_restoreFromBlob(store: VdmStore, bytes: Uint8Array): boolean
  /**
   * Free a mesh handle (allocator-correct: routes to the C++ `alloc::Delete`
   * disposer). Do NOT free meshes via `[Symbol.dispose]` — that path is absent
   * natively and mismatches the engine allocator on WASM.
   */
  Mesh_free(mesh: Mesh): void

  /**
   * Install the material's requested attribute set on a spatial tree (M5/M6).
   * Backend-agnostic: marshals `reqs` into the flat parallel-array wire format
   * (a '\n'-joined name string + Int32Arrays) and calls the dedicated
   * `setTreeRequestedAttrs` C export. Never throws — an empty/bad set yields the
   * legacy single-color path. Pass the tree handle itself, not its `.ptr`.
   */
  SpatialTree_setRequestedAttrs(tree: SpatialTree, reqs: RequestedAttrBridge[]): void
  /**
   * Set the material WGSL the tree's draw batches render with (M5/M6). C++
   * rebuilds + links the tree ShaderDef and flags leaves for a GPU regen.
   */
  SpatialTree_setDrawShader(tree: SpatialTree, wgsl: string): void
  /**
   * Read the advisory list of requested slots with no matching mesh layer
   * (default-filled). Returns a plain `number[]`; never throws.
   */
  SpatialTree_getMissingAttrSlots(tree: SpatialTree): number[]
  /**
   * Force a rebuild of the per-attribute vertex buffers against the *current*
   * mesh layers even when the requested descriptor set is byte-identical (a
   * layer add/remove whose domain matches the category default). The
   * renderengine calls this — instead of re-issuing `SpatialTree_setDrawShader`
   * — when only the mesh's attribute layers changed. Never throws.
   */
  SpatialTree_refreshRequestedAttrs(tree: SpatialTree): void

  /**
   * GPU brush-stroke seam (documentation/plans/gpuGlobalBrushes.md §3): open a
   * stroke session over the C++ marshal (source/brush/gpu_brush_c_api.cc). The
   * MeshLog step must already be open (executor.beginStep). Returns undefined
   * when the tool has no GPU kernel. Backend-agnostic; the session handle is
   * opaque on both backends.
   */
  GpuBrush_beginStroke(
    mesh: Mesh,
    tree: SpatialTree,
    brush: SculptHandle,
    meshLog: SculptHandle,
    tool: int
  ): SculptHandle | undefined
  /** Free a session without touching the mesh (abort before any dab landed). */
  GpuBrush_free(session: SculptHandle): void
  /** The session's kernel stem — the `brushWgsl` key to dispatch. */
  GpuBrush_kernelName(session: SculptHandle): string
  /** Query a `GpuBrushInfo` selector. */
  GpuBrush_info(session: SculptHandle, which: int): int
  /**
   * Marshal one dab image: node filter at `filterRadius` (caller applies the
   * CPU path's widened-radius policy), undo snapshots, and the per-dab upload
   * blobs. `mirrorIdx` 0 = primary image (advances the grab dab generation).
   * Returns the workgroup count to dispatch (0 = nothing to do).
   */
  GpuBrush_marshalDab(
    session: SculptHandle,
    cx: number,
    cy: number,
    cz: number,
    nx: number,
    ny: number,
    nz: number,
    radius: number,
    filterRadius: number,
    mirrorIdx: int,
    nonaccum: int
  ): int
  /**
   * A marshaled blob (`GpuBrushData` selector) ready to upload verbatim.
   * WASM returns a heap view — consume it before the next wasm call (a heap
   * growth invalidates it); native returns a sandbox copy. Empty (length 0)
   * when the blob is absent — never throws.
   */
  GpuBrush_data(session: SculptHandle, which: int): Uint8Array
  /**
   * Per-dab readback apply: write read-back positions into the mesh and dirty
   * the nodes marshaled since the last apply (undo snapshots already taken at
   * marshal time). `co` is packed xyz for every element.
   */
  GpuBrush_applyCo(session: SculptHandle, co: Float32Array): void
  /**
   * Close the stroke: snapshot + write the final positions (pass null when
   * per-dab applies already landed them), dirty every touched node, and free
   * the session. Caller then runs spatial.update + executor.endStep as on the
   * CPU path.
   */
  GpuBrush_endStroke(session: SculptHandle, co: Float32Array | null, no: Float32Array | null): void

  /**
   * Native-backend bulk-data read: the bytes a bound object's raw-pointer
   * member points at (e.g. gpu::Buffer.data). Absent on WASM, which reads the
   * linear-memory heap (`HEAPU8`) directly. A copy under the V8 sandbox.
   */
  pointerBytes?(bound: SculptHandle, member: string, byteLen: number): Uint8Array | undefined
  /**
   * Native-backend stable identity key for a bound object (its C++ address,
   * never dereferenced). Absent on WASM, which uses the numeric `.ptr`.
   */
  objectAddress?(bound: SculptHandle): number | undefined

  /** uses a large cache ring */
  float3(src: ArrayLike<number | undefined>): float3
  /** uses a large cache ring */
  float2(src: ArrayLike<number | undefined>): float2
}

let wasmPromise: Promise<IWasmInterface> | undefined = undefined
let wasm: IWasmInterface | undefined

// A desktop shell (NW.js / Electron) exposes `process`, but its renderer is a
// browser context with fetch/DOM and must use the browser wasm build, not the
// Node one. NW.js sets process.versions.nw, Electron sets process.versions.electron.
const insideDesktopShell = typeof process !== 'undefined' && !!(process?.versions?.nw || process?.versions?.electron)
const insideNode = typeof process !== 'undefined' && typeof process?.platform !== 'undefined' && !insideDesktopShell

class cachering<T> extends Array<T> {
  cur = 0
  constructor(size: number, gen: () => T) {
    super()
    for (let i = 0; i < size; i++) {
      this.push(gen())
    }
  }
  next() {
    let item = this[this.cur]
    this.cur = (this.cur + 1) % this.length
    return item
  }
}

export async function loadWasm(): Promise<IWasmInterface> {
  wasmPromise = undefined

  // Workstream C seam (documentation/plans/native-electron.md). Opt-in via
  // globalThis.__SCULPTCORE_BACKEND === 'native' (e.g. the test harness's
  // `--backend native`); default and the entire browser path are untouched. The
  // native N-API reflection runtime (source/napi/) backs a real IWasmInterface
  // via native factory free-functions (Mesh_createCube, …) and a manager that
  // never reads the WASM linear-memory heap — the litemesh/gpuExecutor paths
  // were de-numbered onto backend-agnostic seams (float3 rings, pointerBytes/
  // objectAddress). We detect + report, then fall back to WASM if the .node is
  // absent. See TODO.md ("native-electron de-numbering / Workstream C").
  if (nativeBackendRequested()) {
    const native = loadNativeAddon()
    if (native) {
      // Run the app on the native backend: build the NativeManager-backed
      // interface and return it instead of loading WASM. Boots the default
      // scene and runs the native litemesh scene end-to-end (build, render,
      // sculpt); see TODO.md / native-electron.md Workstream C.
      const nm = buildNativeManager()!
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      ;(globalThis as any).__nativeManager = nm
      wasm = makeNativeInterface(nm) as unknown as IWasmInterface
      console.warn(
        `[sculptcore] using NATIVE backend (${native.version()}, ${native.bindingCount()} bindings). ` +
          `Workstream C landed: litemesh scene builds, renders + sculpts natively. See native-electron.md.`
      )
      return wasm
    }
    console.warn('[sculptcore] native backend requested but sculptcore_node.node not found; using WASM.')
  }

  const mod = await import(insideNode ? '../build/sculptcore.js' : '../build/sculptcore-browser.js')
  const _wasm = (await mod.default({wasmMemory: createWasmMemory()})) as IWasmMethods
  const initialWasm = {
    ...createWasmHelpers(_wasm, mod.default),
  }

  _wasm.initBindings()
  const managerPtr = _wasm.getBindingManager()
  const manager = new WasmBindingManager<INeededWasm, AllBoundTypes>(initialWasm, managerPtr)
  manager.load()

  const gpu = manager.construct('sculptcore::gpu::GPUManager') as GPUManager

  const float2Ring = new cachering<float2>(1024, () => manager.construct('litestl::math::float2'))
  const float3Ring = new cachering<float3>(1024, () => manager.construct('litestl::math::float3'))

  // Shared heap marshalling for Mesh_serialize / Mesh_serializeRaw: the C++
  // export malloc's a blob + writes its size to a scratch int*; copy out of the
  // (possibly grown) heap before freeing.
  const serializeMeshHeap = (
    meshPtr: pointer,
    exportFn: (mesh: pointer, outSizePtr: pointer) => pointer
  ): Uint8Array => {
    const sizePtr = _wasm._rawAlloc(4)
    try {
      const bufPtr = exportFn(meshPtr, sizePtr) as unknown as number
      const heap = _wasm.HEAPU8
      const len = new DataView(heap.buffer, sizePtr, 4).getInt32(0, true)
      if (!bufPtr || len <= 0) {
        if (bufPtr) _wasm.freeMeshBuffer(bufPtr)
        return new Uint8Array()
      }
      const bytes = heap.slice(bufPtr, bufPtr + len)
      _wasm.freeMeshBuffer(bufPtr)
      return bytes
    } finally {
      _wasm._rawRelease(sizePtr)
    }
  }

  wasm = {
    ...initialWasm,
    manager,
    gpu,
    getBoundVector(vecTypeName: string, bound: SculptHandle) {
      // WASM keeps a numeric heap pointer on the bound object; unwrap it here so
      // callers never have to know the representation.
      const ptr = (bound as unknown as {ptr: pointer}).ptr
      return manager.getBoundVector(vecTypeName, ptr)
    },
    Mesh_createCube(dimen: int, size: number, sphereFac: number) {
      const ptr = _wasm.Mesh_createCube(dimen, size, sphereFac) as unknown as number
      return manager.getBoundPointer('sculptcore::mesh::Mesh', ptr) as Mesh
    },
    Mesh_makeUVSphere(rings: int, segs: int, radius: number) {
      const ptr = _wasm.Mesh_makeUVSphere(rings, segs, radius) as unknown as number
      return manager.getBoundPointer('sculptcore::mesh::Mesh', ptr) as Mesh
    },
    Mesh_makeGrid(nx: int, ny: int, size: number) {
      const ptr = _wasm.Mesh_makeGrid(nx, ny, size) as unknown as number
      return manager.getBoundPointer('sculptcore::mesh::Mesh', ptr) as Mesh
    },
    Mesh_buildSpatialTree(mesh: Mesh, leafLimit: int, depthLimit: int, gpuTriTarget: int) {
      const meshPtr = (mesh as unknown as {ptr: number}).ptr
      const ptr = _wasm.Mesh_buildSpatialTree(
        meshPtr as unknown as Mesh,
        leafLimit,
        depthLimit,
        gpuTriTarget
      ) as unknown as number
      return manager.getBoundPointer('sculptcore::spatial::SpatialTree', ptr) as SpatialTree
    },
    SpatialTree_free(tree: SpatialTree) {
      const treePtr = (tree as unknown as {ptr: number}).ptr
      _wasm.SpatialTree_free(treePtr as unknown as SpatialTree)
    },
    Mesh_serialize(mesh: Mesh): Uint8Array {
      return serializeMeshHeap((mesh as unknown as {ptr: number}).ptr, _wasm.serializeMesh)
    },
    Mesh_serializeRaw(mesh: Mesh): Uint8Array {
      return serializeMeshHeap((mesh as unknown as {ptr: number}).ptr, _wasm.serializeMeshRaw)
    },
    Mesh_deserialize(bytes: Uint8Array): Mesh {
      const dataPtr = _wasm._rawAlloc(bytes.length)
      try {
        _wasm.HEAPU8.set(bytes, dataPtr)
        const ptr = _wasm.deserializeMesh(dataPtr, bytes.length) as unknown as number
        return manager.getBoundPointer('sculptcore::mesh::Mesh', ptr) as Mesh
      } finally {
        _wasm._rawRelease(dataPtr)
      }
    },
    Mesh_free(mesh: Mesh) {
      const meshPtr = (mesh as unknown as {ptr: number}).ptr
      _wasm.freeMesh(meshPtr)
    },
    Mesh_triangulate(mesh: Mesh) {
      const meshPtr = (mesh as unknown as {ptr: number}).ptr
      _wasm.Mesh_triangulate(meshPtr as unknown as Mesh)
    },
    Mesh_ngonFaceCount(mesh: Mesh): number {
      const meshPtr = (mesh as unknown as {ptr: number}).ptr
      return _wasm.Mesh_ngonFaceCount(meshPtr as unknown as Mesh)
    },
    Mesh_quadRemesh(mesh: Mesh, params: SculptHandle): Mesh | undefined {
      const meshPtr = (mesh as unknown as {ptr: number}).ptr
      const paramsPtr = (params as unknown as {ptr: number}).ptr
      const ptr = _wasm.Mesh_quadRemesh(
        meshPtr as unknown as Mesh,
        paramsPtr as unknown as SculptHandle
      ) as unknown as number
      if (!ptr) {
        return undefined // clean failure: infeasible field / too many folds
      }
      return manager.getBoundPointer('sculptcore::mesh::Mesh', ptr) as Mesh
    },
    VdmStore_new(resolution: int, tileSize: int): VdmStore {
      const ptr = _wasm.VdmStore_new(resolution, tileSize) as unknown as number
      return manager.getBoundPointer('sculptcore::vdm::VdmStore', ptr) as VdmStore
    },
    VdmStore_free(store: VdmStore) {
      const storePtr = (store as unknown as {ptr: number}).ptr
      _wasm.VdmStore_free(storePtr as unknown as VdmStore)
    },
    Mesh_vdmSplatDab(
      mesh: Mesh,
      tree: SpatialTree,
      store: VdmStore,
      cx: number,
      cy: number,
      cz: number,
      nx: number,
      ny: number,
      nz: number,
      radius: number,
      strength: number,
      alpha: number,
      invert: int
    ): int {
      return _wasm.Mesh_vdmSplatDab(
        (mesh as unknown as {ptr: number}).ptr as unknown as Mesh,
        (tree as unknown as {ptr: number}).ptr as unknown as SpatialTree,
        (store as unknown as {ptr: number}).ptr as unknown as VdmStore,
        cx,
        cy,
        cz,
        nx,
        ny,
        nz,
        radius,
        strength,
        alpha,
        invert
      )
    },
    Mesh_vdmSplatDabLogged(
      mesh: Mesh,
      tree: SpatialTree,
      store: VdmStore,
      meshLog: SculptHandle,
      cx: number,
      cy: number,
      cz: number,
      nx: number,
      ny: number,
      nz: number,
      radius: number,
      strength: number,
      alpha: number,
      invert: int
    ): int {
      return _wasm.Mesh_vdmSplatDabLogged(
        (mesh as unknown as {ptr: number}).ptr as unknown as Mesh,
        (tree as unknown as {ptr: number}).ptr as unknown as SpatialTree,
        (store as unknown as {ptr: number}).ptr as unknown as VdmStore,
        (meshLog as unknown as {ptr: number}).ptr as unknown as SculptHandle,
        cx,
        cy,
        cz,
        nx,
        ny,
        nz,
        radius,
        strength,
        alpha,
        invert
      )
    },
    Mesh_vdmApplyToVerts(mesh: Mesh, store: VdmStore, clearStore: int): int {
      return _wasm.Mesh_vdmApplyToVerts(
        (mesh as unknown as {ptr: number}).ptr as unknown as Mesh,
        (store as unknown as {ptr: number}).ptr as unknown as VdmStore,
        clearStore
      )
    },
    VdmStore_serializeBlob(store: VdmStore): Uint8Array {
      return serializeMeshHeap((store as unknown as {ptr: number}).ptr, _wasm.VdmStore_serialize)
    },
    VdmStore_deserializeBlob(bytes: Uint8Array): VdmStore | undefined {
      const dataPtr = _wasm._rawAlloc(bytes.length)
      try {
        _wasm.HEAPU8.set(bytes, dataPtr)
        const ptr = _wasm.VdmStore_deserialize(dataPtr, bytes.length) as unknown as number
        return ptr ? (manager.getBoundPointer('sculptcore::vdm::VdmStore', ptr) as VdmStore) : undefined
      } finally {
        _wasm._rawRelease(dataPtr)
      }
    },
    VdmStore_restoreFromBlob(store: VdmStore, bytes: Uint8Array): boolean {
      const dataPtr = _wasm._rawAlloc(bytes.length)
      try {
        _wasm.HEAPU8.set(bytes, dataPtr)
        return _wasm.VdmStore_restoreBlob((store as unknown as {ptr: number}).ptr, dataPtr, bytes.length) !== 0
      } finally {
        _wasm._rawRelease(dataPtr)
      }
    },
    SpatialTree_fillDetailCarrier(tree: SpatialTree, carrier: int) {
      const treePtr = (tree as unknown as {ptr: number}).ptr
      _wasm.SpatialTree_fillDetailCarrier(treePtr as unknown as SpatialTree, carrier)
    },
    Mesh_updateFrames(mesh: Mesh) {
      const meshPtr = (mesh as unknown as {ptr: number}).ptr
      _wasm.Mesh_updateFrames(meshPtr as unknown as Mesh)
    },
    Mesh_layerSetWeight(mesh: Mesh, li: int, weight: number) {
      const meshPtr = (mesh as unknown as {ptr: number}).ptr
      _wasm.Mesh_layerSetWeight(meshPtr as unknown as Mesh, li, weight)
    },
    Mesh_layerSetEnabled(mesh: Mesh, li: int, enabled: int) {
      const meshPtr = (mesh as unknown as {ptr: number}).ptr
      _wasm.Mesh_layerSetEnabled(meshPtr as unknown as Mesh, li, enabled)
    },
    Mesh_layerSetFrozen(mesh: Mesh, li: int, frozen: int) {
      const meshPtr = (mesh as unknown as {ptr: number}).ptr
      _wasm.Mesh_layerSetFrozen(meshPtr as unknown as Mesh, li, frozen)
    },
    Mesh_layerRemove(mesh: Mesh, li: int) {
      const meshPtr = (mesh as unknown as {ptr: number}).ptr
      _wasm.Mesh_layerRemove(meshPtr as unknown as Mesh, li)
    },
    Mesh_setActiveEditLayer(mesh: Mesh, li: int): int {
      const meshPtr = (mesh as unknown as {ptr: number}).ptr
      return _wasm.Mesh_setActiveEditLayer(meshPtr as unknown as Mesh, li)
    },
    Mesh_layerFold(mesh: Mesh) {
      const meshPtr = (mesh as unknown as {ptr: number}).ptr
      _wasm.Mesh_layerFold(meshPtr as unknown as Mesh)
    },
    Multires_new(cage: Mesh, levels: int, leafLimit: int, depthLimit: int, gpuTriTarget: int): Multires {
      const cagePtr = (cage as unknown as {ptr: number}).ptr
      const ptr = _wasm.Multires_new(
        cagePtr as unknown as Mesh,
        levels,
        leafLimit,
        depthLimit,
        gpuTriTarget
      ) as unknown as number
      return manager.getBoundPointer('sculptcore::subdiv::Multires', ptr) as Multires
    },
    Multires_free(mr: Multires) {
      const mrPtr = (mr as unknown as {ptr: number}).ptr
      _wasm.Multires_free(mrPtr as unknown as Multires)
    },
    Multires_setActiveLevel(mr: Multires, level: int): int {
      const mrPtr = (mr as unknown as {ptr: number}).ptr
      return _wasm.Multires_setActiveLevel(mrPtr as unknown as Multires, level)
    },
    Multires_activeMesh(mr: Multires): Mesh | undefined {
      const mrPtr = (mr as unknown as {ptr: number}).ptr
      const ptr = _wasm.Multires_activeMesh(mrPtr as unknown as Multires) as unknown as number
      return ptr ? (manager.getBoundPointer('sculptcore::mesh::Mesh', ptr) as Mesh) : undefined
    },
    Multires_activeTree(mr: Multires): SpatialTree | undefined {
      const mrPtr = (mr as unknown as {ptr: number}).ptr
      const ptr = _wasm.Multires_activeTree(mrPtr as unknown as Multires) as unknown as number
      return ptr ? (manager.getBoundPointer('sculptcore::spatial::SpatialTree', ptr) as SpatialTree) : undefined
    },
    Multires_writeback(mr: Multires, level: int): int {
      const mrPtr = (mr as unknown as {ptr: number}).ptr
      return _wasm.Multires_writeback(mrPtr as unknown as Multires, level)
    },
    Multires_downRefit(mr: Multires, level: int): int {
      const mrPtr = (mr as unknown as {ptr: number}).ptr
      return _wasm.Multires_downRefit(mrPtr as unknown as Multires, level)
    },
    Multires_captureToVdm(mr: Multires, vstore: VdmStore, level: int): int {
      return _wasm.Multires_captureToVdm(
        (mr as unknown as {ptr: number}).ptr as unknown as Multires,
        (vstore as unknown as {ptr: number}).ptr as unknown as VdmStore,
        level
      )
    },
    Multires_storeBlob(mr: Multires): Uint8Array {
      return serializeMeshHeap((mr as unknown as {ptr: number}).ptr, _wasm.Multires_serializeStore)
    },
    Multires_restoreStoreBlob(mr: Multires, bytes: Uint8Array): boolean {
      const mrPtr = (mr as unknown as {ptr: number}).ptr
      const dataPtr = _wasm._rawAlloc(bytes.length)
      try {
        _wasm.HEAPU8.set(bytes, dataPtr)
        return _wasm.Multires_restoreStore(mrPtr, dataPtr, bytes.length) !== 0
      } finally {
        _wasm._rawRelease(dataPtr)
      }
    },
    SpatialTree_setRequestedAttrs(tree: SpatialTree, reqs: RequestedAttrBridge[]) {
      const treePtr = (tree as unknown as {ptr: number}).ptr
      const count = reqs.length
      if (count === 0) {
        _wasm.setTreeRequestedAttrs(treePtr, 0, 0, 0, 0, 0, 0, 0)
        return
      }
      // '\n'-joined names (identifiers never contain newlines, so the join is
      // unambiguous) + five parallel Int32Arrays in slot order.
      const namesPtr = initialWasm.cstring(reqs.map((r) => r.name).join('\n'))
      const bytes = count * 4
      const srcTypesPtr = _wasm._rawAlloc(bytes)
      const elemSizesPtr = _wasm._rawAlloc(bytes)
      const slotsPtr = _wasm._rawAlloc(bytes)
      const domainsPtr = _wasm._rawAlloc(bytes)
      const defaultKindsPtr = _wasm._rawAlloc(bytes)
      try {
        // Re-fetch the heap *after* the allocs (a malloc can grow + rebind the
        // buffer); a DataView avoids needing the HEAP32 view on the _wasm type.
        const dv = new DataView(_wasm.HEAPU8.buffer)
        for (let i = 0; i < count; i++) {
          const r = reqs[i]
          dv.setInt32(srcTypesPtr + i * 4, r.srcType, true)
          dv.setInt32(elemSizesPtr + i * 4, r.elemSize, true)
          dv.setInt32(slotsPtr + i * 4, r.slot, true)
          dv.setInt32(domainsPtr + i * 4, r.domain, true)
          dv.setInt32(defaultKindsPtr + i * 4, r.defaultKind ?? 0, true)
        }
        _wasm.setTreeRequestedAttrs(
          treePtr,
          count,
          namesPtr,
          srcTypesPtr,
          elemSizesPtr,
          slotsPtr,
          domainsPtr,
          defaultKindsPtr
        )
      } finally {
        _wasm._rawRelease(srcTypesPtr)
        _wasm._rawRelease(elemSizesPtr)
        _wasm._rawRelease(slotsPtr)
        _wasm._rawRelease(domainsPtr)
        _wasm._rawRelease(defaultKindsPtr)
      }
    },
    SpatialTree_setDrawShader(tree: SpatialTree, wgsl: string) {
      const treePtr = (tree as unknown as {ptr: number}).ptr
      // WGSL blobs are large + unique — don't pool them via cstring (which never
      // frees). Manually alloc, write ASCII (WGSL is ASCII), NUL-terminate, free.
      const len = wgsl.length
      const ptr = _wasm._rawAlloc(len + 1)
      try {
        const data = _wasm.HEAPU8
        for (let i = 0; i < len; i++) {
          data[ptr + i] = wgsl.charCodeAt(i)
        }
        data[ptr + len] = 0
        _wasm.setTreeDrawShader(treePtr, ptr)
      } finally {
        _wasm._rawRelease(ptr)
      }
    },
    SpatialTree_getMissingAttrSlots(tree: SpatialTree): number[] {
      const treePtr = (tree as unknown as {ptr: number}).ptr
      // Two-call: query the count, then alloc + read.
      const n = _wasm.getTreeMissingAttrSlots(treePtr, 0, 0)
      if (n <= 0) {
        return []
      }
      const outPtr = _wasm._rawAlloc(n * 4)
      try {
        _wasm.getTreeMissingAttrSlots(treePtr, outPtr, n)
        const dv = new DataView(_wasm.HEAPU8.buffer)
        const out: number[] = []
        for (let i = 0; i < n; i++) {
          out.push(dv.getInt32(outPtr + i * 4, true))
        }
        return out
      } finally {
        _wasm._rawRelease(outPtr)
      }
    },
    SpatialTree_refreshRequestedAttrs(tree: SpatialTree) {
      const treePtr = (tree as unknown as {ptr: number}).ptr
      _wasm.refreshTreeRequestedAttrs(treePtr)
    },
    GpuBrush_beginStroke(
      mesh: Mesh,
      tree: SpatialTree,
      brush: SculptHandle,
      meshLog: SculptHandle,
      tool: int
    ): SculptHandle | undefined {
      const raw = _wasm as unknown as IGpuBrushRaw
      const s = raw.GpuBrush_beginStroke(
        (mesh as unknown as {ptr: number}).ptr,
        (tree as unknown as {ptr: number}).ptr,
        (brush as unknown as {ptr: number}).ptr,
        (meshLog as unknown as {ptr: number}).ptr,
        tool
      )
      return s ? ({ptr: s} as SculptHandle) : undefined
    },
    GpuBrush_free(session: SculptHandle) {
      const raw = _wasm as unknown as IGpuBrushRaw
      raw.GpuBrush_free((session as {ptr: number}).ptr)
    },
    GpuBrush_kernelName(session: SculptHandle): string {
      const raw = _wasm as unknown as IGpuBrushRaw
      return initialWasm.jsString(raw.GpuBrush_kernelName((session as {ptr: number}).ptr))
    },
    GpuBrush_info(session: SculptHandle, which: int): int {
      const raw = _wasm as unknown as IGpuBrushRaw
      return raw.GpuBrush_info((session as {ptr: number}).ptr, which)
    },
    GpuBrush_marshalDab(
      session: SculptHandle,
      cx: number,
      cy: number,
      cz: number,
      nx: number,
      ny: number,
      nz: number,
      radius: number,
      filterRadius: number,
      mirrorIdx: int,
      nonaccum: int
    ): int {
      const raw = _wasm as unknown as IGpuBrushRaw
      return raw.GpuBrush_marshalDab(
        (session as {ptr: number}).ptr,
        cx,
        cy,
        cz,
        nx,
        ny,
        nz,
        radius,
        filterRadius,
        mirrorIdx,
        nonaccum
      )
    },
    GpuBrush_data(session: SculptHandle, which: int): Uint8Array {
      const raw = _wasm as unknown as IGpuBrushRaw
      const p = (session as {ptr: number}).ptr
      const size = raw.GpuBrush_dataSize(p, which)
      if (size <= 0) {
        return new Uint8Array()
      }
      const ptr = raw.GpuBrush_dataPtr(p, which)
      if (!ptr) {
        return new Uint8Array()
      }
      // Heap view (zero-copy): valid until the next wasm call that can grow
      // the heap — the dispatcher uploads it to the GPU immediately.
      return new Uint8Array(_wasm.HEAPU8.buffer, ptr, size)
    },
    GpuBrush_applyCo(session: SculptHandle, co: Float32Array) {
      const raw = _wasm as unknown as IGpuBrushRaw
      const bytes = co.length * 4
      const ptr = _wasm._rawAlloc(bytes)
      try {
        new Uint8Array(_wasm.HEAPU8.buffer, ptr, bytes).set(new Uint8Array(co.buffer, co.byteOffset, bytes))
        raw.GpuBrush_applyCo((session as {ptr: number}).ptr, ptr, co.length / 3)
      } finally {
        _wasm._rawRelease(ptr)
      }
    },
    GpuBrush_endStroke(session: SculptHandle, co: Float32Array | null, no: Float32Array | null) {
      const raw = _wasm as unknown as IGpuBrushRaw
      const p = (session as {ptr: number}).ptr
      if (!co) {
        raw.GpuBrush_endStroke(p, 0, 0, 0)
        return
      }
      const coBytes = co.length * 4
      const coPtr = _wasm._rawAlloc(coBytes)
      const noPtr = no ? _wasm._rawAlloc(no.length * 4) : 0
      try {
        // Re-fetch the heap after both allocs (a malloc can grow + rebind it).
        const heap = _wasm.HEAPU8
        new Uint8Array(heap.buffer, coPtr, coBytes).set(new Uint8Array(co.buffer, co.byteOffset, coBytes))
        if (no && noPtr) {
          new Uint8Array(heap.buffer, noPtr, no.length * 4).set(new Uint8Array(no.buffer, no.byteOffset, no.length * 4))
        }
        raw.GpuBrush_endStroke(p, coPtr, noPtr, co.length / 3)
      } finally {
        _wasm._rawRelease(coPtr)
        if (noPtr) {
          _wasm._rawRelease(noPtr)
        }
      }
    },
    /** uses a large cache ring */
    float3(co: ArrayLike<number>) {
      const v = float3Ring.next()
      v.vec[0] = co[0]
      v.vec[1] = co[1]
      v.vec[2] = co[2]
      return v
    },
    /** uses a large cache ring */
    float2(co: ArrayLike<number>) {
      const v = float2Ring.next()
      v.vec[0] = co[0]
      v.vec[1] = co[1]
      return v
    },
  }
  return wasm!
}

export async function getWasm() {
  if (wasm !== undefined) {
    return wasm
  }
  if (wasmPromise !== undefined) {
    return await wasmPromise
  }
  wasmPromise = loadWasm()
  return await wasmPromise
}

export function getWasmImmediate() {
  return wasm
}
const g = globalThis as any
/** debugging console use only */
g.getWasm = () => wasm
