#!/usr/bin/env node
import Path from 'path'
import fs from 'fs'
import os from 'os'
import child_process from 'child_process'
import yargs from 'yargs'
import {hideBin} from 'yargs/helpers'
import {fileURLToPath} from 'url'
import {termColor} from './source/litestl/tests/termColor.js'
import {syntaxHighlight} from './tools/syntaxHighlight.mjs'
import {ensureDeps, configName} from './tools/deps.mjs'
import {archivePdb} from './crash/dump.mjs'

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
//
// `SCULPTCORE_CMAKE_BUILD_TYPE` (set by Blender's CMake integration) overrides
// the local option so the bundled DLL and its native deps match the config the
// superproject is building — a farm build of Blender in Release then stages a
// Release engine without a local-build-options.mjs edit.
const CMAKE_BUILD_TYPE = process.env.SCULPTCORE_CMAKE_BUILD_TYPE || getopt('CMAKE_BUILD_TYPE', 'RelWithDebInfo')
const CMAKE_GENERATOR = getopt('CMAKE_GENERATOR', 'Ninja')
const WITH_ASAN = getopt('WITH_ASAN', false)
const WITH_MESHLOG_ABSEIL_HASHMAP = getopt('WITH_MESHLOG_ABSEIL_HASHMAP', false)
// Use MSVC (cl.exe) instead of clang for native + node-addon builds. Each
// toolchain gets its own build dir so the two trees never clash (cmake errors
// hard if the compiler changes under an existing build dir).
const WITH_NATIVE_MSVC = getopt('WITH_NATIVE_MSVC', false)
// Max parallel compile jobs for every build mode (wasm/native/node, plus the
// debug/sbrush targets and local deps builds). 0 = all cores. The -j/--jobs
// CLI flag overrides it per invocation.
const BUILD_JOBS = getopt('BUILD_JOBS', 0)

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
// Response files keep em++.bat's cmd.exe invocations under the Windows 8191-
// char limit — a long worktree path blows past it via the -I include list.
const CMAKE_WASM_ARGS =
  CMAKE_ARGS_BASE + ` -DBUILD_WASM=ON` + (process.platform === 'win32' ? ' -DCMAKE_NINJA_FORCE_RESPONSE_FILE=ON' : '')
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

/* Max parallel compile jobs for `cmake --build`, defaulted from the BUILD_JOBS
 * local option and overridden by the global -j/--jobs flag (see the yargs
 * middleware). Undefined = let the generator use all cores. Lower it (e.g.
 * `-j 2`) when clang OOMs on the heavy template files. */
let JOBS = Number(BUILD_JOBS) > 0 ? Number(BUILD_JOBS) : undefined
function parallelFlag() {
  return JOBS && JOBS > 0 ? ` --parallel ${JOBS}` : ''
}

/* Opt-in: pipe a long failing build log through `claude` for a summary. */
const SUMMARIZE_ERRORS = false

function summarizeErrors(buf) {
  return new Promise((accept, reject) => {
    // Every path must settle. runBuild() awaits this before propagating a build
    // failure, so a promise that never resolves strands that await, drains the
    // event loop, and exits 0 on a broken build.
    if (!SUMMARIZE_ERRORS || buf.length < 2048 * 80) {
      accept(0)
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

        // Set eagerly: if anything below strands, node still exits non-zero
        // rather than reporting a failed build as success.
        process.exitCode = code

        try {
          await summarizeErrors('==== stderr =====\n' + stderr.fullBuf + '==== stdout =====\n' + stdout.fullBuf)
        } catch (error) {
          process.stderr.write(`summarizeErrors failed: ${error?.message ?? error}\n`)
        }

        process.stderr.write(termColor(`cmd "${cmd}" exited with code ${code}\n`, 'red'))
        process.exit(code)
      }
      accept(code)
    })
  })
}
function ensureDir(p) {
  if (!fs.existsSync(p)) fs.mkdirSync(p, {recursive: true})
}

// Normalize a user-supplied target name. `emsdk` is accepted as an alias for the
// canonical `wasm` (the Emscripten/WASM build) so both spellings work.
function normalizeTarget(target) {
  return target === 'emsdk' ? 'wasm' : target
}

function buildDir(target) {
  target = normalizeTarget(target)
  if (target === 'native') {
    return WITH_NATIVE_MSVC ? 'build/native-msvc' : 'build/native'
  }
  if (target === 'node') {
    return WITH_NATIVE_MSVC ? 'build/native-node-msvc' : 'build/native-node'
  }
  if (target === 'python') {
    return WITH_NATIVE_MSVC ? 'build/python-msvc' : 'build/python'
  }
  return 'build'
}

