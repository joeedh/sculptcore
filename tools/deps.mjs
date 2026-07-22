#!/usr/bin/env node
// Prebuilt native-dependency cache (OpenBLAS + SuiteSparse/CHOLMOD).
//
// Deps are stashed per {platform}/{toolchain-key}/{config} in the gitignored
// sparse clone extern/sculptcore-deps (repo DEPS_REPO). ensureDeps()
// materializes the matching combo: a cache hit (manifest tags match the pins)
// is used as-is; otherwise OpenBLAS then SuiteSparse are built from their pinned
// tags into the combo's install trees. New combos are staged locally only — the
// commit/push back to sculptcore-deps is printed for the user to run.
//
// Pins are tracked in sculptcore/openblasVersion.txt and suitesparseVersion.txt
// (mirroring emsdkVersion.txt).
import fs from 'fs'
import path from 'path'
import child_process from 'child_process'
import {fileURLToPath} from 'url'

const here = path.dirname(fileURLToPath(import.meta.url))
const ROOT = path.resolve(here, '..') // sculptcore/
const CONFIG_ENV = path.join(ROOT, 'configureEnv.mjs')

// Build deps with plain clang (same compiler/ABI as the app) rather than
// sculptcore's native-clang.cmake toolchain: that toolchain injects the
// sccache-launcher wrapper, which mangles OpenBLAS's nested-quote -DVERSION
// define and interferes with its getarch probe. shEnv() still supplies the
// vcvars environment so `clang` resolves to the MSVC-bundled clang on Windows.
const COMPILER_ARGS = '-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++'

const DEPS_CLONE = path.join(ROOT, 'extern', 'sculptcore-deps')
const SRC_CACHE = path.join(ROOT, 'build', 'deps-src')
const BUILD_CACHE = path.join(ROOT, 'build', 'deps-build')

const DEPS_REPO = 'https://github.com/joeedh/sculptcore-deps.git'
const OPENBLAS_REPO = 'https://github.com/OpenMathLib/OpenBLAS.git'
const SUITESPARSE_REPO = 'https://github.com/DrTimothyAldenDavis/SuiteSparse.git'

export const OPENBLAS_TAG = fs.readFileSync(path.join(ROOT, 'openblasVersion.txt'), 'utf-8').trim()
export const SUITESPARSE_TAG = fs.readFileSync(path.join(ROOT, 'suitesparseVersion.txt'), 'utf-8').trim()

// Bump when the build flags below change: combos whose manifest records an
// older revision are rebuilt (same-tag artifacts would otherwise be reused).
// rev 2: threaded OpenBLAS (USE_THREAD+USE_OPENMP, NUM_THREADS=16 cap).
export const DEPS_REVISION = 2

// CHOLMOD + the packages it depends on.
const SUITESPARSE_PROJECTS = 'suitesparse_config;amd;colamd;camd;ccolamd;cholmod'

function sh(cmd, opts = {}) {
  child_process.execSync(cmd, {stdio: 'inherit', shell: true, ...opts})
}

// Run a command with the native build environment applied (vcvars on Windows).
function shEnv(cmd, opts = {}) {
  sh(`node "${CONFIG_ENV}" ${cmd}`, opts)
}

function capture(cmd) {
  return child_process.execSync(cmd, {encoding: 'utf-8', shell: true}).trim()
}

// Capture a command's output with the native build environment applied. clang is
// only on PATH under vcvars, so combo-identity probes must go through this (a
// bare `clang --version` fails outside configureEnv and would mislabel the combo).
function captureEnv(cmd) {
  return capture(`node "${CONFIG_ENV}" ${cmd}`)
}

// --- combo identity ----------------------------------------------------------

function platformDir() {
  switch (process.platform) {
    case 'win32':
      return 'windows'
    case 'darwin':
      return 'macos'
    default:
      return 'linux'
  }
}

function clangMajor() {
  const out = captureEnv('clang --version')
  const m = out.match(/clang version (\d+)/i)
  if (!m) {
    throw new Error('deps: could not determine clang major version (clang --version under configureEnv)')
  }
  return m[1]
}

