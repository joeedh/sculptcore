#!/usr/bin/env node
import Path from 'path'
import fs from 'fs'
import child_process from 'child_process'
import yargs from 'yargs'
import {hideBin} from 'yargs/helpers'
import {fileURLToPath} from 'url'
import {termColor} from './source/litestl/tests/termColor.js'
import {syntaxHighlight} from './tools/syntaxHighlight.mjs'
import {ensureDeps, configName} from './tools/deps.mjs'

// Every input/output path below is resolved relative to the working dir,
// assuming it's this script's own directory; chdir here so `node make.mjs ...`
// and `node sculptcore/make.mjs ...` (from the repo root) both work.
process.chdir(Path.dirname(fileURLToPath(import.meta.url)))

let options = {}
if (fs.existsSync('local-build-options.mjs')) {
  console.log('Loading local build options from ./local-build-options.mjs')
  options = (await import('./local-build-options.mjs')).default
}

let usedOptsMsg = ''
const validOpts = new Set()
const getopt = (k, defval) => {
  validOpts.add(k)
  if (k in options) {
    usedOptsMsg += `  ${k} = ${options[k]}\n`
  }
  return options[k] ?? defval
}

// read local options
const CMAKE_BUILD_TYPE = getopt('CMAKE_BUILD_TYPE', 'RelWithDebInfo')
const CMAKE_GENERATOR = getopt('CMAKE_GENERATOR', 'Ninja')
const WITH_ASAN = getopt('WITH_ASAN', false)
const WITH_MESHLOG_ABSEIL_HASHMAP = getopt('WITH_MESHLOG_ABSEIL_HASHMAP', false)

for (const k in options) {
  if (!validOpts.has(k)) {
    usedOptsMsg += `  invalid option ${k}\n`
  }
}

if (usedOptsMsg.length > 0) {
  usedOptsMsg = `\nLocal options:\n${usedOptsMsg}\n`
  process.stdout.write(usedOptsMsg)
}

let CMAKE_ARGS_BASE = `-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}`
if (WITH_MESHLOG_ABSEIL_HASHMAP) {
  CMAKE_ARGS_BASE += ` -DWITH_MESHLOG_ABSEIL_HASHMAP=ON`
}
CMAKE_ARGS_BASE += ` -G ${CMAKE_GENERATOR} -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`

const EMSDK_VERSION = fs.readFileSync('./emsdkVersion.txt', 'utf-8').trim()
const NAGA_VERSION = fs.readFileSync('./nagaVersion.txt', 'utf-8').trim()
const CMAKE_WASM_ARGS = CMAKE_ARGS_BASE + ` -DBUILD_WASM=ON`
const EMSDK_COMMIT = '2a9b4692ab24a0497249eeaa696ac1153d22e07e'

/**
 * Ensures final linked files are destroyed,
 * since emscripten is not that great at making
 * errors during complication actually be obvious
 */
function deleteFinalWasmFiles() {
  if (fs.existsSync('build/sculptcore.js')) {
    fs.rmSync('build/sculptcore.js', {force: true})
  }
  if (fs.existsSync('build/sculptcore.wasm')) {
    fs.rmSync('build/sculptcore.wasm', {force: true})
  }
  if (fs.existsSync('build/sculptcore-browser.js')) {
    fs.rmSync('build/sculptcore-browser.js', {force: true})
  }
  if (fs.existsSync('build/sculptcore-browser.wasm')) {
    fs.rmSync('build/sculptcore-browser.wasm', {force: true})
  }
  if (fs.existsSync('typescript/build/sculptcore.js')) {
    fs.rmSync('typescript/build/sculptcore.js', {force: true})
  }
  if (fs.existsSync('typescript/build/sculptcore.wasm')) {
    fs.rmSync('typescript/build/sculptcore.wasm', {force: true})
  }
}

function run(cmd, options = {shell: true}) {
  try {
    return child_process.execSync(cmd, {shell: options?.shell ?? true, stdio: 'inherit'})
  } catch (error) {
    process.stderr.write(error.message + '\n')
    process.exit(1)
  }
}

/* Max parallel compile jobs for `cmake --build`, set from the global -j/--jobs
 * flag (see the yargs middleware). Undefined = let the generator use all cores.
 * Lower it (e.g. `-j 2`) when clang OOMs on the heavy template files. */
let JOBS = undefined
function parallelFlag() {
  return JOBS && JOBS > 0 ? ` --parallel ${JOBS}` : ''
}

function summarizeErrors(buf) {
  return new Promise((accept, reject) => {
    // disable for now
    return
    if (buf.length < 2048 * 80) {
      return
    }
    console.log('\n\nSummarizing errors...\n')
    fs.writeFileSync('build/summary.log', buf)

    const proc = child_process.spawn(
      `claude --print --no-session-persistence --model haiku "summarize the file build/summary.log, ignore all nodejs warnings"`,
      {
        shell: true,
        stdio: ['ignore', 'pipe', 'pipe'],
      }
    )
    proc.on('error', (err) => {
      console.log(err.message)
      accept(1)
    })
    proc.on('exit', (code) => {
      accept(code)
    })
    proc.stdout.on('data', (data) => {
      process.stdout.write(data)
    })
    proc.stderr.on('data', (data) => {
      process.stderr.write(data)
    })
  })
}

class BuildColor {
  buf = ''
  fullBuf = ''
  istty = false
  stream = process.stdout
  needFlush = false

  constructor(stream) {
    this.stream = stream
    this.istty = process.stdout.isTTY
  }

  write(str) {
    if (typeof str !== 'string') {
      str = str.toString('ascii')
    }
    this.buf += str
    this.fullBuf += str
    if (str.search(/\n/) !== -1) {
      this.queueFlush()
    }
  }

