import * as binding from '@litestl/typescript-runtime'
import fs from 'fs'
import Path from 'path'
import type {AllBoundTypes} from '../typescript/index'
import {BoundVector} from '@litestl/typescript-runtime/boundVector'

async function setupWasm() {
  const wasmURL = process.argv[2] ?? '../build/sculptcore.js'
  const _wasm = await import(wasmURL)
  const wasmMod = await _wasm.default({wasmMemory: binding.createWasmMemory()})

  function unprefix(module: any) {
    for (const k in module) {
      if (typeof k === 'string' && k[0] == '_') {
        module[k.slice(1)] = module[k]
      }
    }

    const wasm2 = binding.createWasmHelpers(module, _wasm.default)
    return wasm2 as unknown as binding.INeededWasm
  }

  const wasm = binding.createWasmHelpers(unprefix(wasmMod), wasmMod)

  wasmMod._initBindings()
  const managerPtr = wasmMod._getBindingManager()
  const manager = new binding.BindingManager<binding.INeededWasm, AllBoundTypes>(wasm, managerPtr)
  manager.load()

  return {wasm, manager}
}

const {wasm, manager} = await setupWasm()

// XXX use jest
const expect = (test: boolean, ...args: any[]) => {
  if (!test) {
    process.stderr.write(args.join('\n') + '\n')
    process.exit(-1)
  }
}

function testBoundVector() {
  const test1 = [1, 2, 3, 4, 5]

  const testType = (ntype: string) => {
    const type = manager.types.get(ntype) as binding.Binding
    console.log([...manager.types.keys()])
    expect(type !== undefined, ntype + ' does not exist in manager')
    const vec = BoundVector.constructFromItems(wasm, manager, type, test1) as unknown as ArrayLike<number>

    expect(vec !== undefined)
    expect(vec.length === test1.length)

    if (test1.find((n, i) => Math.abs(vec[i] - test1[i]) > 0.0001) !== undefined) {
      console.log('expected:', test)
      console.log('got:', Array.from(vec))
      expect(false, 'vector differs')
    }
  }

  testType('float')
  testType('double')
  for (const t of [8, 16, 32]) {
    testType('int' + t)
    testType('uint' + t)
  }
  testType('pointer')
}

const Mesh = manager.get('sculptcore::mesh::Mesh')! as binding.StructType

const constructor = Mesh.constructors[0]
//const mesh = manager.constructClass(constructor)
const mesh = manager.construct('sculptcore::mesh::Mesh')

testBoundVector()

console.log('tests/wasmTest.ts PASSED')