export function toolchainKey() {
  return `clang-${clangMajor()}-${process.arch}`
}

// config (lowercase folder name) -> CMAKE_BUILD_TYPE
const CONFIGS = {
  release       : 'Release',
  relwithdebinfo: 'RelWithDebInfo',
  debug         : 'Debug',
  asan          : 'Debug',
}

export function configName(c) {
  const name = (c || 'relwithdebinfo').toLowerCase()
  if (!(name in CONFIGS)) {
    throw new Error(`deps: unknown config "${c}" (expected ${Object.keys(CONFIGS).join('|')})`)
  }
  return name
}

function comboRelPath(config) {
  return path.posix.join(platformDir(), toolchainKey(), config)
}

// --- sparse clone ------------------------------------------------------------

// Static libs (OpenBLAS+LAPACK in particular) exceed GitHub's 50MB warn / 100MB
// hard limit, so the deps repo stores libs via git-lfs. Patterns are kept in the
// repo's root .gitattributes (committed alongside the artifacts).
const LFS_PATTERNS = ['*.a', '*.lib', '*.dll', '*.dylib', '*.so']

function ensureLfs() {
  try {
    sh(`git -C "${DEPS_CLONE}" lfs install --local`, {stdio: 'ignore'})
  } catch {
    console.warn('deps: WARNING git-lfs not available; large libs may exceed GitHub limits')
    return
  }
  const file = path.join(DEPS_CLONE, '.gitattributes')
  const have = fs.existsSync(file) ? fs.readFileSync(file, 'utf-8').split(/\r?\n/) : []
  const want = LFS_PATTERNS.map((p) => `${p} filter=lfs diff=lfs merge=lfs -text`)
  const missing = want.filter((l) => !have.includes(l))
  if (missing.length) {
    fs.writeFileSync(file, [...have.filter(Boolean), ...missing].join('\n') + '\n')
  }
}

function ensureClone() {
  if (fs.existsSync(path.join(DEPS_CLONE, '.git'))) return
  fs.mkdirSync(path.dirname(DEPS_CLONE), {recursive: true})
  console.log(`deps: cloning ${DEPS_REPO} (sparse)`)
  sh(`git clone --no-checkout "${DEPS_REPO}" "${DEPS_CLONE}"`)
  // Cone-mode sparse checkout: start empty, add only the combos we touch.
  sh(`git -C "${DEPS_CLONE}" sparse-checkout init --cone`)
}

// Materialize a combo's subtree (empty in the worktree if the remote lacks it).
function checkoutCombo(comboRel) {
  sh(`git -C "${DEPS_CLONE}" sparse-checkout add "${comboRel}"`)
  // Empty/fresh repos have no branch to check out yet; ignore failure.
  try {
    sh(`git -C "${DEPS_CLONE}" checkout`, {stdio: 'ignore'})
  } catch {
    // no commits yet
  }
}

// --- cmake helpers -----------------------------------------------------------

function cmakeBuildType(config) {
  return CONFIGS[config]
}

function sanitizerFlags(config) {
  return config === 'asan' ? '-fsanitize=address' : ''
}

function gitCloneTag(repo, tag, dest) {
  if (fs.existsSync(path.join(dest, '.git'))) return
  fs.mkdirSync(path.dirname(dest), {recursive: true})
  sh(`git clone --depth 1 --branch ${tag} "${repo}" "${dest}"`)
}

// First match of any glob is returned (libs differ in name/ext per platform).
function findLib(libDir, names) {
  if (!fs.existsSync(libDir)) return null
  const files = fs.readdirSync(libDir)
  for (const base of names) {
    const re = new RegExp(`^(lib)?${base}([._-].*)?\\.(a|lib)$`, 'i')
    const hit = files.find((f) => re.test(f))
    if (hit) return path.join(libDir, hit)
  }
  return null
}

// --- from-source build -------------------------------------------------------