// Returns the `node ../configureEnv.mjs [--emsdk]` prefix used inside buildDir.
function envPrefix(target) {
  const nativeLike = target === 'native' || target === 'python'
  const rel = nativeLike ? '../..' : '..'
  const emsdk = nativeLike ? '' : '--emsdk '
  return `node ${rel}/configureEnv.mjs ${emsdk}`.trimEnd()
}

// Native toolchain file, written relative to the build dir (build/native or
// build/native-msvc). clang is the project default; WITH_NATIVE_MSVC switches
// to cl.exe (build_files/native-msvc.cmake).
function nativeToolchainFlag() {
  const tc = WITH_NATIVE_MSVC ? 'native-msvc.cmake' : 'native-clang.cmake'
  return `--toolchain ../../build_files/${tc} `
}

// === Node / Electron N-API addon ===
//
// Builds sculptcore_node.node (Workstream A of documentation/plans/
// native-electron.md). cmake-js drives the *configure* step — it downloads the
// Electron headers + node.lib and injects CMAKE_JS_* — then we build only the
// addon target with the repo's clang toolchain. A dedicated build dir keeps the
// normal build/native (default CRT) untouched. The clang↔Electron link was
// de-risked in sculptcore/spike/napi/ (see RESULTS.md).

// The NW.js manifest is the repo-root package.json (one level above sculptcore/).
// Its `nw` devDependency is pinned like "0.95.0-sdk"; we want the bare version.
function readNwjsVersion() {
  try {
    const pkg = JSON.parse(fs.readFileSync('../package.json', 'utf-8'))
    const v = pkg.devDependencies?.nw ?? pkg.dependencies?.nw ?? ''
    const m = v.match(/\d+\.\d+\.\d+/)
    if (m) return m[0]
  } catch {
    /* fall through to default */
  }
  return '0.95.0'
}

function readElectronVersion() {
  try {
    const pkg = JSON.parse(fs.readFileSync('../package.json', 'utf-8'))
    const v = pkg.devDependencies?.electron ?? pkg.dependencies?.electron ?? ''
    const m = v.match(/\d+\.\d+\.\d+/)
    if (m) return m[0]
  } catch {
    /* fall through to default */
  }
  return '41.1.1'
}

// Resolve a runtime's executable path. `require('electron')` returns its path
// directly; the `nw` package instead exposes findpath() → the cached binary.
// cwd = the repo root (where both deps resolve).
function resolveRuntimeExe(runtime) {
  const expr =
    runtime === 'electron'
      ? `require('electron')`
      : `require('nw').findpath().then(p=>process.stdout.write(p),()=>process.exit(1))`
  const cmd = runtime === 'electron' ? `node -p "${expr}"` : `node -e "${expr}"`
  try {
    const out = child_process.execSync(cmd, {cwd: '..', encoding: 'utf-8'}).trim()
    return out || undefined
  } catch {
    return undefined
  }
}

