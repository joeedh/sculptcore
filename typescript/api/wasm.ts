import {
  INeededWasm,
  pointer,
  BindingManager as WasmBindingManager,
  createWasmHelpers,
  IWasmBase,
  int,
  createWasmMemory,
} from '@litestl/typescript-runtime'
import type {AllBoundTypes, float2, float3, GPUManager, Mesh, SpatialTree} from '../index'

import {BindingManager} from './manager'
import {loadNativeAddon, nativeBackendRequested} from './nativeBackend'
import {buildNativeManager, makeNativeInterface} from './nativeManager'

interface IWasmMethods extends IWasmBase {
  getBindingManager(): pointer
  initBindings(): void
  /** create a gridded cube with `dimen` x `dimen` quads on each of the six cubic faces.*/
  Mesh_createCube(dimen: int, size: number, sphereFac: number): Mesh
  /** build a coarse BVH over `mesh`'s faces; pass leafLimit<=0 to keep the default. */
  Mesh_buildSpatialTree(mesh: Mesh, leafLimit: int, depthLimit: int): SpatialTree
  SpatialTree_free(tree: SpatialTree): void
  getSpatialShaders(): pointer

  // Raw mesh-serialization exports (mesh/c-api/mesh_c_api.cc). Pointer-level;
  // the `Mesh_serialize`/`Mesh_deserialize`/`Mesh_free` helpers on
  // IWasmInterface wrap these with heap marshalling.
  /** serialize `mesh` into a fresh malloc'd blob; writes the byte count to `outSizePtr` (int*). Free the returned ptr with `freeMeshBuffer`. */
  serializeMesh(mesh: pointer, outSizePtr: pointer): pointer
  /** reconstruct a Mesh from a `serializeMesh` blob; returns a `Mesh*`. */
  deserializeMesh(dataPtr: pointer, size: int): pointer
  /** free a Mesh (`alloc::Delete`) — created by `Mesh_createCube`/`deserializeMesh`. */
  freeMesh(mesh: pointer): void
  /** free a blob returned by `serializeMesh`. */
  freeMeshBuffer(buf: pointer): void
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
  /** Reconstruct a mesh from a `Mesh_serialize` blob. */
  Mesh_deserialize(bytes: Uint8Array): Mesh
  /**
   * Free a mesh handle (allocator-correct: routes to the C++ `alloc::Delete`
   * disposer). Do NOT free meshes via `[Symbol.dispose]` — that path is absent
   * natively and mismatches the engine allocator on WASM.
   */
  Mesh_free(mesh: Mesh): void

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

// Electron's renderer (nodeIntegration) exposes `process`, but it's a browser
// context with fetch/DOM and must use the browser wasm build, not the Node one.
const insideElectron = typeof process !== 'undefined' && !!process?.versions?.electron
const insideNode =
  typeof process !== 'undefined' && typeof process?.platform !== 'undefined' && !insideElectron

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
  // native N-API reflection runtime (source/napi/) loads and works, but is not
  // yet a drop-in IWasmInterface — that needs native factory free-functions
  // (Mesh_createCube, …) and a manager that doesn't read the WASM linear-memory
  // heap (HEAPF32/_rawAlloc), which litemesh.ts/gpuExecutor.ts use today. Until
  // that lands we detect + report, then fall back to WASM so the app keeps
  // working. See TODO.md ("native-electron de-numbering / Workstream C").
  if (nativeBackendRequested()) {
    const native = loadNativeAddon()
    if (native) {
      // Run the app on the native backend: build the NativeManager-backed
      // interface and return it instead of loading WASM. This boots the
      // default (sculptcore-free) scene; sculpt/heap paths (litemesh rayCast,
      // gpuExecutor) still need their reworks and will throw on the absent
      // HEAP* fields (see TODO.md / native-electron.md Workstream C).
      const nm = buildNativeManager()!
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      ;(globalThis as any).__nativeManager = nm
      wasm = makeNativeInterface(nm) as unknown as IWasmInterface
      console.warn(
        `[sculptcore] using NATIVE backend (${native.version()}, ${native.bindingCount()} bindings). ` +
          `Partial: sculpt/heap paths not yet wired. See native-electron.md Workstream C.`,
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
    Mesh_buildSpatialTree(mesh: Mesh, leafLimit: int, depthLimit: int) {
      const meshPtr = (mesh as unknown as {ptr: number}).ptr
      const ptr = _wasm.Mesh_buildSpatialTree(meshPtr as unknown as Mesh, leafLimit, depthLimit) as unknown as number
      return manager.getBoundPointer('sculptcore::spatial::SpatialTree', ptr) as SpatialTree
    },
    SpatialTree_free(tree: SpatialTree) {
      const treePtr = (tree as unknown as {ptr: number}).ptr
      _wasm.SpatialTree_free(treePtr as unknown as SpatialTree)
    },
    Mesh_serialize(mesh: Mesh): Uint8Array {
      const meshPtr = (mesh as unknown as {ptr: number}).ptr
      // Scratch int* for the out-size; serializeMesh malloc's the blob.
      const sizePtr = _wasm._rawAlloc(4)
      try {
        const bufPtr = _wasm.serializeMesh(meshPtr, sizePtr) as unknown as number
        // Read the heap *after* serializeMesh — a malloc can grow (and rebind)
        // the memory views.
        const heap = _wasm.HEAPU8
        const len = new DataView(heap.buffer, sizePtr, 4).getInt32(0, true)
        if (!bufPtr || len <= 0) {
          if (bufPtr) _wasm.freeMeshBuffer(bufPtr)
          return new Uint8Array()
        }
        // Copy out of the heap before freeing the C++ buffer.
        const bytes = heap.slice(bufPtr, bufPtr + len)
        _wasm.freeMeshBuffer(bufPtr)
        return bytes
      } finally {
        _wasm._rawRelease(sizePtr)
      }
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