  queueFlush() {
    if (this.needFlush) {
      return
    }
    this.needFlush = true
    setTimeout(() => this.flush(), 150)
  }

  flush() {
    this.needFlush = false
    const diagRE = /(.*)(error|note|warning):(.*)/

    const lines = this.buf.split('\n').map((l) => {
      // cmake status messages
      if (/^\[\d+\/\d+\]/.test(l)) {
        l = termColor(l, 'yellow')
      } else if (diagRE.test(l)) {
        const parts = diagRE.exec(l)
        // make source paths relative
        if (parts[1].replace(/\\/g, '/').startsWith(process.cwd().replace(/\\/g, '/'))) {
          parts[1] = Path.relative(Path.normalize(process.cwd()), Path.normalize(parts[1]))
        }

        if (parts[2] === 'note') {
          // unquote quoted code snippet from clang
          if (/because '/.test(parts[3])) {
            let i = parts[3].search(/'/)
            parts[3] = parts[3].slice(0, i) + '(' + parts[3].slice(i + 1)
            let j = parts[3].lastIndexOf("'")
            parts[3] = parts[3].slice(0, j) + ')' + parts[3].slice(j + 1)
          }
          l = `${termColor(parts[1], 'teal')}${termColor(parts[2], 'green')}:${syntaxHighlight(parts[3])}`
        } else if (parts[2] === 'warning') {
          l = `${termColor(parts[1], 'teal')}${termColor(parts[2], 'yellow')}:${termColor(parts[3], 'yellow')}`
        } else {
          l = `${termColor(parts[1], 'teal')}${termColor(parts[2], 'red')}:${termColor(parts[3], 'yellow')}`
        }
        /*
        let i = l.search(diagRE)
        l =
          termColor(l.slice(0, i), 'teal') + //
          termColor(l.slice(i, i + 6), 'red') +
          termColor(l.slice(i + 6), 'yellow')*/
      } else if (/\^/.test(l)) {
        let i = l.search(/\^/)
        l = l.slice(0, i) + termColor('^', 'yellow') + l.slice(i + 1)
      }

      // source snippets from clang
      if (/[ \t]+\d+ \| /.test(l)) {
        const i = l.search(/\|/)
        const start = l.slice(0, i)
        l = l.slice(i + 1)
        l = termColor(start, 'blue') + '|' + syntaxHighlight(l)
      }

      l = l.replace(/\~/g, termColor('~', 'red'))
      return l
    })
    this.stream.write(lines.join('\n'))
    this.buf = ''
  }
}

function runBuild(cmd) {
  return new Promise((accept, reject) => {
    const proc = child_process.spawn(cmd, {stdio: 'pipe', shell: true})
    proc.on('error', (err) => {
      process.stderr.write(err.stack + '\n')
      process.stderr.write(err.message + '\n')
      process.exit(1)
    })

    const stdout = new BuildColor(process.stdout)
    const stderr = new BuildColor(process.stderr)

    proc.stdout.on('data', (data) => stdout.write(data))
    proc.stderr.on('data', (data) => stderr.write(data))
    proc.on('close', async (code) => {
      if (code) {
        stdout.flush()
        stderr.flush()

        await summarizeErrors('==== stderr =====\n' + stderr.fullBuf + '==== stdout =====\n' + stdout.fullBuf)
        process.stderr.write(termColor(`cmd "${cmd}" existed with code ${code}\n`, 'red'))
        process.exit(code)
      }
      accept(code)
    })
  })
}
function ensureDir(p) {
  if (!fs.existsSync(p)) fs.mkdirSync(p, {recursive: true})
}

function buildDir(target) {
  return target === 'native' ? 'build/native' : 'build'
}

// Returns the `node ../configureEnv.mjs [--emsdk]` prefix used inside buildDir.
function envPrefix(target) {
  const rel = target === 'native' ? '../..' : '..'
  const emsdk = target === 'native' ? '' : '--emsdk '
  return `node ${rel}/configureEnv.mjs ${emsdk}`.trimEnd()
}

// Use clang for native builds on every platform — it is the project's
// required toolchain. Path is written relative to the build dir
// (build/native).
function nativeToolchainFlag() {
  return '--toolchain ../../build_files/native-clang.cmake '
}

// === Node / Electron N-API addon ===
//
// Builds sculptcore_node.node (Workstream A of documentation/plans/
// native-electron.md). cmake-js drives the *configure* step — it downloads the
// Electron headers + node.lib and injects CMAKE_JS_* — then we build only the
// addon target with the repo's clang toolchain. A dedicated build dir keeps the
// normal build/native (default CRT) untouched. The clang↔Electron link was
// de-risked in sculptcore/spike/napi/ (see RESULTS.md).

// The electron/ app package lives one level above sculptcore/ (repo root).
function readElectronVersion() {
  try {
    const pkg = JSON.parse(fs.readFileSync('../electron/package.json', 'utf-8'))
    const v = pkg.devDependencies?.electron ?? pkg.dependencies?.electron ?? ''
    const m = v.match(/\d+\.\d+\.\d+/)
    if (m) return m[0]
  } catch {
    /* fall through to default */
  }
  return '41.1.1'
}

// Resolve the Electron executable path (require('electron') returns it when
// required outside Electron). cwd = the repo's electron/ app.
function resolveElectronExe() {
  try {
    return child_process.execSync(`node -p "require('electron')"`, {cwd: '../electron', encoding: 'utf-8'}).trim()
  } catch {
    return undefined
  }
}

async function buildNodeAddon(electronVersion, smoke) {
  const dir = 'build/native-node'
  ensureDir(dir)
  const ev = electronVersion || readElectronVersion()
  const toolchain = Path.resolve('build_files/native-clang.cmake').replace(/\\/g, '/')
  const cmakeJs = 'node node_modules/cmake-js/bin/cmake-js'
  // Native env prefix, run from the sculptcore root (where configureEnv.mjs is).
  const env = 'node configureEnv.mjs'

  console.log(`Building Node addon for Electron ${ev} -> ${dir}/sculptcore_node.node`)

  // Prebuilt OpenBLAS + SuiteSparse/CHOLMOD, same as `configure native`, so the
  // addon links the cholmod target instead of warning it off.
  const depsDir = await ensureDeps({config: configName(CMAKE_BUILD_TYPE)})
  const depsDef = `--CDSCULPTCORE_DEPS_DIR=${depsDir.replace(/\\/g, '/')}`

  // cmake-js defaults to the static CRT (/MT); force the dynamic CRT so the
  // addon matches Electron, the rest of the native tree, and the prebuilt
  // /MD deps (mismatched CRTs surface as undefined dllimport CRT symbols).
  const crtDef = '--CDCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL'

  // 1. Configure via cmake-js: downloads the Electron headers + node.lib and
  //    injects CMAKE_JS_INC/LIB/SRC. Clang toolchain + Ninja, like the rest of
  //    the native tree.
  run(
    `${env} "${cmakeJs} configure -O ${dir} -G Ninja --CDWITH_ASAN=${WITH_ASAN ? 'ON' : 'OFF'} --CDCMAKE_TOOLCHAIN_FILE=${toolchain} ${depsDef} ${crtDef} -r electron -v ${ev} -a x64"`
  )

  // 2. Build ONLY the addon target. Its static deps come along; the SHARED
  //    `sculptcore` lib is intentionally not built here (see the CMakeLists
  //    note about the global /DELAYLOAD flag under the clang driver).
  await runBuild(`${env} "cmake --build ${dir} --target sculptcore_node${parallelFlag()}"`)

  const out = Path.resolve(dir, 'sculptcore_node.node').replace(/\\/g, '/')
  if (!fs.existsSync(out)) {
    process.stderr.write(`node: addon not found at ${out}\n`)
    process.exit(1)
  }
  console.log(`node: built ${out}`)

  if (smoke) {
    const electronExe = resolveElectronExe()
    if (!electronExe) {
      process.stderr.write('node: --smoke needs electron installed under ../electron\n')
      process.exit(1)
    }
    run(`"${electronExe}" source/napi/electron_smoke.cjs --no-sandbox`)
  }
}

// === sbrush DSL codegen ===
//
// Builds the host-side `sbrushc` binary (via the existing native CMake
// tree) and runs it across every .sbrush in source/brush/kernels/,
// writing outputs to source/brush/kernels/generated/. Idempotent —
// sbrushc skips writing files whose contents are unchanged so CMake
// won't churn.
async function sbrushCodegen() {
  const kernelsDir = 'source/brush/kernels'
  const outDir = `${kernelsDir}/generated`
  ensureDir(outDir)

  const nativeBuild = buildDir('native')
  const sbrushcCandidates = [
    `${nativeBuild}/source/brush/compiler/sbrushc`,
    `${nativeBuild}/source/brush/compiler/sbrushc.exe`,
    `${nativeBuild}/source/brush/compiler/Debug/sbrushc.exe`,
    `${nativeBuild}/source/brush/compiler/Release/sbrushc.exe`,
  ]
  let sbrushc = sbrushcCandidates.find((p) => fs.existsSync(p))

  if (!sbrushc) {
    if (!fs.existsSync(`${nativeBuild}/CMakeCache.txt`)) {
      console.log('codegen: native build not configured; configuring first...')
      ensureDir(nativeBuild)
      run(
        `cd ${nativeBuild} && ${envPrefix('native')} cmake ../.. -G Ninja ${nativeToolchainFlag()}-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}`
      )
    }
    console.log('codegen: building sbrushc...')
    run(`cd ${nativeBuild} && ${envPrefix('native')} cmake --build . --target sbrushc`)
    sbrushc = sbrushcCandidates.find((p) => fs.existsSync(p))
    if (!sbrushc) {
      process.stderr.write('codegen: sbrushc not found after build\n')
      process.exit(1)
    }
  }

  const inputs = fs.readdirSync(kernelsDir).filter((f) => f.endsWith('.sbrush'))
  if (inputs.length === 0) {
    console.log('codegen: no .sbrush inputs found')
    return
  }
  for (const inp of inputs) {
    const stem = inp.replace(/\.sbrush$/, '')
    const inPath = `${kernelsDir}/${inp}`
    const outPath = `${outDir}/${stem}.brush.gen.h`
    run(`"${sbrushc}" --backend=cpp --in="${inPath}" --out="${outPath}"`)
  }
}

// === sbrush cross-backend verification ===
//
// Drives every tests/scripts/brush_backends/*_ab.txt script through the
// native debug_app, which runs the same stroke under the `cpp` then
// `wgsl` backend gate (both currently execute via the C++ executor — there
// is no native WebGPU dispatch yet). For each brush we check two things:
//   1. cross-backend: the cpp and wgsl dumps agree (trivially true today,
//      but the gate that future real GPU dispatch must keep satisfying).
//   2. regression: the cpp dump matches tests/golden/<brush>.json within
//      tolerance. `--regen` (re)writes those goldens from the cpp dump.
// Comparison is a tolerant recursive numeric diff over the dump JSON, so
// fp noise across compilers/platforms doesn't trip it but real brush
// behavior changes do (the dump carries a co_sum/co_sqsum fingerprint).
const VERIFY_ATOL = 1e-5
const VERIFY_RTOL = 1e-4

// Dump keys excluded from the verify comparison. `flag` is a spatial-node
// dirty/update bitmask (pending normals/bounds/GPU regen) that reflects
// update *history* — e.g. it differs depending on whether an `undo`
// preceded the stroke — not brush *output*. Equivalence here is about
// geometry + topology + structure, so these transient fields are skipped.
const VERIFY_IGNORE_KEYS = new Set(['flag'])

// Recursively diff two parsed-JSON dump values. Returns an array of
// human-readable mismatch strings (empty == equal). `path` tracks the
// JSON pointer for messages.
function diffDump(a, b, path = '') {
  const out = []
  if (typeof a === 'number' && typeof b === 'number') {
    const tol = VERIFY_ATOL + VERIFY_RTOL * Math.max(Math.abs(a), Math.abs(b))
    if (Math.abs(a - b) > tol) {
      out.push(`${path || '<root>'}: ${a} != ${b} (|Δ|=${Math.abs(a - b).toExponential(3)} > ${tol.toExponential(3)})`)
    }
    return out
  }
  if (Array.isArray(a) && Array.isArray(b)) {
    if (a.length !== b.length) {
      out.push(`${path}: array length ${a.length} != ${b.length}`)
      return out
    }
    for (let i = 0; i < a.length; i++) out.push(...diffDump(a[i], b[i], `${path}[${i}]`))
    return out
  }
  if (a && b && typeof a === 'object' && typeof b === 'object') {
    const keys = new Set([...Object.keys(a), ...Object.keys(b)])
    for (const k of keys) {
      if (VERIFY_IGNORE_KEYS.has(k)) continue
      if (!(k in a) || !(k in b)) {
        out.push(`${path}/${k}: present in only one dump`)
        continue
      }
      out.push(...diffDump(a[k], b[k], `${path}/${k}`))
    }
    return out
  }
  if (a !== b) out.push(`${path}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`)
  return out
}

function readDumpJson(p) {
  try {
    return JSON.parse(fs.readFileSync(p, 'utf-8'))
  } catch (e) {
    return null
  }
}

async function sbrushVerify(regen) {
  const dir = buildDir('native')
  const env = envPrefix('native')
  ensureDir(dir)

  // debug_app must accept `set_backend backend=wgsl` (gated on
  // SBRUSH_BACKEND_WGSL) and, for real GPU dispatch, load the SPIR-V kernels
  // (gated on SBRUSH_BACKEND_SPIRV). Configure native with cpp+wgsl+spirv,
  // then build debug_app plus the sbrush-spirv target that emits the .spv the
  // dispatcher loads at runtime.
  const sbrushFlags = sbrushBackendFlags('cpp,wgsl,spirv')
  run(
    `cd ${dir} && ${env} cmake ../.. -G Ninja ${nativeToolchainFlag()}-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} ${sbrushFlags}`
  )
  await runBuild(`cd ${dir} && ${env} cmake --build . --target debug_app sbrush-spirv${parallelFlag()}`)

  const debugApp = `${dir}/source/debug/debug_app${process.platform === 'win32' ? '.exe' : ''}`
  if (!fs.existsSync(debugApp)) {
    process.stderr.write(`sbrush-verify: debug_app not found at ${debugApp}\n`)
    process.exit(1)
  }

  const scriptDir = 'tests/scripts/brush_backends'
  const goldenDir = 'tests/golden'
  ensureDir(goldenDir)
  const outDir = `${dir}/sbrush_verify_out`
  ensureDir(outDir)

  const scripts = fs
    .readdirSync(scriptDir)
    .filter((f) => f.endsWith('_ab.txt'))
    .sort()
  if (scripts.length === 0) {
    process.stderr.write(`sbrush-verify: no *_ab.txt scripts in ${scriptDir}\n`)
    process.exit(1)
  }

  let failures = 0
  let regenerated = 0
  for (const s of scripts) {
    const brush = s.replace(/_ab\.txt$/, '')
    const res = child_process.spawnSync(debugApp, ['--script', `${scriptDir}/${s}`, '--out', outDir, '--headless'], {
      encoding: 'utf-8',
    })
    if (res.status !== 0) {
      failures++
      process.stderr.write(`✗ ${brush}: debug_app exited ${res.status}\n`)
      if (res.stderr) process.stderr.write(res.stderr.split('\n').slice(-8).join('\n') + '\n')
      continue
    }

    const cppPath = `${outDir}/${brush}_cpp.json`
    const wgslPath = `${outDir}/${brush}_wgsl.json`
    const cpp = readDumpJson(cppPath)
    const wgsl = readDumpJson(wgslPath)
    if (!cpp || !wgsl) {
      failures++
      process.stderr.write(`✗ ${brush}: missing/invalid dump (cpp=${!!cpp} wgsl=${!!wgsl})\n`)
      continue
    }

    // 1. cross-backend equivalence.
    const ab = diffDump(cpp, wgsl)
    if (ab.length) {
      failures++
      process.stderr.write(`✗ ${brush}: cpp vs wgsl mismatch:\n  ${ab.slice(0, 6).join('\n  ')}\n`)
      continue
    }

    // 2. golden regression.
    const goldenPath = `${goldenDir}/${brush}.json`
    if (regen) {
      fs.copyFileSync(cppPath, goldenPath)
      regenerated++
      console.log(`↻ ${brush}: golden written`)
      continue
    }
    const golden = readDumpJson(goldenPath)
    if (!golden) {
      failures++
      process.stderr.write(`✗ ${brush}: no golden at ${goldenPath} (run with --regen)\n`)
      continue
    }
    const gd = diffDump(cpp, golden)
    if (gd.length) {
      failures++
      process.stderr.write(`✗ ${brush}: cpp vs golden mismatch:\n  ${gd.slice(0, 6).join('\n  ')}\n`)
      continue
    }
    console.log(`✓ ${brush}: cpp == wgsl, matches golden`)
  }

  if (regen) {
    console.log(`\nsbrush-verify: regenerated ${regenerated} golden(s).`)
    return
  }
  if (failures) {
    process.stderr.write(`\nsbrush-verify: ${failures} brush(es) failed.\n`)
    process.exit(1)
  }
  console.log(`\nsbrush-verify: all ${scripts.length} brush(es) passed.`)
}

// Validates the sbrush WGSL kernels on a real WebGPU runtime (Dawn, via
// @kmamal/gpu — headless on software Vulkan). debug_app --gpu-capture records
// the exact per-binding buffer bytes and final readback of each native wgsl
// stroke into a JSON fixture; tests/webgpu/replay.mjs replays those bytes
// through Dawn and diffs the GPU readback against the native reference. This
// exercises the SAME WGSL the native SPIR-V path runs (build/native/
// sbrush_out/spirv/<k>.wgsl) without reimplementing the engine in JS.
async function webgpuVerify() {
  const dir = buildDir('native')
  const env = envPrefix('native')
  ensureDir(dir)

  // Same configure/build as sbrush-verify: debug_app needs the wgsl backend
  // and the SPIR-V kernels (whose .wgsl the harness feeds to Dawn).
  const sbrushFlags = sbrushBackendFlags('cpp,wgsl,spirv')
  run(
    `cd ${dir} && ${env} cmake ../.. -G Ninja ${nativeToolchainFlag()}-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} ${sbrushFlags}`
  )
  await runBuild(`cd ${dir} && ${env} cmake --build . --target debug_app sbrush-spirv${parallelFlag()}`)

  const debugApp = `${dir}/source/debug/debug_app${process.platform === 'win32' ? '.exe' : ''}`
  if (!fs.existsSync(debugApp)) {
    process.stderr.write(`webgpu-verify: debug_app not found at ${debugApp}\n`)
    process.exit(1)
  }
  const wgslDir = `${dir}/sbrush_out/spirv`
  if (!fs.existsSync(wgslDir)) {
    process.stderr.write(`webgpu-verify: WGSL kernels not found at ${wgslDir}\n`)
    process.exit(1)
  }

  const harness = 'tests/webgpu/replay.mjs'
  if (!fs.existsSync(harness)) {
    process.stderr.write(`webgpu-verify: Dawn harness not found at ${harness}\n`)
    process.exit(1)
  }

  const scriptDir = 'tests/scripts/brush_backends'
  const outDir = `${dir}/webgpu_verify_out`
  ensureDir(outDir)
  const scripts = fs
    .readdirSync(scriptDir)
    .filter((f) => f.endsWith('_ab.txt'))
    .sort()
  if (scripts.length === 0) {
    process.stderr.write(`webgpu-verify: no *_ab.txt scripts in ${scriptDir}\n`)
    process.exit(1)
  }

  let failures = 0
  for (const s of scripts) {
    const brush = s.replace(/_ab\.txt$/, '')
    // No --backend override: the script's own set_backend drives it, so exactly
    // the wgsl stroke is captured into <brush>.json.
    const res = child_process.spawnSync(
      debugApp,
      ['--script', `${scriptDir}/${s}`, '--out', outDir, '--headless', '--gpu-capture', brush],
      {encoding: 'utf-8'}
    )
    if (res.status !== 0) {
      failures++
      process.stderr.write(`✗ ${brush}: debug_app exited ${res.status}\n`)
      if (res.stderr) process.stderr.write(res.stderr.split('\n').slice(-8).join('\n') + '\n')
      continue
    }
    const fixture = `${outDir}/${brush}.json`
    if (!fs.existsSync(fixture)) {
      failures++
      process.stderr.write(`✗ ${brush}: no fixture captured at ${fixture}\n`)
      continue
    }
    // One Dawn process per fixture: @kmamal/gpu deadlocks on a second
    // instance/device in the same process, so isolate each replay.
    const rep = child_process.spawnSync('node', [harness, '--wgsl-dir', wgslDir, '--fixture', fixture], {
      encoding: 'utf-8',
    })
    const pass = (rep.stdout || '').split('\n').find((l) => l.startsWith('PASS'))
    if (rep.status === 0 && pass) {
      console.log(`✓ ${brush}: ${pass.replace(/^PASS\s*/, '')}`)
    } else {
      failures++
      const detail = (rep.stderr || '')
        .split('\n')
        .filter((l) => l.trim())
        .slice(-4)
        .join('\n  ')
      process.stderr.write(`✗ ${brush}: webgpu replay failed\n  ${detail}\n`)
    }
  }

  if (failures) {
    process.stderr.write(`\nwebgpu-verify: ${failures} brush(es) failed.\n`)
    process.exit(1)
  }
  console.log(`\nwebgpu-verify: all ${scripts.length} brush(es) passed on WebGPU.`)
}

// Compute analogue of the Phase-2 render parity: proves the *shipping* C++
// WebGPU compute backend (source/webgpu/wgpu_compute, run through wgpu-native)
// is bit-modulo-fp identical to the C++ reference across every brush — now
// through the real backend the app uses, not the @kmamal/gpu replay. Reuses
// the existing A/B scripts: each runs its cpp stroke as-is, but the wgsl pass
// is rewritten to `set_backend backend=webgpu` so the same stroke is dispatched
// through wgpu_compute; the two dumps are diffed within the verify tolerance.
async function wgpuNativeVerify() {
  const dir = buildDir('native')
  const env = envPrefix('native')
  ensureDir(dir)

  // The native WebGPU compute path layers on the spirv GPU-dispatch gate, so it
  // needs cpp+wgsl+spirv plus -DSBRUSH_WEBGPU_COMPUTE=ON. Build debug_app and
  // both kernel sets (spirv for the dispatch gate; wgsl is what wgpu_compute loads).
  const sbrushFlags = sbrushBackendFlags('cpp,wgsl,spirv')
  run(
    `cd ${dir} && ${env} cmake ../.. -G Ninja ${nativeToolchainFlag()}-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} ${sbrushFlags} -DSBRUSH_WEBGPU_COMPUTE=ON`
  )
  await runBuild(`cd ${dir} && ${env} cmake --build . --target debug_app sbrush-spirv sbrush-wgsl${parallelFlag()}`)

  const debugApp = `${dir}/source/debug/debug_app${process.platform === 'win32' ? '.exe' : ''}`
  if (!fs.existsSync(debugApp)) {
    process.stderr.write(`wgpu-native-verify: debug_app not found at ${debugApp}\n`)
    process.exit(1)
  }

  const scriptDir = 'tests/scripts/brush_backends'
  const outDir = `${dir}/wgpu_native_verify_out`
  ensureDir(outDir)
  const tmpDir = `${outDir}/scripts`
  ensureDir(tmpDir)

  const scripts = fs
    .readdirSync(scriptDir)
    .filter((f) => f.endsWith('_ab.txt'))
    .sort()
  if (scripts.length === 0) {
    process.stderr.write(`wgpu-native-verify: no *_ab.txt scripts in ${scriptDir}\n`)
    process.exit(1)
  }

  // The WgpuNative dispatcher (source/webgpu/wgpu_compute) does not yet
  // implement setAttr/readbackAttr or the face-stage buffers, so attr/face
  // brushes abort on it. Skip those scripts here (they're covered by the
  // Vulkan/Dawn path) until WgpuNative grows attr support — see the GPU-attr
  // implementation-scope note in documentation/plans/boundary-conditions.md.
  const ATTR_BRUSHES = new Set(['color', 'polygroup', 'bsmooth'])

  let failures = 0
  for (const s of scripts) {
    const brush = s.replace(/_ab\.txt$/, '')
    if (ATTR_BRUSHES.has(brush)) {
      console.log(`⊘ ${brush}: skipped (WgpuNative lacks attr/face dispatch)`)
      continue
    }
    // Rewrite the wgsl pass to the native webgpu backend; cpp pass is untouched.
    const text = fs
      .readFileSync(`${scriptDir}/${s}`, 'utf-8')
      .split('backend=wgsl')
      .join('backend=webgpu')
      .split('_wgsl.json')
      .join('_webgpu.json')
    const tmpScript = `${tmpDir}/${brush}_webgpu.txt`
    fs.writeFileSync(tmpScript, text)

    const res = child_process.spawnSync(debugApp, ['--script', tmpScript, '--out', outDir, '--headless'], {
      encoding: 'utf-8',
    })
    if (res.status !== 0) {
      failures++
      process.stderr.write(`✗ ${brush}: debug_app exited ${res.status}\n`)
      if (res.stderr) process.stderr.write(res.stderr.split('\n').slice(-8).join('\n') + '\n')
      continue
    }

    const cpp = readDumpJson(`${outDir}/${brush}_cpp.json`)
    const wg = readDumpJson(`${outDir}/${brush}_webgpu.json`)
    if (!cpp || !wg) {
      failures++
      process.stderr.write(`✗ ${brush}: missing/invalid dump (cpp=${!!cpp} webgpu=${!!wg})\n`)
      continue
    }
    const ab = diffDump(cpp, wg)
    if (ab.length) {
      failures++
      process.stderr.write(`✗ ${brush}: cpp vs webgpu mismatch:\n  ${ab.slice(0, 6).join('\n  ')}\n`)
      continue
    }
    console.log(`✓ ${brush}: cpp == webgpu (native wgpu-native compute)`)
  }

  if (failures) {
    process.stderr.write(`\nwgpu-native-verify: ${failures} brush(es) failed.\n`)
    process.exit(1)
  }
  console.log(`\nwgpu-native-verify: all ${scripts.length} brush(es) passed on native WebGPU.`)
}

function setupPNPM() {
  const invokePNPM = (str, cmd) => {
    const cwd = process.cwd()
    process.chdir('source/litestl/tests')
    run('pnpm ' + cmd)
    process.chdir(cwd)
  }
  // in theory if we have pnpm-workspace.yaml property set up we just
  // need this single invocation
  run('pnpm i')

  //invokePNPM('source/litestl/tests', 'i')
  //invokePNPM('tests', 'i')
  //invokePNPM('tools', 'i')
}

const targetPositional = (y) =>
  y.positional('target', {
    choices : ['wasm', 'native'],
    default : 'wasm',
    describe: 'Build target',
  })

// Known sbrush backends, matching the SBRUSH_BACKEND_<X> CMake options.
const SBRUSH_BACKENDS = ['cpp', 'wgsl', 'spirv', 'cuda', 'hip', 'opencl']

// Parse `--backends=cpp,wgsl,...` into the `-DSBRUSH_BACKEND_<X>=ON`
// flags consumed by configure. `cpp` is always implicitly on (it's the
// reference emitter and the only one that links into libbrush).
function sbrushBackendFlags(backendsArg) {
  if (!backendsArg) return ''
  const picked = backendsArg
    .split(',')
    .map((s) => s.trim().toLowerCase())
    .filter(Boolean)
  const unknown = picked.filter((b) => !SBRUSH_BACKENDS.includes(b))
  if (unknown.length) {
    process.stderr.write(`unknown sbrush backend(s): ${unknown.join(', ')}\n`)
    process.stderr.write(`valid: ${SBRUSH_BACKENDS.join(', ')}\n`)
    process.exit(2)
  }
  const flags = []
  for (const b of SBRUSH_BACKENDS) {
    if (b === 'cpp') continue // always on
    flags.push(`-DSBRUSH_BACKEND_${b.toUpperCase()}=${picked.includes(b) ? 'ON' : 'OFF'}`)
  }
  return flags.join(' ')
}

yargs(hideBin(process.argv))
  .scriptName('make.mjs')
  .option('jobs', {
    alias   : 'j',
    type    : 'number',
    describe: 'Max parallel compile jobs for cmake --build (default: all cores). Lower it (e.g. -j 2) if clang OOMs.',
  })
  .middleware((argv) => {
    if (argv.jobs && argv.jobs > 0) {
      JOBS = argv.jobs
    }
  })
  .command(
    'configure [target]',
    'Configure the build',
    (y) =>
      targetPositional(y).option('backends', {
        type    : 'string',
        describe: `comma-separated sbrush backends to enable (subset of: ${SBRUSH_BACKENDS.join(',')}); cpp is always on`,
      }),
    async ({target, backends}) => {
      setupPNPM()
      ensureDir('build')
      const dir = buildDir(target)
      ensureDir(dir)
      const env = envPrefix(target)
      const sbrushFlags = sbrushBackendFlags(backends)
      if (target === 'native') {
        // Build + register the cross-worktree sccache launcher before cmake
        // resolves it (build_files/native-clang.cmake). Best-effort: a no-op when
        // the superproject's tools dir is absent (sculptcore built standalone).
        const sccacheSetup = Path.resolve('../tools/sccache-wrapper/setup.mjs')
        if (fs.existsSync(sccacheSetup)) {
          run(`node "${sccacheSetup}"`)
        }
        // Fetch-or-build the prebuilt native deps (OpenBLAS + SuiteSparse/CHOLMOD)
        // for this config, then hand cmake the combo dir. cmake wants forward slashes.
        const depsDir = await ensureDeps({config: configName(CMAKE_BUILD_TYPE)})
        const depsFlag = `-DSCULPTCORE_DEPS_DIR="${depsDir.replace(/\\/g, '/')}"`

        let NATIVE_CMAKE_ARGS = CMAKE_ARGS_BASE
        NATIVE_CMAKE_ARGS += ` -DWITH_ASAN=${WITH_ASAN ? 'ON' : 'OFF'} `
        NATIVE_CMAKE_ARGS += ` ${nativeToolchainFlag()}`
        NATIVE_CMAKE_ARGS += ` ${depsFlag} ${sbrushFlags}`

        run(`cd ${dir} && ${env} cmake ../.. ${NATIVE_CMAKE_ARGS}`)
      } else {
        run(
          `cd ${dir} && ${env} emcmake cmake .. ${CMAKE_WASM_ARGS} -DWITH_ASAN=${WITH_ASAN ? 'ON' : 'OFF'} ${sbrushFlags}`
        )
      }
    }
  )
  .command(
    'deps [config]',
    'Fetch-or-build the prebuilt native deps (OpenBLAS + SuiteSparse/CHOLMOD)',
    (y) =>
      y.positional('config', {
        type    : 'string',
        default : CMAKE_BUILD_TYPE,
        describe: 'build config: release | relwithdebinfo | debug | asan',
      }),
    async ({config}) => {
      const dir = await ensureDeps({config: configName(config)})
      console.log(`deps: ready at ${dir}`)
    }
  )
  .command('build [target]', 'Build', targetPositional, async ({target}) => {
    console.log('Building...')
    const dir = buildDir(target)
    const env = envPrefix(target)
    if (target === 'wasm') {
      deleteFinalWasmFiles()
    }

    await sbrushCodegen()
    await runBuild(`cd ${dir} && ${env} cmake --build .${parallelFlag()} `)

    if (target === 'wasm') {
      // copy wasm to typescript/
      fs.mkdirSync('typescript/build', {recursive: true})
      fs.copyFileSync('build/sculptcore.js', 'typescript/build/sculptcore.js')
      fs.copyFileSync('build/sculptcore.wasm', 'typescript/build/sculptcore.wasm')
      fs.copyFileSync('build/sculptcore-browser.js', 'typescript/build/sculptcore-browser.js')
      fs.copyFileSync('build/sculptcore-browser.wasm', 'typescript/build/sculptcore-browser.wasm')

      run('cd tools && pnpm build')
    }
  })
  .command('fullclean', 'Clean build files and node_module dirs', {}, () => {
    console.log('Full clean')
    if (fs.existsSync('build')) {
      fs.rmSync('build', {recursive: true, force: true})
    }

    const walk = (dir, cb) => {
      fs.readdirSync(dir).forEach((entry) => {
        const isDir = fs.statSync(`${dir}/${entry}`).isDirectory()
        if (isDir) {
          cb(Path.join(dir, entry), true, () => walk(Path.join(dir, entry), cb))
        } else {
          cb(Path.join(dir, entry), false)
        }
      })
    }
    walk(process.cwd(), (path, isDir, visitDir) => {
      if (Path.dirname(path).startsWith('.') || path.search(/emsdk/) !== -1 || !isDir) {
        return
      }
      if (path.search(/node_modules/) !== -1) {
        console.log(path)
        fs.rmSync(path, {recursive: true, force: true})
      } else {
        visitDir(path)
      }
    })
  })
  .command('clean [target]', 'Clean build dir', targetPositional, ({target}) => {
    console.log('Cleaning...')
    run(`cd ${buildDir(target)} && ${envPrefix(target)} ninja clean`)
  })
  .command('test [targetTest]', 'Run ctest', targetPositional, ({targetTest}) => {
    if (!targetTest) {
      run(`cd ${buildDir('native')} && ${envPrefix('native')} ctest .`)
    } else {
      const stem = `${targetTest}.cc_out`
      const candidates = process.platform === 'win32' ? [stem + '.exe', stem] : [stem, stem + '.exe']
      const dirs = ['build/native/tests', 'build/native/source/litestl/tests']
      for (const dir of dirs) {
        for (const name of candidates) {
          const path = Path.join(dir, name)
          if (fs.existsSync(path)) {
            run(path, {shell: false})
            return
          }
        }
      }
      process.stderr.write(`Could not find test ${targetTest}\n`)
      process.exit(-1)
    }
  })
  .command('codegen', 'Compile .sbrush kernels to backend sources', {}, async () => {
    await sbrushCodegen()
  })
  .command('sbrush-build', 'Alias for codegen', {}, async () => {
    await sbrushCodegen()
  })
  .command('sbrush-clean', 'Remove generated sbrush outputs', {}, () => {
    const gen = 'source/brush/kernels/generated'
    if (fs.existsSync(gen)) {
      fs.rmSync(gen, {recursive: true, force: true})
      console.log(`removed ${gen}`)
    }
    const wgslOut = 'build/native/sbrush_out'
    if (fs.existsSync(wgslOut)) {
      fs.rmSync(wgslOut, {recursive: true, force: true})
      console.log(`removed ${wgslOut}`)
    }
  })
  .command(
    'sbrush-validate <backend>',
    'Reconfigure native with the given sbrush backend enabled (with SBRUSH_VALIDATE_ALL=ON) and run its validator pass',
    (y) =>
      y.positional('backend', {
        choices : SBRUSH_BACKENDS.filter((b) => b !== 'cpp'),
        describe: 'sbrush backend to validate (cpp has no external validator)',
      }),
    async ({backend}) => {
      const dir = buildDir('native')
      ensureDir(dir)
      const env = envPrefix('native')
      const flag = `-DSBRUSH_BACKEND_${backend.toUpperCase()}=ON`
      run(
        `cd ${dir} && ${env} cmake ../.. -G Ninja ${nativeToolchainFlag()}-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} ${flag} -DSBRUSH_VALIDATE_ALL=ON`
      )
      await runBuild(`cd ${dir} && ${env} cmake --build . --target sbrush-${backend}${parallelFlag()}`)
    }
  )
  .command(
    'sbrush-verify',
    'Run per-brush A/B scripts through debug_app: cross-backend (cpp vs wgsl) + golden regression',
    (y) =>
      y.option('regen', {
        type    : 'boolean',
        default : false,
        describe: '(re)write tests/golden/<brush>.json references from the cpp dump',
      }),
    async ({regen}) => {
      await sbrushVerify(regen)
    }
  )
  .command(
    'webgpu-verify',
    'Replay sbrush WGSL kernels through Dawn (WebGPU) and diff against the native GPU dispatch',
    {},
    async () => {
      await webgpuVerify()
    }
  )
  .command(
    'wgpu-native-verify',
    'Run each brush A/B script through the native wgpu-native compute backend and diff cpp vs webgpu',
    {},
    async () => {
      await wgpuNativeVerify()
    }
  )
  .command(
    'node',
    'Build the Node/Electron N-API addon (.node) via cmake-js + clang',
    (y) =>
      y
        .option('electron-version', {
          type    : 'string',
          describe: 'Electron version to target (default: read from ../electron/package.json)',
        })
        .option('smoke', {
          type    : 'boolean',
          default : false,
          describe: 'After building, load the .node in Electron and call version()/bindingCount()',
        }),
    async ({electronVersion, smoke}) => {
      await sbrushCodegen()
      await buildNodeAddon(electronVersion, smoke)
    }
  )
  .command('install-tools', 'Install host build tools (naga)', {}, () => {
    console.log(`Installing naga-cli ${NAGA_VERSION}...`)
    try {
      const v = child_process.execSync('naga --version', {stdio: 'pipe'}).toString().trim()
      if (v.includes(NAGA_VERSION)) {
        console.log(`naga ${v} already on PATH`)
        return
      }
      console.log(`Found ${v}, installing pinned version`)
    } catch {
      console.log('naga not found on PATH, installing via cargo')
    }
    try {
      child_process.execSync('cargo --version', {stdio: 'pipe'})
    } catch {
      process.stderr.write(
        `cargo not found. Install Rust (https://rustup.rs/) so 'cargo install naga-cli' is available.\n`
      )
      process.exit(1)
    }
    run(`cargo install --force --version ${NAGA_VERSION} naga-cli`)
  })
  .command('fetch-wgpu-native', 'Download the pinned wgpu-native prebuilt (native WebGPU backend)', {}, () => {
    run('node extern/wgpu_native/fetch.mjs')
  })
  .command('install-emsdk', 'Install pinned emsdk', {}, () => {
    console.log('Installing emsdk...')
    run('git clone https://github.com/emscripten-core/emsdk.git emsdk')
    run(`cd emsdk && git checkout ${EMSDK_COMMIT}`)
    run(
      `cd emsdk && bash emsdk install ${EMSDK_VERSION} && bash emsdk install cmake-4.2.0-rc3-64bit ninja-git-release-64bit `
    )
    run(`cd emsdk && bash emsdk activate ${EMSDK_VERSION} cmake-4.2.0-rc3-64bit ninja-git-release-64bit --permanent `)
    // emsdk annoyingly is missing a .gitignore for their cmake binary folder
    fs.appendFileSync('emsdk/.gitignore', '\ncmake\n')
  })
  .demandCommand(1, 'Specify a command (see --help)')
  .strict()
  .help()
  .parse()