// Pre-populate cmake-js's per-version NW.js cache from dl.nwjs.io.
//
// cmake-js downloads NW.js dev files from the legacy node-webkit S3 mirror,
// which no longer carries current releases (e.g. v0.112.0 → 404), so it writes
// an empty nw.lib the linker then rejects ("unknown file type"). NW.js 0.111.3+
// instead publishes standard Node-compatible headers + import lib at
// dl.nwjs.io. We lay those into the cache cmake-js reads (its `downloaded` check
// in lib/dist.js wants src/node.h + deps/v8/include/v8.h + the winLibs), so its
// own broken download is skipped. Idempotent — a complete cache is left alone.
function provisionNwjsCache(ver) {
  const cacheDir = Path.join(os.homedir(), '.cmake-js', 'nw-x64', `v${ver}`)
  const libDir = Path.join(cacheDir, 'x64')
  const nodeLib = Path.join(libDir, 'node.lib')
  const nwLib = Path.join(libDir, 'nw.lib')
  const srcNodeH = Path.join(cacheDir, 'src', 'node.h')
  const v8H = Path.join(cacheDir, 'deps', 'v8', 'include', 'v8.h')
  if (fs.existsSync(srcNodeH) && fs.existsSync(v8H) && fs.existsSync(nodeLib) && fs.existsSync(nwLib)) {
    return
  }
  const base = `https://dl.nwjs.io/v${ver}`
  console.log(`nwjs: provisioning cmake-js header/lib cache for nw ${ver} from ${base}`)
  fs.mkdirSync(libDir, {recursive: true})
  // Import lib. NW.js 0.111.3+ is node-ABI-compatible and ships only node.lib;
  // cmake-js's CMAKE_JS_LIB still lists nw.lib (same DLL exports), so mirror it.
  run(`curl -fSL -o "${nodeLib}" "${base}/x64/node.lib"`)
  fs.copyFileSync(nodeLib, nwLib)
  // Headers: only the full source tarball is published (the node-gyp "-headers"
  // variant 404s). Extract it flat into the version cache — cmake-js's nw include
  // dirs cover src/, where the raw C N-API header (node_api.h) the addon uses lives.
  const tmp = fs.mkdtempSync(Path.join(os.tmpdir(), 'nwhdr-'))
  try {
    const tgz = Path.join(tmp, 'src.tar.gz')
    run(`curl -fSL -o "${tgz}" "${base}/node-v${ver}.tar.gz"`)
    // Feed the archive on stdin (`-f -`): a `C:\…` archive arg makes git-bash's
    // GNU tar treat the drive letter as a remote host. Forward-slash the `-C`
    // dir for the same reason. Works for both GNU tar and Windows bsdtar.
    try {
      child_process.execSync(`tar -xzf - -C "${cacheDir.replace(/\\/g, '/')}" --strip-components=1`, {
        input: fs.readFileSync(tgz),
        stdio: ['pipe', 'inherit', 'inherit'],
      })
    } catch (error) {
      process.stderr.write(error.message + '\n')
      process.exit(1)
    }
  } finally {
    fs.rmSync(tmp, {recursive: true, force: true})
  }
}

// Resolve the runtime ABI version, defaulting from the workspace package.json.
function resolveRuntimeVersion(runtime, version) {
  return version || (runtime === 'electron' ? readElectronVersion() : readNwjsVersion())
}

// Configure the Node N-API addon via cmake-js: downloads the runtime headers +
// import lib and injects CMAKE_JS_INC/LIB/SRC, using the repo's clang toolchain
// (or MSVC under WITH_NATIVE_MSVC) + Ninja, like the rest of the native tree.
// `-r nw` targets the NW.js ABI; `-r electron` the Electron one.
async function configureNodeAddon(runtime, version) {
  runtime = runtime === 'electron' ? 'electron' : 'nw'
  const dir = buildDir('node')
  ensureDir(dir)
  const ver = resolveRuntimeVersion(runtime, version)
  const toolchainFile = WITH_NATIVE_MSVC ? 'native-msvc.cmake' : 'native-clang.cmake'
  const toolchain = Path.resolve('build_files', toolchainFile).replace(/\\/g, '/')
  const cmakeJs = 'node node_modules/cmake-js/bin/cmake-js'
  // Native env prefix, run from the sculptcore root (where configureEnv.mjs is).
  const env = 'node configureEnv.mjs'

  console.log(`Configuring Node addon for ${runtime} ${ver} -> ${dir}`)

  // Prebuilt OpenBLAS + SuiteSparse/CHOLMOD, same as `configure native`, so the
  // addon links the cholmod target instead of warning it off.
  const depsDir = await ensureDeps({config: configName(CMAKE_BUILD_TYPE), jobs: JOBS})
  const depsDef = `--CDSCULPTCORE_DEPS_DIR=${depsDir.replace(/\\/g, '/')}`

  // cmake-js defaults to the static CRT (/MT); force the dynamic CRT so the
  // addon matches the runtime, the rest of the native tree, and the prebuilt
  // /MD deps (mismatched CRTs surface as undefined dllimport CRT symbols).
  const crtDef = '--CDCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL'

  // NW.js dev files come from dl.nwjs.io (cmake-js's built-in mirror is stale);
  // lay them into cmake-js's cache so its own download is skipped. See
  // provisionNwjsCache. Electron's cmake-js download still works as-is.
  if (runtime === 'nw') {
    provisionNwjsCache(ver)
  }

  run(
    `${env} "${cmakeJs} configure -O ${dir} -G Ninja --CDWITH_ASAN=${WITH_ASAN ? 'ON' : 'OFF'} --CDCMAKE_TOOLCHAIN_FILE=${toolchain} ${depsDef} ${crtDef} -r ${runtime} -v ${ver} -a x64"`
  )
}

