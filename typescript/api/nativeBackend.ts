/**
 * Native (N-API) sculptcore backend loader — Workstream C of
 * documentation/plans/native-electron.md.
 *
 * Loads `sculptcore_node.node` (built by `make.mjs build node`, see
 * source/napi/napi_runtime.{h,cc}) in the Electron renderer via the Node
 * `require` that nodeIntegration exposes. The browser build never has `require`,
 * so this returns `undefined` there and the WASM path is used.
 *
 * This exposes the C++ reflection runtime's surface (construct / structNames /
 * vectorView / …) directly — pointers stay in C++. It is intentionally NOT yet
 * wired as a full drop-in `IWasmInterface` in `wasm.ts`: that needs (a) native
 * factory free-functions (`Mesh_createCube`, `Mesh_buildSpatialTree`, …) which
 * the WASM build provides via Embind glue but the addon does not yet export, and
 * (b) a manager shape that doesn't depend on the WASM linear-memory heap
 * (`HEAPF32`, `_rawAlloc`, …) that `litemesh.ts`/`gpuExecutor.ts` read today.
 * Those are the remaining C tasks (see TODO.md).
 */

/** A bound C++ object. Its pointer never crosses into JS as a number. */
export type NativeBound = object

export interface NativeAddon {
  version(): string
  bindingCount(): number
  structNames(): string[]
  structInfo(name: string):
    | {
        size: number
        hasDefaultCtor: boolean
        members: {name: string; type: string; offset: number}[]
        methods: {name: string; params: number; ret: string; static: boolean}[]
      }
    | undefined
  construct(name: string): NativeBound
  /**
   * Build a bound owning instance with a named, parameterized constructor
   * (e.g. constructWith('sculptcore::brush::CommandExecutor', 'main', tree,
   * brush)). Bound-object args marshal to C++ pointer/reference params.
   */
  constructWith(structName: string, ctorName: string, ...args: unknown[]): NativeBound
  /**
   * A fresh owning, empty Vector<SpatialNode*> — the out-param for
   * SpatialTree.filterNodes and the nodes arg for CommandExecutor.execBrush.
   * (The specialization can't be looked up by element type, so the addon
   * recovers it from SpatialTree::leaves()'s return descriptor.)
   */
  makeNodeVector(): NativeBound
  /**
   * A fresh owning, empty Vector<int> — the out-param for
   * SpatialTree.castScreenCircle / castScreenRect (recovered from
   * castScreenCircle's param descriptor, since no method returns it by value).
   */
  makeIntVector(): NativeBound
  /**
   * A fresh owning, empty Vector<float> — the out-param for
   * Mesh.edgePathCoords (recovered from that method's param descriptor).
   */
  makeFloatVector(): NativeBound
  /** length (size_) of a bound litestl::util::Vector. */
  vectorLength(vec: NativeBound): number | undefined
  /** i-th element of a bound Vector as a bound value/wrapper. */
  vectorGet(vec: NativeBound, i: number): unknown
  /**
   * Typed-array view over a bound Vector's contiguous storage. NOTE: under
   * Electron's V8 sandbox this is a *copy*, not a zero-copy external buffer
   * (B3 finding) — writes don't propagate back to C++.
   */
  vectorView(vec: NativeBound): ArrayBufferView | undefined
  // Native factory free-functions (return bound wrappers).
  meshCreateCube(dimen: number, size: number, sphereFac: number): NativeBound
  /** UV-sphere primitive (poles are the only singularities) — the remesh-friendly host test mesh. */
  meshMakeUVSphere(rings: number, segs: number, radius: number): NativeBound
  meshBuildSpatialTree(mesh: NativeBound, leafLimit: number, depthLimit: number, gpuTriTarget: number): NativeBound
  spatialTreeFree(tree: NativeBound): void
  /** Free a Mesh created by meshCreateCube. Nulls the wrapper's pointer. */
  meshFree(mesh: NativeBound): void
  /** Fan-triangulate every n-gon of a Mesh in place (n_ngon_faces -> 0). Rebuild any spatial tree afterwards. */
  meshTriangulate(mesh: NativeBound): void
  /**
   * Feature-aligned quad remesh: returns a NEW Mesh wrapper (input untouched),
   * or undefined on a clean failure (infeasible field / >10% folded faces).
   * Free the result via meshFree. `params` is a bound RemeshParams.
   */
  meshQuadRemesh(mesh: NativeBound, params: NativeBound): NativeBound | undefined
  /** Serialize a Mesh to a versioned, lz4hc-compressed blob (copied into a sandbox ArrayBuffer). */
  meshSerialize(mesh: NativeBound): Uint8Array
  /** Serialize a Mesh to the uncompressed column payload only (autosave worker compresses off-thread). */
  meshSerializeRaw(mesh: NativeBound): Uint8Array
  /** Reconstruct a Mesh from a meshSerialize blob (Uint8Array). Returns a non-owning wrapper. */
  meshDeserialize(bytes: Uint8Array): NativeBound
  // VDM engine seam (vdm/c-api vdm_c_api.cc; displacementAndSubSurf.md V3).
  /** Fresh bound VdmStore (sparse tiled float3 texel store). Pass <= 0 to keep a default; free via vdmStoreFree. */
  vdmStoreNew(resolution: number, tileSize: number): NativeBound
  /** Free a VdmStore created by vdmStoreNew. Nulls the wrapper's pointer. */
  vdmStoreFree(store: NativeBound): void
  /**
   * Splat one VDM dab: writes tangent-space float3 texels into UV-keyed tiles
   * (no vertex moves). Returns the texels touched. Call meshUpdateFrames first.
   */
  meshVdmSplatDab(
    mesh: NativeBound,
    tree: NativeBound,
    store: NativeBound,
    cx: number,
    cy: number,
    cz: number,
    nx: number,
    ny: number,
    nz: number,
    radius: number,
    strength: number,
    alpha: number,
    invert: number
  ): number
  /** texelsClamped of the most recent meshVdmSplatDab (X1 add-a-level prompt signal). */
  vdmLastSplatClamped(): number
  /** Logged splat: the tile-delta rides `meshLog`'s open step as a VdmLogChunk. */
  meshVdmSplatDabLogged(
    mesh: NativeBound,
    tree: NativeBound,
    store: NativeBound,
    meshLog: NativeBound,
    cx: number,
    cy: number,
    cz: number,
    nx: number,
    ny: number,
    nz: number,
    radius: number,
    strength: number,
    alpha: number,
    invert: number
  ): number
  /** VDM -> geometry extraction: displace verts by the store's field; returns verts moved. */
  meshVdmApplyToVerts(mesh: NativeBound, store: NativeBound, clearStore: number): number
  /** VdmStore v2 blob, or undefined on failure. */
  vdmStoreSerialize(store: NativeBound): Uint8Array | undefined
  /** Rebuild a store from a vdmStoreSerialize blob; undefined on parse failure. */
  vdmStoreDeserialize(bytes: Uint8Array): NativeBound | undefined
  /** Refill an existing store from a blob (instance kept). */
  vdmStoreRestoreBlob(store: NativeBound, bytes: Uint8Array): boolean
  /** Logged splat: the tile-delta rides `meshLog`'s open step as a VdmLogChunk. */
  meshVdmSplatDabLogged(
    mesh: NativeBound,
    tree: NativeBound,
    store: NativeBound,
    meshLog: NativeBound,
    cx: number,
    cy: number,
    cz: number,
    nx: number,
    ny: number,
    nz: number,
    radius: number,
    strength: number,
    alpha: number,
    invert: number
  ): number
  /** VDM -> geometry extraction: displace verts by the store's field; returns verts moved. */
  meshVdmApplyToVerts(mesh: NativeBound, store: NativeBound, clearStore: number): number
  /** VdmStore v2 blob, or undefined on failure. */
  vdmStoreSerialize(store: NativeBound): Uint8Array | undefined
  /** Rebuild a store from a vdmStoreSerialize blob; undefined on parse failure. */
  vdmStoreDeserialize(bytes: Uint8Array): NativeBound | undefined
  /** Refill an existing store from a blob (instance kept). */
  vdmStoreRestoreBlob(store: NativeBound, bytes: Uint8Array): boolean
  /** Tag every live face's `.detail.carrier` (0 = GEOM, 1 = VDM). */
  spatialTreeFillDetailCarrier(tree: NativeBound, carrier: number): void
  /** Recompute vertex normals + the F3 frames — the splatter's prerequisite. */
  meshUpdateFrames(mesh: NativeBound): void
  // Sculpt-layer settings mutators (displace/c-api displace_c_api.cc; V5).
  // Each keeps evaluated v.co current; re-applying the prior value is the undo.
  /** Set layer `li`'s weight. */
  meshLayerSetWeight(mesh: NativeBound, li: number, weight: number): void
  /** Enable/disable layer `li` (contribution added/subtracted from co). */
  meshLayerSetEnabled(mesh: NativeBound, li: number, enabled: number): void
  /** Freeze/unfreeze layer `li` (excluded from brush writes, still composited). */
  meshLayerSetFrozen(mesh: NativeBound, li: number, frozen: number): void
  /** Remove layer `li`: subtract contribution, drop settings row + column. */
  meshLayerRemove(mesh: NativeBound, li: number): void
  // Multires seam (subdiv/c-api subdiv_c_api.cc; displacementAndSubSurf S).
  /** Bound Multires stack over `cage` (not owned; must outlive the stack). Tree params 0 = defaults. Free via multiresFree. */
  multiresNew(cage: NativeBound, levels: number, leafLimit: number, depthLimit: number, gpuTriTarget: number): NativeBound
  /** Free a stack created by multiresNew (never frees the cage). Nulls the wrapper's pointer. */
  multiresFree(mr: NativeBound): void
  /** Write back the outgoing level, activate `level` (clamped); returns the active level. */
  multiresSetActiveLevel(mr: NativeBound, level: number): number
  /** The active level's mesh — a NON-owning view of the stack's slot (never free). */
  multiresActiveMesh(mr: NativeBound): NativeBound | undefined
  /** The active level's spatial tree — a NON-owning view (never free). */
  multiresActiveTree(mr: NativeBound): NativeBound | undefined
  /** Fold the level's resident edits into the grids store; returns changed verts. */
  multiresWriteback(mr: NativeBound, level: number): number
  /** Least-squares refit of level−1 to `level`'s surface; returns changed level−1 verts. */
  multiresDownRefit(mr: NativeBound, level: number): number
  /** Grids-store blob (undo seam for down-refit / stack delete). */
  multiresSerializeStore(mr: NativeBound): Uint8Array
  /** Replace the grids store from a blob; invalidates all levels (re-set the active level after). */
  multiresRestoreStore(mr: NativeBound, bytes: Uint8Array): boolean
  /**
   * Bytes a raw-pointer member of a bound object points at — the native
   * bulk-data read (e.g. gpu::Buffer.data). The pointer never crosses to JS as
   * a number; C++ reads it off the descriptor. A *copy* under Electron's V8
   * sandbox (like vectorView). The bound object must outlive the view.
   */
  pointerBytes(bound: NativeBound, member: string, byteLen: number): Uint8Array | undefined
  /**
   * The bound C++ object's address as an opaque identity key (never
   * dereferenced) — for caches keyed by object identity (gpuExecutor's
   * per-Buffer GL-buffer cache). WASM uses the numeric `.ptr` for this.
   */
  objectAddress(bound: NativeBound): number | undefined
  // M5 requested-attribute bridge (spatial/c-api setTree*). Strings + JS arrays
  // can't cross the generic method binding, so these are dedicated exports.
  /**
   * Install the requested attr set on a bound SpatialTree: `count` entries;
   * `namesJoined` is the '\n'-joined name string; the five int arrays are
   * `Int32Array`s of length `count` in slot order (any may be empty/undefined
   * to default). Never throws.
   */
  spatialTreeSetRequestedAttrs(
    tree: NativeBound,
    count: number,
    namesJoined: string,
    srcTypes: Int32Array,
    elemSizes: Int32Array,
    slots: Int32Array,
    domains: Int32Array,
    defaultKinds: Int32Array
  ): void
  /** Set the material WGSL for a bound SpatialTree's requested-attr draw shader. */
  spatialTreeSetDrawShader(tree: NativeBound, wgsl: string): void
  /** The advisory missing-slot list for a bound SpatialTree, as a plain number[]. */
  spatialTreeGetMissingAttrSlots(tree: NativeBound): number[]
  /** Force a per-attribute buffer rebuild against current mesh layers (byte-identical descriptor set). */
  spatialTreeRefreshRequestedAttrs(tree: NativeBound): void
  // GPU brush-stroke seam (brush/c-api gpu_brush_c_api.cc). The session handle
  // is a napi external — opaque; freed only by gpuBrushEndStroke/gpuBrushFree.
  /** Open a GPU stroke session; undefined when the tool has no GPU kernel. */
  gpuBrushBeginStroke(
    mesh: NativeBound,
    tree: NativeBound,
    brush: NativeBound,
    meshLog: NativeBound,
    tool: number
  ): NativeBound | undefined
  /** Free a session without touching the mesh (abort path). */
  gpuBrushFree(session: NativeBound): void
  /** The session's kernel stem (brushWgsl key). */
  gpuBrushKernelName(session: NativeBound): string
  /** Query a GpuBrushInfo selector. */
  gpuBrushInfo(session: NativeBound, which: number): number
  /** Marshal one dab image; returns the workgroup count (0 = nothing to do). */
  gpuBrushMarshalDab(
    session: NativeBound,
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
  /** A marshaled blob (GpuBrushData selector) as a sandbox copy; empty when absent. */
  gpuBrushData(session: NativeBound, which: number): Uint8Array
  /** Per-dab readback apply (packed xyz for every element). */
  gpuBrushApplyCo(session: NativeBound, co: Float32Array): void
  /** Final apply (co/no may be null) + free the session. */
  gpuBrushEndStroke(session: NativeBound, co: Float32Array | null, no: Float32Array | null): void
  // litestl allocator introspection (binding.cc LSTL_*).
  /** Tracked allocation size in bytes (+ the permanent pool when includePermanent). */
  getMemSize(includePermanent: boolean): number
  /** Dump every live allocation block to the log sink (renderer DevTools console). */
  printAllocBlocks(includePermanent: boolean): void
  /** A string describing the allocation backing one bound object (its pointer never crosses to JS as a number). */
  formatBlock(bound: NativeBound): string
  /** A string describing every live allocation block. */
  formatBlocks(printPermanent: boolean): string
  /**
   * Write a message straight to the process stdout (fd 1) from C++ — the smoke
   * test for whether native-side stdout reaches the launched NW.js process's
   * captured output. Defaults to a fixed marker when called with no argument.
   */
  testPrint(msg?: string): void
  /**
   * freopen() the C stdout stream onto `path` and return whether it succeeded.
   * The NW.js renderer starts with fd 0/1/2 closed (EBADF), so a bare
   * `testPrint` printf is lost; redirecting stdout to a launcher-supplied file
   * first gives it a real destination the wrapper reads back (the Windows
   * GUI-subsystem stdout workaround).
   */
  redirectStdout(path: string): boolean
}

// Candidate locations for the built addon, relative to common runtime cwds.
const CANDIDATES = [
  'sculptcore/build/native-node/sculptcore_node.node',
  '../sculptcore/build/native-node/sculptcore_node.node',
  '../../sculptcore/build/native-node/sculptcore_node.node',
]

let cached: NativeAddon | null | undefined

/**
 * Try to load the native addon. Returns `undefined` outside Electron/Node (no
 * `require`) or if the `.node` isn't built. Override the path with
 * `globalThis.__SCULPTCORE_NODE_PATH`. Cached after the first call.
 */
export function loadNativeAddon(): NativeAddon | undefined {
  if (cached !== undefined) return cached ?? undefined

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  const g = globalThis as any
  const req = g.require as ((id: string) => unknown) | undefined
  if (typeof req !== 'function') {
    cached = null
    return undefined
  }

  const paths: string[] = []
  if (typeof g.__SCULPTCORE_NODE_PATH === 'string') paths.push(g.__SCULPTCORE_NODE_PATH)
  paths.push(...CANDIDATES)

  for (const p of paths) {
    try {
      cached = req(p) as NativeAddon
      return cached
    } catch {
      /* try next candidate */
    }
  }
  cached = null
  return undefined
}

/** True if the renderer should use the native backend (opt-in, default off). */
export function nativeBackendRequested(): boolean {
  // Reach process/__SCULPTCORE_BACKEND through globalThis (no @types/node in
  // this tsconfig, so a bare `process` would be an undeclared name).
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  const g = globalThis as any
  // NW.js sets process.versions.nw; Electron sets process.versions.electron.
  const insideDesktopShell = !!(g.process?.versions?.nw || g.process?.versions?.electron)
  return insideDesktopShell && g.__SCULPTCORE_BACKEND === 'native'
}
