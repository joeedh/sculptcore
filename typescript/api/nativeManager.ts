/**
 * NativeManager — assembles the native N-API addon's primitives into the
 * `BindingManager`-shaped surface the app consumes (Workstream C of
 * documentation/plans/native-electron.md, now functionally landed). It is a
 * drop-in `IWasmInterface`: the formerly WASM-heap-bound paths (`litemesh.ts
 * rayCast`, `gpuExecutor`) were de-numbered onto backend-agnostic seams
 * (float3 rings, pointerBytes/objectAddress) — see TODO.md.
 *
 * Built on the addon's proven primitives: construct / meshCreateCube /
 * meshBuildSpatialTree / spatialTreeFree / vectorLength / vectorGet, plus
 * fixed-size Array members (`float3.vec`) for the float ring helpers.
 */

import {NativeAddon, NativeBound, loadNativeAddon} from './nativeBackend'
import type {RequestedAttrBridge} from './wasm'

/**
 * Array-like view over a bound litestl::util::Vector, presenting `.length` and
 * numeric indexing (what the `getBoundVector` use site in sculptcore_ops needs:
 * `boundNodes.length`, `boundNodes[0]`) plus iteration.
 */
export function makeNativeBoundVector(addon: NativeAddon, vec: NativeBound): unknown {
  return new Proxy(
    {addon, vec},
    {
      get(t, p) {
        if (p === 'length') return t.addon.vectorLength(t.vec) ?? 0
        if (typeof p === 'string' && /^\d+$/.test(p)) return t.addon.vectorGet(t.vec, Number(p))
        if (p === Symbol.iterator) {
          return function* () {
            const n = t.addon.vectorLength(t.vec) ?? 0
            for (let i = 0; i < n; i++) yield t.addon.vectorGet(t.vec, i)
          }
        }
        return undefined
      },
    }
  )
}

// Small ring of reusable bound values (mirrors wasm.ts's float2/3 cacherings).
class Ring {
  private items: NativeBound[] = []
  private cur = 0
  constructor(size: number, gen: () => NativeBound) {
    for (let i = 0; i < size; i++) this.items.push(gen())
  }
  next(): NativeBound {
    const item = this.items[this.cur]
    this.cur = (this.cur + 1) % this.items.length
    return item
  }
}

export class NativeManager {
  private f3ring: Ring
  private f2ring: Ring

  constructor(public addon: NativeAddon) {
    this.f3ring = new Ring(256, () => addon.construct('litestl::math::float3'))
    this.f2ring = new Ring(256, () => addon.construct('litestl::math::float2'))
  }