async function buildNodeAddon(runtime, version, smoke) {
  runtime = runtime === 'electron' ? 'electron' : 'nw'
  const dir = buildDir('node')
  const env = 'node configureEnv.mjs'

  // Configure on demand if `build node` was run without a prior `configure node`.
  if (!fs.existsSync(Path.join(dir, 'CMakeCache.txt'))) {
    await configureNodeAddon(runtime, version)
  }

  console.log(`Building Node addon for ${runtime} -> ${dir}/sculptcore_node.node`)

  // Build ONLY the addon target. Its static deps come along; the SHARED
  // `sculptcore` lib is intentionally not built here (see the CMakeLists note
  // about the global /DELAYLOAD flag under the clang driver).
  await runBuild(`${env} "cmake --build ${dir} --target sculptcore_node${parallelFlag()}"`)

  const out = Path.resolve(dir, 'sculptcore_node.node').replace(/\\/g, '/')
  if (!fs.existsSync(out)) {
    process.stderr.write(`node: addon not found at ${out}\n`)
    process.exit(1)
  }
  console.log(`node: built ${out}`)

  // Crashpad: archive the addon's PDB into the content-addressed symbol store
  // (build/crashdumps/syms) keyed by its CodeView GUID, so minidumps stay
  // symbolizable after a rebuild. Best-effort (no-op without a PDB, e.g. WASM
  // or a non-Windows host). See documentation/plans/crashpad.md.
  const pdb = Path.resolve(dir, 'sculptcore_node.pdb')
  if (fs.existsSync(pdb)) {
    const stored = archivePdb(out, pdb)
    if (stored) console.log(`node: archived PDB -> ${stored}`)
  }

  if (smoke) {
    if (runtime === 'electron') {
      // The shell is NW.js; --smoke is only wired for the nw runtime. The
      // shared smoke body (source/napi/napi_smoke.cjs) is shell-agnostic if an
      // Electron harness is ever reintroduced.
      console.warn('node: --smoke is only wired for the nw runtime; skipping electron smoke')
      return
    }
    smokeNwjs(out)
  }
}

// === Python ctypes C-API shared lib ===
//
// Builds sculptcore_capi — the aggregate shared lib force-exporting the
// LSTL_* dispatch ABI plus the engine c-api symbols — into build/python/,
// for the Python ctypes runtime (python/sculptcore). Configures on demand
// like the node addon. No host-side sbrush codegen pass: the native
// configure regenerates kernels in-build (SBRUSH_REGEN_ON_BUILD).
async function buildPythonCapi() {
  const dir = buildDir('python')
  const env = envPrefix('python')

  if (!fs.existsSync(Path.join(dir, 'CMakeCache.txt'))) {
    await configureTarget('python', {})
  }

  console.log(`Building Python C-API lib -> ${dir}`)
  await runBuild(`cd ${dir} && ${env} cmake --build . --target sculptcore_capi${parallelFlag()}`)

  const name =
    process.platform === 'win32'
      ? 'sculptcore_capi.dll'
      : process.platform === 'darwin'
        ? 'libsculptcore_capi.dylib'
        : 'libsculptcore_capi.so'
  const out = Path.resolve(dir, name)
  if (!fs.existsSync(out)) {
    process.stderr.write(`python: capi lib not found at ${out}\n`)
    process.exit(1)
  }
  console.log(`python: built ${out}`)
}

// === Addon bundle ===
//
// `bundle` vendors the ctypes package + capi lib into a Blender addon's
// lib/sculptcore/ so Blender needs no SCULPTCORE_* env vars: engine.py already
// prefers a vendored lib/, and _capi.py finds the lib beside the package
// (wgpu_native resolves via add_dll_directory on the same dir).

// Shared-library names staged from build/python, per platform.
function bundleLibNames() {
  if (process.platform === 'win32') {
    return ['sculptcore_capi.dll', 'wgpu_native.dll']
  }
  if (process.platform === 'darwin') {
    return ['libsculptcore_capi.dylib', 'libwgpu_native.dylib']
  }
  return ['libsculptcore_capi.so', 'libwgpu_native.so']
}

// Relative paths of every file in the python/sculptcore package (caches skipped).
function bundlePackageFiles(root) {
  const out = []
  const walk = (rel) => {
    for (const entry of fs.readdirSync(Path.join(root, rel), {withFileTypes: true})) {
      if (entry.name === '__pycache__' || entry.name === '.mypy_cache') {
        continue
      }
      const relPath = rel ? Path.join(rel, entry.name) : entry.name
      if (entry.isDirectory()) {
        walk(relPath)
      } else {
        out.push(relPath)
      }
    }
  }
  walk('')
  return out
}

