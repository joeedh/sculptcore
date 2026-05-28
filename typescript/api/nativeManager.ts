/**
 * NativeManager — assembles the native N-API addon's primitives into the
 * `BindingManager`-shaped surface the app consumes (Workstream C of
 * documentation/plans/native-electron.md). The remaining gap to a full drop-in
 * `IWasmInterface` is the WASM-heap-bound paths (`litemesh.ts rayCast`,
 * `gpuExecutor`) and the `gpu` manager — see TODO.md.
 *
 * Built on the addon's proven primitives: construct / meshCreateCube /
 * meshBuildSpatialTree / spatialTreeFree / vectorLength / vectorGet, plus
 * fixed-size Array members (`float3.vec`) for the float ring helpers.
 */

import {NativeAddon, NativeBound, loadNativeAddon} from './nativeBackend'

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
    },
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
  getBoundVector(_name: string, vec: NativeBound): unknown {
    return makeNativeBoundVector(this.addon, vec)
  }
  Mesh_createCube(dimen: number, size: number, sphereFac: number): NativeBound {
    return this.addon.meshCreateCube(dimen, size, sphereFac)
  }
  Mesh_buildSpatialTree(mesh: NativeBound, leafLimit: number, depthLimit: number): NativeBound {
    return this.addon.meshBuildSpatialTree(mesh, leafLimit, depthLimit)
  }
  SpatialTree_free(tree: NativeBound): void {
    this.addon.spatialTreeFree(tree)
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
 * A partial `IWasmInterface` backed by the NativeManager — enough to boot the
 * default (sculptcore-free) scene under `--backend native`. `gpu` is lazy so
 * boot never constructs `GPUManager`; the WASM-heap fields (HEAPF32, _rawAlloc,
 * …) are intentionally absent and will throw if a sculpt/heap path touches them
 * (those are the remaining reworks — litemesh rayCast, gpuExecutor; see TODO.md).
 */
export function makeNativeInterface(nm: NativeManager): unknown {
  let gpu: NativeBound | undefined
  return {
    manager: nm,
    get gpu(): NativeBound {
      return (gpu ??= nm.construct('sculptcore::gpu::GPUManager'))
    },
    getBoundVector: (name: string, bound: NativeBound) => nm.getBoundVector(name, bound),
    Mesh_createCube: (d: number, s: number, sp: number) => nm.Mesh_createCube(d, s, sp),
    Mesh_buildSpatialTree: (m: NativeBound, l: number, dp: number) =>
      nm.Mesh_buildSpatialTree(m, l, dp),
    SpatialTree_free: (t: NativeBound) => nm.SpatialTree_free(t),
    float2: (c: ArrayLike<number>) => nm.float2(c),
    float3: (c: ArrayLike<number>) => nm.float3(c),
    /** marker so callers/tests can confirm the native backend is active. */
    __backend: 'native',
  }
}