  construct(name: string): NativeBound {
    return this.addon.construct(name)
  }
  /**
   * Build with a named, parameterized constructor. `ctor` is one of the shims
   * minted by `get(...).findConstructor` / `findVectorClass(...)
   * .findDefaultConstructor` below (the native backend has no rich Constructor
   * objects; the shim just carries the names the addon needs). Bound-object
   * args (e.g. the SpatialTree + Brush for CommandExecutor 'main') marshal to
   * C++ pointer params.
   */
  constructWith(ctor: unknown, ...args: unknown[]): NativeBound {
    const c = ctor as {
      __struct?: string
      __ctor?: string
      __nodeVector?: boolean
      __intVector?: boolean
      __floatVector?: boolean
    }
    if (c && c.__intVector) return this.addon.makeIntVector()
    if (c && c.__floatVector) return this.addon.makeFloatVector()
    if (c && c.__nodeVector) return this.addon.makeNodeVector()
    if (c && typeof c.__struct === 'string' && typeof c.__ctor === 'string') {
      return this.addon.constructWith(c.__struct, c.__ctor, ...args)
    }
    throw new Error('NativeManager.constructWith: unrecognized constructor shim')
  }
  /**
   * The BindingManager.get surface the sculpt path uses: a struct handle whose
   * `findConstructor(name)` yields a shim for `constructWith`. (We don't model
   * the full StructType — only what `sculptcore_bindings.ts` reads.)
   */
  get(name: string): unknown {
    return {
      name,
      buildFullName  : () => name,
      findConstructor: (ctorName: string) => ({__struct: name, __ctor: ctorName}),
    }
  }
  /**
   * The Vector-class handle the sculpt/pick paths need (`findDefaultConstructor`
   * + `buildFullName`). Three specializations are requested: `Vector<SpatialNode*>`
   * (brush filterNodes), `Vector<int>` (screen-pick faces/verts, graph stats) and
   * `Vector<float>` (edgePathCoords). The addon's makeNodeVector / makeIntVector /
   * makeFloatVector recover each from a method descriptor, so the default-ctor
   * shim just flags which one.
   */
  findVectorClass(elemName: string): unknown {
    const isInt = elemName === 'int' || elemName === 'int32'
    const isFloat = elemName === 'float' || elemName === 'float32'
    return {
      buildFullName         : () => `litestl::util::Vector<${elemName}>`,
      findDefaultConstructor: () =>
        isInt ? {__intVector: true} : isFloat ? {__floatVector: true} : {__nodeVector: true},
    }
  }
  getBoundVector(_name: string, vec: NativeBound): unknown {
    return makeNativeBoundVector(this.addon, vec)
  }
  Mesh_createCube(dimen: number, size: number, sphereFac: number): NativeBound {
    return this.addon.meshCreateCube(dimen, size, sphereFac)
  }
  Mesh_makeUVSphere(rings: number, segs: number, radius: number): NativeBound {
    return this.addon.meshMakeUVSphere(rings, segs, radius)
  }
  Mesh_buildSpatialTree(mesh: NativeBound, leafLimit: number, depthLimit: number, gpuTriTarget: number): NativeBound {
    return this.addon.meshBuildSpatialTree(mesh, leafLimit, depthLimit, gpuTriTarget)
  }
  SpatialTree_free(tree: NativeBound): void {
    this.addon.spatialTreeFree(tree)
  }
  Mesh_free(mesh: NativeBound): void {
    this.addon.meshFree(mesh)
  }
  Mesh_triangulate(mesh: NativeBound): void {
    this.addon.meshTriangulate(mesh)
  }
  Mesh_quadRemesh(mesh: NativeBound, params: NativeBound): NativeBound | undefined {
    return this.addon.meshQuadRemesh(mesh, params)
  }
  /** Reflection runtime exposes the `ngonFaceCount()` struct method directly on
   * the bound mesh wrapper, so this needs no dedicated C-API export. */
  Mesh_ngonFaceCount(mesh: NativeBound): number {
    return (mesh as unknown as {ngonFaceCount(): number}).ngonFaceCount()
  }
  Mesh_serialize(mesh: NativeBound): Uint8Array {
    return this.addon.meshSerialize(mesh)
  }
  Mesh_serializeRaw(mesh: NativeBound): Uint8Array {
    return this.addon.meshSerializeRaw(mesh)
  }
  Mesh_deserialize(bytes: Uint8Array): NativeBound {
    return this.addon.meshDeserialize(bytes)
  }
  /** Bytes of a bound object's raw-pointer member (e.g. gpu::Buffer.data). */
  pointerBytes(bound: NativeBound, member: string, byteLen: number): Uint8Array | undefined {
    return this.addon.pointerBytes(bound, member, byteLen)
  }
  /** Stable opaque identity key for a bound object (its C++ address). */
  objectAddress(bound: NativeBound): number | undefined {
    return this.addon.objectAddress(bound)
  }
  SpatialTree_setRequestedAttrs(tree: NativeBound, reqs: RequestedAttrBridge[]): void {
    const count = reqs.length
    if (count === 0) {
      this.addon.spatialTreeSetRequestedAttrs(
        tree,
        0,
        '',
        new Int32Array(0),
        new Int32Array(0),
        new Int32Array(0),
        new Int32Array(0),
        new Int32Array(0)
      )
      return
    }
    const names = reqs.map((r) => r.name).join('\n')
    const srcTypes = new Int32Array(count)
    const elemSizes = new Int32Array(count)
    const slots = new Int32Array(count)
    const domains = new Int32Array(count)
    const defaultKinds = new Int32Array(count)
    for (let i = 0; i < count; i++) {
      const r = reqs[i]
      srcTypes[i] = r.srcType
      elemSizes[i] = r.elemSize
      slots[i] = r.slot
      domains[i] = r.domain
      defaultKinds[i] = r.defaultKind ?? 0
    }
    this.addon.spatialTreeSetRequestedAttrs(tree, count, names, srcTypes, elemSizes, slots, domains, defaultKinds)
  }
  SpatialTree_setDrawShader(tree: NativeBound, wgsl: string): void {
    this.addon.spatialTreeSetDrawShader(tree, wgsl)
  }
  SpatialTree_getMissingAttrSlots(tree: NativeBound): number[] {
    return this.addon.spatialTreeGetMissingAttrSlots(tree)
  }
  SpatialTree_refreshRequestedAttrs(tree: NativeBound): void {
    this.addon.spatialTreeRefreshRequestedAttrs(tree)
  }
  // litestl allocator introspection (INeededWasm LSTL_* parity). Natively
  // LSTL_FormatBlock takes the bound object itself (the opaque pointer), where
  // WASM passes a numeric heap pointer.
  LSTL_GetMemSize(includePermanent: boolean): number {
    return this.addon.getMemSize(includePermanent)
  }
  LSTL_PrintAllocBlocks(includePermanent: boolean): void {
    this.addon.printAllocBlocks(includePermanent)
  }
  LSTL_FormatBlocks(includePermanent: boolean): string {
    return this.addon.formatBlocks(includePermanent)
  }
  LSTL_FormatBlock(bound: NativeBound): string {
    return this.addon.formatBlock(bound)
  }
  float3(co: ArrayLike<number>): NativeBound {
    const v = this.f3ring.next() as {vec: number[]}
    const vec = v.vec // capture the array wrapper once (one wrapper, not three)
    vec[0] = co[0]
    vec[1] = co[1]
    vec[2] = co[2]
    return v as unknown as NativeBound
  }
  float2(co: ArrayLike<number>): NativeBound {
    const v = this.f2ring.next() as {vec: number[]}
    const vec = v.vec
    vec[0] = co[0]
    vec[1] = co[1]
    return v as unknown as NativeBound
  }
}