/* Destination package dirs. Explicit dest: <dest>/sculptcore only. Default:
 * the superproject's addon (../../scripts/addons_core/sculptcore_addon) plus
 * every sibling build tree's addon copy — blender.exe runs the build-tree
 * copy, and nothing refreshes it between Blender's own install steps. */
function bundleDests(destArg) {
  if (destArg) {
    return [Path.resolve(destArg, 'sculptcore')]
  }
  const addon = Path.resolve('../../scripts/addons_core/sculptcore_addon')
  if (!fs.existsSync(Path.join(addon, '__init__.py'))) {
    process.stderr.write(`bundle: no Blender addon at ${addon}; pass an explicit dest\n`)
    process.exit(2)
  }
  const dests = [Path.join(addon, 'lib', 'sculptcore')]
  const parent = Path.resolve('../../..')
  for (const entry of fs.readdirSync(parent)) {
    if (!entry.startsWith('build_')) {
      continue
    }
    const bin = Path.join(parent, entry, 'bin')
    if (!fs.existsSync(bin)) {
      continue
    }
    for (const ver of fs.readdirSync(bin)) {
      const copy = Path.join(bin, ver, 'scripts', 'addons_core', 'sculptcore_addon')
      if (fs.existsSync(Path.join(copy, '__init__.py'))) {
        dests.push(Path.join(copy, 'lib', 'sculptcore'))
      }
    }
  }
  return dests
}

// Copy src over dst; when dst is a shared lib loaded by a running Blender
// (win32 EPERM/EBUSY), rename it aside — Windows allows renaming a loaded
// module — and copy fresh. Returns true when the hot-swap path was taken.
function bundleCopy(src, dst) {
  try {
    fs.copyFileSync(src, dst)
    return false
  } catch (err) {
    if (process.platform !== 'win32' || !['EPERM', 'EBUSY', 'EACCES'].includes(err.code)) {
      throw err
    }
    let n = 0
    while (fs.existsSync(`${dst}.stale${n}`)) {
      n++
    }
    fs.renameSync(dst, `${dst}.stale${n}`)
    fs.copyFileSync(src, dst)
    return true
  }
}

// Recursively copy a directory tree (deps combos are small; no need for a dep).
function copyTree(src, dst) {
  ensureDir(dst)
  for (const entry of fs.readdirSync(src, {withFileTypes: true})) {
    const s = Path.join(src, entry.name)
    const d = Path.join(dst, entry.name)
    if (entry.isDirectory()) {
      copyTree(s, d)
    } else {
      fs.copyFileSync(s, d)
    }
  }
}

// Stage the native-deps combo that the just-completed build produced into
// `<publishDir>/<comboRel>` — but only when it was built from source this run
// (a cache hit is already published). ensureDeps records the outcome in
// build/deps-last-fresh.json. Used by Blender's CMake integration to collect
// farm-built deps as a separate artifact for pushing back to sculptcore-deps.
function publishFreshDeps(publishDir) {
  const marker = 'build/deps-last-fresh.json'
  if (!fs.existsSync(marker)) {
    console.log('bundle: no deps marker; nothing to publish (deps were not (re)resolved this build)')
    return
  }
  let info
  try {
    info = JSON.parse(fs.readFileSync(marker, 'utf-8'))
  } catch {
    process.stderr.write(`bundle: could not parse ${marker}; skipping deps publish\n`)
    return
  }
  if (!info.fresh) {
    console.log(`bundle: deps combo ${info.comboRel} was a cache hit; not publishing`)
    return
  }
  if (!info.comboDir || !fs.existsSync(info.comboDir)) {
    process.stderr.write(`bundle: fresh deps combo missing at ${info.comboDir}; skipping publish\n`)
    return
  }
  const dst = Path.join(publishDir, info.comboRel)
  copyTree(info.comboDir, dst)
  console.log(`bundle: published fresh deps combo ${info.comboRel} -> ${dst}`)
}

