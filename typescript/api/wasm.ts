import {
  INeededWasm,
  pointer,
  BindingManager as WasmBindingManager,
  createWasmHelpers,
  IWasmBase,
  int,
  createWasmMemory,
} from '@litestl/typescript-runtime'
import type {AllBoundTypes, GPUManager, Mesh, SpatialTree} from '../index'

import {BindingManager} from './manager'

interface IWasmMethods extends IWasmBase {
  getBindingManager(): pointer
  initBindings(): void
  /** create a gridded cube with `dimen` x `dimen` quads on each of the six cubic faces.*/
  Mesh_createCube(dimen: int, size: number, sphereFac: number): Mesh
  /** build a coarse BVH over `mesh`'s faces; pass leafLimit<=0 to keep the default. */
  Mesh_buildSpatialTree(mesh: Mesh, leafLimit: int, depthLimit: int): SpatialTree
  SpatialTree_free(tree: SpatialTree): void
  getSpatialShaders(): pointer
}
export interface IWasmInterface extends INeededWasm, IWasmMethods {
  manager: BindingManager
  gpu: GPUManager
}

let wasmPromise: Promise<IWasmInterface> | undefined = undefined
let wasm: IWasmInterface | undefined

const insideNode = typeof process !== 'undefined' && typeof process?.platform !== 'undefined'

export async function loadWasm(): Promise<IWasmInterface> {
  wasmPromise = undefined

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

  wasm = {
    ...initialWasm,
    manager,
    gpu,
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
