import * as binding from '@litestl/typescript-runtime'
import fs from 'fs'
import Path from 'path'
import type {AllBoundTypes} from '../typescript/index'

async function setupWasm() {
  const wasmURL = process.argv[2] ?? '../build/sculptcore.js'
  const _wasm = await import(wasmURL)
  const wasmMod = await _wasm.default()

  function unprefix(module: any) {
    for (const k in module) {
      if (typeof k === 'string' && k[0] == '_') {
        module[k.slice(1)] = module[k]
      }
    }

    const wasm2 = binding.createWasmHelpers(module)
    return wasm2 as unknown as binding.INeededWasm
  }

  const wasm = unprefix(wasmMod)

  wasmMod._initBindings()
  const managerPtr = wasmMod._getBindingManager()
  const manager = new binding.BindingManager<binding.INeededWasm, AllBoundTypes>(wasm, managerPtr)
  manager.load()

  return {wasm: wasmMod, manager}
}

const {wasm, manager} = await setupWasm()
const Mesh = manager.get('sculptcore::mesh::Mesh')! as binding.StructType

const constructor = Mesh.constructors[0]
//const mesh = manager.constructClass(constructor)
const mesh = manager.construct('sculptcore::mesh::Mesh')