async function bundleAddon(dest, {build, pdb, publishDepsTo}) {
  if (build) {
    await buildPythonCapi()
  }

  if (publishDepsTo) {
    publishFreshDeps(publishDepsTo)
  }

  const pkgRoot = 'python/sculptcore'
  const libDir = buildDir('python')
  const [capiName, wgpuName] = bundleLibNames()

  // {src, rel}: rel is the path under the destination sculptcore/ dir.
  const files = bundlePackageFiles(pkgRoot).map((rel) => ({src: Path.join(pkgRoot, rel), rel}))
  files.push({src: Path.join(libDir, capiName), rel: capiName})
  if (fs.existsSync(Path.join(libDir, wgpuName))) {
    files.push({src: Path.join(libDir, wgpuName), rel: wgpuName})
  } else {
    process.stderr.write(`bundle: warning: ${wgpuName} not found in ${libDir}, not staged\n`)
  }
  if (pdb && process.platform === 'win32' && fs.existsSync(Path.join(libDir, 'sculptcore_capi.pdb'))) {
    files.push({src: Path.join(libDir, 'sculptcore_capi.pdb'), rel: 'sculptcore_capi.pdb'})
  }

  let hotSwapped = false
  for (const destDir of bundleDests(dest)) {
    ensureDir(destDir)

    const staged = new Set(files.map((f) => Path.join(destDir, f.rel)))
    for (const f of files) {
      ensureDir(Path.dirname(Path.join(destDir, f.rel)))
      hotSwapped = bundleCopy(f.src, Path.join(destDir, f.rel)) || hotSwapped
    }

    // Mirror: drop anything not staged (removed sources, __pycache__, prior
    // .stale renames — locked ones survive until Blender exits, ignore those).
    const prune = (dir) => {
      for (const entry of fs.readdirSync(dir, {withFileTypes: true})) {
        const p = Path.join(dir, entry.name)
        if (entry.isDirectory()) {
          prune(p)
          if (fs.readdirSync(p).length === 0) {
            fs.rmdirSync(p)
          }
        } else if (!staged.has(p)) {
          try {
            fs.rmSync(p)
          } catch {}
        }
      }
    }
    prune(destDir)

    console.log(`bundle: staged ${files.length} files -> ${destDir}`)
  }
  if (hotSwapped) {
    console.log('bundle: a running Blender holds the old lib; restart it to pick up the new build')
  }
}

// Smoke-load the freshly built .node under NW.js. NW.js has no main process, so
// we drive the smoke from a hidden window: a generated temp app whose page
// requires the addon + the shared napi_smoke body, runs it, writes the result
// JSON, and quits. Then we read + report the result.
function smokeNwjs(addonPath) {
  const exe = resolveRuntimeExe('nw')
  if (!exe) {
    process.stderr.write('node: --smoke needs nw installed under ../nwjs (pnpm i)\n')
    process.exit(1)
  }

  const tmp = fs.mkdtempSync(Path.join(os.tmpdir(), 'scsmoke-'))
  const fwd = (p) => p.replace(/\\/g, '/')
  const outPath = fwd(Path.join(tmp, 'smoke-result.json'))
  const smokeBody = fwd(Path.resolve('source/napi/napi_smoke.cjs'))

  fs.writeFileSync(
    Path.join(tmp, 'package.json'),
    JSON.stringify({name: 'scsmoke', main: 'smoke.html', window: {show: false}}, null, 2)
  )
  fs.writeFileSync(
    Path.join(tmp, 'smoke.html'),
    `<!doctype html><html><head><script>
      const {runNapiSmoke} = require(${JSON.stringify(smokeBody)})
      const addon = require(${JSON.stringify(fwd(addonPath))})
      runNapiSmoke(addon, ${JSON.stringify(outPath)}, () => nw.App.quit())
    </script></head><body></body></html>`
  )

  run(`"${exe}" "${fwd(tmp)}"`)

  if (!fs.existsSync(outPath)) {
    process.stderr.write(`node: --smoke produced no result at ${outPath}\n`)
    process.exit(1)
  }
  const result = JSON.parse(fs.readFileSync(outPath, 'utf-8'))
  console.log('node: smoke result', JSON.stringify(result, null, 2))
  if (!result.ok) {
    process.stderr.write('node: --smoke reported failure\n')
    process.exit(1)
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

  const inputs = fs.readdirSync(kernelsDir).filter((f) => f.endsWith('.sbrush'))

  // CI / cross-compile escape hatch: trust the checked-in *.brush.gen.h headers
  // and skip building the native sbrushc host tool (which would drag in the full
  // native CMake configure — Vulkan, wgpu_native — none of which exist on the
  // WASM/Pages runner). Fail loudly if a header is missing so a stale checkout
  // can't silently ship outdated kernels.
  if (process.env.SBRUSH_SKIP_NATIVE_CODEGEN === '1') {
    const missing = inputs
      .map((inp) => `${outDir}/${inp.replace(/\.sbrush$/, '')}.brush.gen.h`)
      .concat(['typescript/sculptcore/brush/brushWgsl.ts'])
      .filter((p) => !fs.existsSync(p))
    if (missing.length) {
      process.stderr.write(
        `codegen: SBRUSH_SKIP_NATIVE_CODEGEN=1 but missing checked-in headers:\n  ${missing.join('\n  ')}\n` +
          `Run \`node make.mjs codegen\` natively and commit them.\n`
      )
      process.exit(1)
    }
    console.log('codegen: using checked-in *.brush.gen.h (native sbrushc skipped)')
    return
  }

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
    run(`cd ${nativeBuild} && ${envPrefix('native')} cmake --build . --target sbrushc${parallelFlag()}`)
    sbrushc = sbrushcCandidates.find((p) => fs.existsSync(p))
    if (!sbrushc) {
      process.stderr.write('codegen: sbrushc not found after build\n')
      process.exit(1)
    }
  }

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

  emitBrushWgslTs(sbrushc, kernelsDir, inputs)
}

