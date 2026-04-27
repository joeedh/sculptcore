import {
  INeededWasm,
  pointer,
  BindingManager as WasmBindingManager,
  createWasmHelpers,
  IWasmBase,
} from '@litestl/typescript-runtime'
import type {AllBoundTypes, Mesh} from '../index'

import {BindingManager} from './manager'

interface IWasmMethods extends IWasmBase {
  getBindingManager(): pointer
  initBindings(): void
  Mesh_createCube(): Mesh
}
export interface IWasmInterface extends INeededWasm, IWasmMethods {
  manager: BindingManager
}

let wasmPromise: Promise<IWasmInterface> | undefined = undefined
let wasm: IWasmInterface | undefined

const insideNode = typeof process !== 'undefined' && typeof process?.platform !== 'undefined'

export async function loadWasm(): Promise<IWasmInterface> {
  wasmPromise = undefined

  const mod = await import(insideNode ? '../build/sculptcore.js' : '../build/sculptcore-browser.js')
  const _wasm = (await mod.default()) as IWasmMethods
  const initialWasm = {
    ...createWasmHelpers(_wasm),
  }

  _wasm.initBindings()
  const managerPtr = _wasm.getBindingManager()
  const manager = new WasmBindingManager<INeededWasm, AllBoundTypes>(initialWasm, managerPtr)
  manager.load()

  wasm = {
    ...initialWasm,
    manager,
    Mesh_createCube() {
      const ptr = _wasm.Mesh_createCube() as unknown as number
      const st = manager.getStruct('sculptcore::mesh::Mesh')
      const cls = manager.getBoundClass(st)
      return new cls(wasm!, ptr, manager, st)
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

const g = globalThis as any
/** debugging console use only */
g.getWasm = () => wasm