// Set by ensureDeps({jobs}); caps `cmake --build --parallel` for the local
// dep builds. 0/undefined = all cores.
let BUILD_JOBS = 0
function parallelArg() {
  return BUILD_JOBS > 0 ? ` ${BUILD_JOBS}` : ''
}

function buildOpenBLAS(config, installDir) {
  // OpenBLAS's CMake support is flaky and non-deterministic across runs; once the
  // install tree exists, never re-invoke its configure/build — just reuse it.
  const existing = findLib(path.join(installDir, 'lib'), ['openblas'])
  if (existing) {
    console.log(`deps: reusing installed OpenBLAS (${path.basename(existing)})`)
    return existing
  }

  const src = path.join(SRC_CACHE, `OpenBLAS-${OPENBLAS_TAG}`)
  const bdir = path.join(BUILD_CACHE, `openblas-${config}`)

  const applyPatch = !fs.existsSync(src)
  gitCloneTag(OPENBLAS_REPO, OPENBLAS_TAG, src)

  // apply patch
  if (applyPatch) {
    console.log('Applying OpenBLAS patch')
    fs.copyFileSync(
      path.join(process.cwd(), 'patches', 'openblas-configure.diff'),
      path.join(src, 'openblas-configure.diff')
    )
    sh('git apply openblas-configure.diff', {cwd: src})
  }

  const flags = sanitizerFlags(config)
  const args = [
    `-G Ninja`,
    COMPILER_ARGS,
    `-DCMAKE_INSTALL_PREFIX="${installDir}"`,
    `-DCMAKE_BUILD_TYPE=${cmakeBuildType(config)}`,
    `-DNOFORTRAN=ON`, // portable C reference LAPACK; BLAS kernels stay C/asm
    `-DBUILD_WITHOUT_LAPACK=OFF`, // LAPACK enabled (CHOLMOD supernodal needs it)
    `-DBUILD_WITHOUT_CBLAS=ON`, // CHOLMOD uses the Fortran BLAS interface
    `-DBINARY=64`, // 64-bit build; stops getarch misdetecting -m32
    // Runtime CPU dispatch -> portable across the toolchain key. Off on macOS:
    // DYNAMIC_ARCH generates one object set per micro-arch, and archiving them
    // all overflows `ar`'s arg limit there. The combo key already pins arch
    // (darwin/clang-N-arm64), so a host-arch build is fine for CI/nightly.
    process.platform === 'darwin' ? `-DDYNAMIC_ARCH=OFF` : `-DDYNAMIC_ARCH=ON`,
    `-DBUILD_TESTING=OFF`,
    '-DUSE_THREAD=ON',
    '-DUSE_OPENMP=ON', // share CHOLMOD's OpenMP runtime -> one thread-count knob
    '-DNUM_THREADS=16', // compile-time pool cap (default bakes the build host's core count)
    '-DUSE_LOCKING=ON', // thread-safe when the caller drops to 1 BLAS thread
    `-DBUILD_SHARED_LIBS=OFF`,
    `-DBUILD_STATIC_LIBS=ON`,
    flags ? `-DCMAKE_C_FLAGS="${flags}"` : '',
    flags ? `-DCMAKE_CXX_FLAGS="${flags}"` : '',
  ]
    .filter(Boolean)
    .join(' ')

  console.log(`deps: configuring OpenBLAS ${OPENBLAS_TAG} (${config})`)
  shEnv(`cmake ${args} -S "${src}" -B "${bdir}"`)
  console.log('deps: building OpenBLAS (this can take a while on a clean tree)')
  shEnv(`cmake --build "${bdir}" --config ${cmakeBuildType(config)} --parallel${parallelArg()}`)
  shEnv(`cmake --install "${bdir}" --config ${cmakeBuildType(config)}`)

  const lib = findLib(path.join(installDir, 'lib'), ['openblas'])
  if (!lib) throw new Error(`deps: OpenBLAS built but no static lib under ${installDir}/lib`)
  return lib
}