// Emit the aggregated, committed WGSL kernel module the TS app dispatches from
// (documentation/plans/gpuGlobalBrushes.md D2). Runs sbrushc --backend=wgsl per
// kernel into a build staging dir, then bundles every kernel's text into
// typescript/sculptcore/brush/brushWgsl.ts so both the browser bundle and NW.js
// load kernels with zero runtime file access. Idempotent: the .ts is only
// rewritten when its content changes.
function emitBrushWgslTs(sbrushc, kernelsDir, inputs) {
  const stageDir = 'build/wgsl_ts'
  ensureDir(stageDir)

  const entries = []
  for (const inp of inputs) {
    const stem = inp.replace(/\.sbrush$/, '')
    const wgslPath = `${stageDir}/${stem}.wgsl`
    run(`"${sbrushc}" --backend=wgsl --in="${kernelsDir}/${inp}" --out="${wgslPath}"`)
    const wgsl = fs.readFileSync(wgslPath, 'utf-8')
    // Escape for a JS template literal (WGSL has none of these today, but a
    // future kernel must not silently corrupt the module).
    const escaped = wgsl.replace(/\\/g, '\\\\').replace(/`/g, '\\`').replace(/\$\{/g, '\\${')
    entries.push(`  ${JSON.stringify(stem)}: \`${escaped}\`,`)
  }

  const out =
    '// AUTO-GENERATED by `node make.mjs codegen` (sbrushc --backend=wgsl) — DO NOT EDIT.\n' +
    '// One entry per source/brush/kernels/*.sbrush kernel, keyed by kernel stem.\n' +
    '// Committed like the rest of the generated typescript/ tree so the app\n' +
    '// bundles the WGSL with zero runtime file access on every backend.\n\n' +
    'export const brushWgsl: Record<string, string> = {\n' +
    entries.join('\n') +
    '\n}\n'

  const tsPath = 'typescript/sculptcore/brush/brushWgsl.ts'
  const prev = fs.existsSync(tsPath) ? fs.readFileSync(tsPath, 'utf-8') : null
  if (prev !== out) {
    fs.writeFileSync(tsPath, out)
    console.log(`codegen: wrote ${tsPath}`)
  } else {
    console.log(`codegen: ${tsPath} up to date`)
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
    // Scripts that never run a wgsl-backend pass (e.g. smooth_csr's cpp-vs-cpp
    // neighbor-source A/B) capture no GPU fixture — nothing to replay here.
    if (!fs.readFileSync(`${scriptDir}/${s}`, 'utf-8').includes('backend=wgsl')) {
      console.log(`- ${brush}: no wgsl pass in script; skipped`)
      continue
    }
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

// Targets accepted by `configure` / `build`. `emsdk` is an alias for `wasm`.
const BUILD_TARGETS = ['wasm', 'emsdk', 'native', 'node', 'python']

// `configure [target]` — omitting the target configures all three (wasm, native,
// node), so it has no default.
const configureTargetPositional = (y) =>
  y.positional('target', {
    choices : BUILD_TARGETS,
    describe: 'Build target (omit to configure wasm, native and node)',
  })

// `build [target]` — defaults to wasm, like the historical behavior.
const buildTargetPositional = (y) =>
  y.positional('target', {
    choices : BUILD_TARGETS,
    default : 'wasm',
    describe: 'Build target (wasm | native | node | python)',
  })

// Runtime-ABI options shared by `configure node` / `build node` (ignored for the
// wasm and native targets).
const nodeRuntimeOptions = (y) =>
  y
    .option('runtime', {
      type    : 'string',
      choices : ['nw', 'electron'],
      default : 'nw',
      describe: 'Node addon target runtime ABI — node target only (nw default; electron fallback)',
    })
    .option('runtime-version', {
      type    : 'string',
      describe: 'Node addon runtime version — node target only (default: read from ../nwjs/package.json)',
    })

// Configure a single target's build dir. `node` goes through cmake-js
// (configureNodeAddon); wasm/native run (emcmake) cmake directly.
async function configureTarget(target, {backends, runtime, runtimeVersion}) {
  target = normalizeTarget(target)
  ensureDir('build')
  if (target === 'node') {
    await configureNodeAddon(runtime, runtimeVersion)
    return
  }
  const dir = buildDir(target)
  ensureDir(dir)
  const env = envPrefix(target)
  const sbrushFlags = sbrushBackendFlags(backends)
  if (target === 'native' || target === 'python') {
    // Build + register the cross-worktree sccache launcher before cmake
    // resolves it (build_files/native-clang.cmake). Best-effort: a no-op when
    // the superproject's tools dir is absent (sculptcore built standalone).
    const sccacheSetup = Path.resolve('../tools/sccache-wrapper/setup.mjs')
    if (fs.existsSync(sccacheSetup)) {
      run(`node "${sccacheSetup}"`)
    }
    // Fetch-or-build the prebuilt native deps (OpenBLAS + SuiteSparse/CHOLMOD)
    // for this config, then hand cmake the combo dir. cmake wants forward slashes.
    const depsDir = await ensureDeps({config: configName(CMAKE_BUILD_TYPE), jobs: JOBS})
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
    describe:
      'Max parallel compile jobs for cmake --build (default: the BUILD_JOBS local option, else all cores). Lower it (e.g. -j 2) if clang OOMs.',
  })
  .middleware((argv) => {
    if (argv.jobs && argv.jobs > 0) {
      JOBS = argv.jobs
    }
  })
  .command(
    'configure [target]',
    'Configure the build (omit target to configure wasm, native and node)',
    (y) =>
      nodeRuntimeOptions(configureTargetPositional(y)).option('backends', {
        type    : 'string',
        describe: `comma-separated sbrush backends to enable (subset of: ${SBRUSH_BACKENDS.join(',')}); cpp is always on`,
      }),
    async ({target, backends, runtime, runtimeVersion}) => {
      setupPNPM()
      const targets = target ? [normalizeTarget(target)] : ['wasm', 'native', 'node']
      for (const t of targets) {
        await configureTarget(t, {backends, runtime, runtimeVersion})
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
      const dir = await ensureDeps({config: configName(config), jobs: JOBS})
      console.log(`deps: ready at ${dir}`)
    }
  )
  .command(
    'build [target]',
    'Build (target: wasm | native | node)',
    (y) =>
      nodeRuntimeOptions(buildTargetPositional(y)).option('smoke', {
        type    : 'boolean',
        default : false,
        describe: 'node target only: after building, load the .node under NW.js and run the napi smoke',
      }),
    async ({target, runtime, runtimeVersion, smoke}) => {
      target = normalizeTarget(target)
      if (target === 'node') {
        await sbrushCodegen()
        await buildNodeAddon(runtime, runtimeVersion, smoke)
        return
      }
      if (target === 'python') {
        await buildPythonCapi()
        return
      }
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
    }
  )
  .command(
    'bundle [dest]',
    'Build the python capi lib and vendor it + the ctypes package into the Blender addon (lib/sculptcore)',
    (y) =>
      y
        .positional('dest', {
          type    : 'string',
          describe:
            'stage into <dest>/sculptcore instead of the detected Blender addon (source tree + sibling build trees)',
        })
        .option('build', {
          type    : 'boolean',
          default : true,
          describe: 'build the capi lib first (--no-build: restage existing outputs only)',
        })
        .option('pdb', {
          type    : 'boolean',
          default : false,
          describe: 'also stage sculptcore_capi.pdb (win32) for native debugging',
        })
        .option('publish-deps-to', {
          type    : 'string',
          describe:
            'if the native deps were built from source this run, copy the combo into <dir>/<platform>/<toolchain>/<config> for publishing back to sculptcore-deps',
        }),
    async ({dest, build, pdb, publishDepsTo}) => {
      await bundleAddon(dest, {build, pdb, publishDepsTo})
    }
  )
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
      const nbuild = buildDir('native')
      const dirs = [`${nbuild}/tests`, `${nbuild}/source/litestl/tests`]
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
