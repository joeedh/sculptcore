/**
 * Native (N-API) sculptcore backend loader — Workstream C of
 * documentation/plans/native-electron.md.
 *
 * Loads `sculptcore_node.node` (built by `make.mjs node`, see
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
  meshBuildSpatialTree(mesh: NativeBound, leafLimit: number, depthLimit: number): NativeBound
  spatialTreeFree(tree: NativeBound): void
  /** Free a Mesh created by meshCreateCube. Nulls the wrapper's pointer. */
  meshFree(mesh: NativeBound): void
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
  const insideElectron = !!g.process?.versions?.electron
  return insideElectron && g.__SCULPTCORE_BACKEND === 'native'
}