/**
 * OpenMP runtime linker flags for the SuiteSparse BLAS/threading probe.
 *
 * OpenBLAS is built USE_OPENMP=ON, so its static lib pulls in OpenMP runtime
 * symbols; the probe must link them or it fails to compile and SuiteSparse
 * rejects OpenBLAS. clang's runtime is libomp (-lomp, not -llibomp). On macOS
 * Homebrew's libomp isn't on the default linker path, so add -L from
 * OpenMP_ROOT (set by CI / FindOpenMP).
 */
function ompLinkerFlags() {
  const root = process.env.OpenMP_ROOT
  return root ? `-L${root.replace(/\\/g, '/')}/lib -lomp` : '-lomp'
}

function buildSuiteSparse(config, installDir, openblasLib) {
  const existing = findLib(path.join(installDir, 'lib'), ['cholmod'])
  if (existing) {
    console.log(`deps: reusing installed SuiteSparse (${path.basename(existing)})`)
    return existing
  }

  const src = path.join(SRC_CACHE, `SuiteSparse-${SUITESPARSE_TAG}`)
  const bdir = path.join(BUILD_CACHE, `suitesparse-${config}`)
  gitCloneTag(SUITESPARSE_REPO, SUITESPARSE_TAG, src)

  const flags = sanitizerFlags(config)
  // CMake treats backslashes as escapes; a Windows lib path injected into
  // SuiteSparse's generated try_run probe breaks it. Always pass forward slashes.
  const blasLib = openblasLib.replace(/\\/g, '/')
  // SuiteSparse probes/links BLAS+LAPACK at configure; point it at our OpenBLAS
  // (it provides both) so its FindBLAS/FindLAPACK return early.
  const args = [
    `-G Ninja`,
    COMPILER_ARGS,
    `-DCMAKE_INSTALL_PREFIX="${installDir}"`,
    `-DCMAKE_BUILD_TYPE=${cmakeBuildType(config)}`,
    `-DBUILD_SHARED_LIBS=OFF`,
    `-DSUITESPARSE_ENABLE_PROJECTS="${SUITESPARSE_PROJECTS}"`,
    `-DSUITESPARSE_DEMOS=OFF`,
    `-DSUITESPARSE_USE_FORTRAN=OFF`,
    `-DSUITESPARSE_USE_CUDA=OFF`,
    '-DSUITESPARSE_USE_OPENMP=ON',
    `-DBLA_VENDOR=OpenBLAS`,
    `-DBLAS_LIBRARIES="${blasLib}"`,
    `-DBLAS_LINKER_FLAGS="${ompLinkerFlags()}"`,
    `-DLAPACK_LIBRARIES="${blasLib}"`,
    `-DBLAS_FOUND=TRUE`,
    `-DLAPACK_FOUND=TRUE`,
    flags ? `-DCMAKE_C_FLAGS="${flags}"` : '',
    flags ? `-DCMAKE_CXX_FLAGS="${flags}"` : '',
  ]
    .filter(Boolean)
    .join(' ')

  console.log(`deps: configuring SuiteSparse ${SUITESPARSE_TAG} (${config})`)
  shEnv(`cmake ${args} -S "${src}" -B "${bdir}"`)
  console.log('deps: building SuiteSparse / CHOLMOD')
  shEnv(`cmake --build "${bdir}" --config ${cmakeBuildType(config)} --parallel${parallelArg()}`)
  shEnv(`cmake --install "${bdir}" --config ${cmakeBuildType(config)}`)

  const lib = findLib(path.join(installDir, 'lib'), ['cholmod'])
  if (!lib) throw new Error(`deps: SuiteSparse built but no CHOLMOD lib under ${installDir}/lib`)
  return lib
}

function readManifest(comboDir) {
  const file = path.join(comboDir, 'manifest.json')
  if (!fs.existsSync(file)) return null
  try {
    return JSON.parse(fs.readFileSync(file, 'utf-8'))
  } catch {
    return null
  }
}