/** Build a NativeManager if the addon is available, else undefined. */
export function buildNativeManager(): NativeManager | undefined {
  const addon = loadNativeAddon()
  return addon ? new NativeManager(addon) : undefined
}

/**
 * An `IWasmInterface` backed by the NativeManager — boots the default scene and
 * runs the native litemesh scene (build, render, sculpt) under `--backend
 * native`. `gpu` is lazy so boot never constructs `GPUManager`; the WASM-heap
 * fields (HEAPF32, _rawAlloc, …) are intentionally absent because nothing reads
 * them anymore — the sculpt/heap paths (litemesh rayCast, gpuExecutor) go
 * through backend-agnostic seams instead (see TODO.md).
 */
export function makeNativeInterface(nm: NativeManager): unknown {
  let gpu: NativeBound | undefined
  return {
    manager: nm,
    get gpu(): NativeBound {
      return (gpu ??= nm.construct('sculptcore::gpu::GPUManager'))
    },
    getBoundVector                   : (name: string, bound: NativeBound) => nm.getBoundVector(name, bound),
    pointerBytes                     : (b: NativeBound, m: string, n: number) => nm.pointerBytes(b, m, n),
    objectAddress                    : (b: NativeBound) => nm.objectAddress(b),
    Mesh_createCube                  : (d: number, s: number, sp: number) => nm.Mesh_createCube(d, s, sp),
    Mesh_makeUVSphere                : (r: number, s: number, rad: number) => nm.Mesh_makeUVSphere(r, s, rad),
    Mesh_buildSpatialTree            : (m: NativeBound, l: number, dp: number, t: number) => nm.Mesh_buildSpatialTree(m, l, dp, t),
    SpatialTree_free                 : (t: NativeBound) => nm.SpatialTree_free(t),
    Mesh_free                        : (m: NativeBound) => nm.Mesh_free(m),
    Mesh_triangulate                 : (m: NativeBound) => nm.Mesh_triangulate(m),
    Mesh_quadRemesh                  : (m: NativeBound, p: NativeBound) => nm.Mesh_quadRemesh(m, p),
    Mesh_ngonFaceCount               : (m: NativeBound) => nm.Mesh_ngonFaceCount(m),
    Mesh_serialize                   : (m: NativeBound) => nm.Mesh_serialize(m),
    Mesh_serializeRaw                : (m: NativeBound) => nm.Mesh_serializeRaw(m),
    Mesh_deserialize                 : (b: Uint8Array) => nm.Mesh_deserialize(b),
    SpatialTree_setRequestedAttrs: (t: NativeBound, reqs: RequestedAttrBridge[]) =>
      nm.SpatialTree_setRequestedAttrs(t, reqs),
    SpatialTree_setDrawShader        : (t: NativeBound, wgsl: string) => nm.SpatialTree_setDrawShader(t, wgsl),
    SpatialTree_getMissingAttrSlots  : (t: NativeBound) => nm.SpatialTree_getMissingAttrSlots(t),
    SpatialTree_refreshRequestedAttrs: (t: NativeBound) => nm.SpatialTree_refreshRequestedAttrs(t),
    LSTL_GetMemSize                  : (p: boolean) => nm.LSTL_GetMemSize(p),
    LSTL_PrintAllocBlocks            : (p: boolean) => nm.LSTL_PrintAllocBlocks(p),
    LSTL_FormatBlocks                : (p: boolean) => nm.LSTL_FormatBlocks(p),
    LSTL_FormatBlock                 : (b: NativeBound) => nm.LSTL_FormatBlock(b),
    float2                           : (c: ArrayLike<number>) => nm.float2(c),
    float3                           : (c: ArrayLike<number>) => nm.float3(c),
    /** marker so callers/tests can confirm the native backend is active. */
    __backend                        : 'native',
  }
}
