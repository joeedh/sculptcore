import {IWasmInterface} from './wasm'

export function createMeshCube(wasm: IWasmInterface, dimen: number) {
  return wasm.Mesh_createCube(dimen, 1.0, 0.0)
}