function manifestMatches(m) {
  return !!m && m.openblas === OPENBLAS_TAG && m.suitesparse === SUITESPARSE_TAG && m.revision === DEPS_REVISION
}

// A stale manifest (pin or revision bump) must clear the install trees and the
// cmake build dirs: the per-lib findLib reuse short-circuit and stale CMake
// caches would otherwise resurrect the old artifacts.
function clearStaleCombo(comboDir, cfg) {
  console.log('deps: manifest stale (pin/revision changed); clearing cached combo')
  for (const sub of ['openblas', 'suitesparse', 'manifest.json']) {
    fs.rmSync(path.join(comboDir, sub), {recursive: true, force: true})
  }
  for (const sub of [`openblas-${cfg}`, `suitesparse-${cfg}`]) {
    fs.rmSync(path.join(BUILD_CACHE, sub), {recursive: true, force: true})
  }
}

function printPushHint(comboRel) {
  const rel = comboRel
  console.log('\ndeps: built locally. To publish to sculptcore-deps, run:')
  console.log('  # .gitattributes routes *.a/*.lib through git-lfs automatically')
  console.log(`  git -C "${DEPS_CLONE}" add .gitattributes "${rel}"`)
  console.log(`  git -C "${DEPS_CLONE}" commit -m "add ${rel}"`)
  console.log(`  git -C "${DEPS_CLONE}" push\n`)
}

// --- entry point -------------------------------------------------------------

// Returns the absolute combo dir (to feed CMake as SCULPTCORE_DEPS_DIR).
export async function ensureDeps({config, jobs} = {}) {
  BUILD_JOBS = Number(jobs) > 0 ? Number(jobs) : 0
  const cfg = configName(config)
  const comboRel = comboRelPath(cfg)
  const comboDir = path.join(DEPS_CLONE, comboRel)

  ensureClone()
  ensureLfs()
  checkoutCombo(comboRel)

  const cached = readManifest(comboDir)
  if (manifestMatches(cached)) {
    console.log(`deps: cache hit ${comboRel} (OpenBLAS ${OPENBLAS_TAG}, SuiteSparse ${SUITESPARSE_TAG})`)
    writeFreshMarker({comboRel, comboDir, fresh: false})
    return comboDir
  }
  if (cached) {
    clearStaleCombo(comboDir, cfg)
  }

  console.log(`deps: building ${comboRel} from source`)
  fs.mkdirSync(comboDir, {recursive: true})
  const openblasLib = buildOpenBLAS(cfg, path.join(comboDir, 'openblas'))
  buildSuiteSparse(cfg, path.join(comboDir, 'suitesparse'), openblasLib)

  const manifest = {
    openblas      : OPENBLAS_TAG,
    suitesparse   : SUITESPARSE_TAG,
    revision      : DEPS_REVISION,
    platform      : platformDir(),
    toolchain     : toolchainKey(),
    config        : cfg,
    cmakeBuildType: cmakeBuildType(cfg),
  }
  fs.writeFileSync(path.join(comboDir, 'manifest.json'), JSON.stringify(manifest, null, 2) + '\n')

  writeFreshMarker({comboRel, comboDir, fresh: true})
  printPushHint(comboRel)
  return comboDir
}

// Record the last-resolved combo and whether it was built from source this run.
// `make.mjs bundle --publish-deps-to` reads this so a farm build only exports
// (and later commits) deps it actually built, never a cache hit.
function writeFreshMarker({comboRel, comboDir, fresh}) {
  const dir = path.join(ROOT, 'build')
  fs.mkdirSync(dir, {recursive: true})
  fs.writeFileSync(path.join(dir, 'deps-last-fresh.json'), JSON.stringify({comboRel, comboDir, fresh}, null, 2) + '\n')
}

// Allow `node tools/deps.mjs [config]` for standalone use.
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  ensureDeps({config: process.argv[2]}).then(
    (dir) => console.log(`deps: ready at ${dir}`),
    (err) => {
      console.error(err.message || err)
      process.exit(1)
    }
  )
}
