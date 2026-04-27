import * as binding from '@litestl/typescript-runtime'
import fs from 'fs'
import Path from 'path'

/** Files/folders in typescript/ to not delete when rebuilding. */
const staticFiles = new Set(['package.json', 'readme.md', 'tsconfig.json', 'api', 'node_modules', 'build'])

const wasmURL = process.argv[2] ?? '../build/sculptcore.js'
const _wasm = await import(wasmURL)
const wasmMod = await _wasm.default()

function setupWasm(wasm: any) {
  for (const k in wasm) {
    if (typeof k === 'string' && k[0] == '_') {
      wasm[k.slice(1)] = wasm[k]
    }
  }

  const wasm2 = binding.createWasmHelpers(wasm)
  return wasm2 as unknown as binding.INeededWasm
}

const wasm = setupWasm(wasmMod)

wasmMod._initBindings()
const managerPtr = wasmMod._getBindingManager()
const manager = new binding.BindingManager(wasm, managerPtr)
manager.load()
const files = manager.generateTypeScript()

const baseDir = Path.resolve('../typescript')
const header =
  `
/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
`.trim() + '\n'
const footer = ''

for (const entry of fs.readdirSync(baseDir)) {
  if (staticFiles.has(entry.toLowerCase())) {
    continue
  }
  fs.rmSync(Path.join(baseDir, entry), {recursive: true, force: true})
}

for (const [path, file] of files) {
  const dirname = Path.join(baseDir, Path.dirname(path))
  fs.mkdirSync(dirname, {recursive: true})
  const finalPath = Path.join(baseDir, path)
  fs.writeFileSync(finalPath, header + file + footer)
}
import {termColor} from '../source/litestl/tests/termColor'
console.log(termColor('Generated typescript interfaces', 'blue'))
