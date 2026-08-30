#!/usr/bin/env node
// Publish farm-built native deps back to the sculptcore-deps repo.
//
// A Blender build with WITH_SCULPTCORE that hits a deps cache miss compiles
// OpenBLAS + SuiteSparse/CHOLMOD from source and stages the combo into the
// `sculptcore_deps` install component (see build_files/cmake/sculptcore.cmake).
// Build workers should not hold push credentials, so publishing is a separate,
// trusted, off-farm step: this tool takes the collected component output (a
// directory, or a Blender package .zip it was bundled into), commits any combo
// the sculptcore-deps repo is missing, and can strip the deps back out of a
// package so they never ship.
//
// Usage:
//   node tools/publish-deps-from-package.mjs <path> [options]
//
//   <path>   Directory or .zip containing a `sculptcore-deps/` subtree (as
//            produced by `cmake --install <build> --component sculptcore_deps
//            --prefix <dir>`, or bundled into a Blender package).
//
// Options:
//   --commit        Stage + commit each missing combo into sculptcore-deps.
//   --push          Also `git push` (implies --commit).
//   --strip         Remove the sculptcore-deps/ subtree from <path> afterward.
//   --out <file>    When <path> is a .zip, write the (stripped) result here
//                   instead of overwriting in place.
//   --repo <url>    Override the sculptcore-deps repo URL.
//   --clone <dir>   Working clone location (default: build/deps-publish-clone).
//   --dry-run       Report what would happen; make no repo or filesystem edits.
//
// Without --commit the tool only reports which combos are new vs already
// published (a safe default).

import fs from 'fs'
import os from 'os'
import path from 'path'
import child_process from 'child_process'
import {fileURLToPath} from 'url'

const here = path.dirname(fileURLToPath(import.meta.url))
const ROOT = path.resolve(here, '..') // sculptcore/

const DEFAULT_REPO = 'https://github.com/joeedh/sculptcore-deps.git'
// Kept in sync with tools/deps.mjs LFS_PATTERNS.
const LFS_PATTERNS = ['*.a', '*.lib', '*.dll', '*.dylib', '*.so']

function sh(cmd, opts = {}) {
  child_process.execSync(cmd, {stdio: 'inherit', shell: true, ...opts})
}
function shQuiet(cmd, opts = {}) {
  return child_process.execSync(cmd, {stdio: ['ignore', 'pipe', 'pipe'], shell: true, encoding: 'utf-8', ...opts})
}

function parseArgs(argv) {
  const opts = {commit: false, push: false, strip: false, dryRun: false, repo: DEFAULT_REPO, clone: null, out: null}
  const positional = []
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]
    switch (a) {
      case '--commit':
        opts.commit = true
        break
      case '--push':
        opts.push = true
        opts.commit = true
        break
      case '--strip':
        opts.strip = true
        break
      case '--dry-run':
        opts.dryRun = true
        break
      case '--repo':
        opts.repo = argv[++i]
        break
      case '--clone':
        opts.clone = argv[++i]
        break
      case '--out':
        opts.out = argv[++i]
        break
      default:
        if (a.startsWith('--')) {
          process.stderr.write(`unknown option: ${a}\n`)
          process.exit(2)
        }
        positional.push(a)
    }
  }
  if (positional.length !== 1) {
    process.stderr.write('usage: node tools/publish-deps-from-package.mjs <path> [options]\n')
    process.exit(2)
  }
  opts.input = positional[0]
  opts.clone = opts.clone || path.join(ROOT, 'build', 'deps-publish-clone')
  return opts
}

// Extract a .zip using bsdtar (`tar`), which reads zip on Windows 10+/macOS and
// most modern Linux. Returns the extraction dir.
function extractZip(zip, dest) {
  fs.mkdirSync(dest, {recursive: true})
  sh(`tar -xf "${zip}" -C "${dest.replace(/\\/g, '/')}"`)
  return dest
}

// Repackage a directory tree into a .zip via bsdtar (auto-format from the .zip
// extension). Overwrites `out`.
function makeZip(srcDir, out) {
  fs.rmSync(out, {force: true})
  // -a: derive the format from the file extension; -C: pack tree contents.
  sh(`tar -a -c -f "${out}" -C "${srcDir.replace(/\\/g, '/')}" .`)
}

// Locate the deps root: a directory named `sculptcore-deps`, else `<root>` if it
// already looks like the combos root (contains a platform dir with a manifest).
function findDepsRoot(root) {
  const named = path.join(root, 'sculptcore-deps')
  if (fs.existsSync(named) && fs.statSync(named).isDirectory()) {
    return named
  }
  return root
}

// Every combo is a directory holding a manifest.json. Returns combos as
// {rel, dir, manifest} where `rel` is the path under the deps root
// (`<platform>/<toolchain>/<config>`).
function findCombos(depsRoot) {
  const combos = []
  const walk = (dir) => {
    if (fs.existsSync(path.join(dir, 'manifest.json'))) {
      const rel = path.relative(depsRoot, dir).split(path.sep).join('/')
      let manifest = null
      try {
        manifest = JSON.parse(fs.readFileSync(path.join(dir, 'manifest.json'), 'utf-8'))
      } catch {
        /* leave manifest null; treated as unpublishable below */
      }
      combos.push({rel, dir, manifest})
      return // manifests don't nest
    }
    for (const entry of fs.readdirSync(dir, {withFileTypes: true})) {
      if (entry.isDirectory()) {
        walk(path.join(dir, entry.name))
      }
    }
  }
  if (fs.existsSync(depsRoot)) {
    walk(depsRoot)
  }
  return combos
}

function manifestsEqual(a, b) {
  return !!a && !!b && a.openblas === b.openblas && a.suitesparse === b.suitesparse && a.revision === b.revision
}

function copyTree(src, dst) {
  fs.mkdirSync(dst, {recursive: true})
  for (const entry of fs.readdirSync(src, {withFileTypes: true})) {
    const s = path.join(src, entry.name)
    const d = path.join(dst, entry.name)
    if (entry.isDirectory()) {
      copyTree(s, d)
    } else {
      fs.copyFileSync(s, d)
    }
  }
}

function ensureClone(repo, clone) {
  if (fs.existsSync(path.join(clone, '.git'))) {
    return
  }
  fs.mkdirSync(path.dirname(clone), {recursive: true})
  console.log(`publish-deps: cloning ${repo} (sparse) -> ${clone}`)
  sh(`git clone --no-checkout "${repo}" "${clone}"`)
  sh(`git -C "${clone}" sparse-checkout init --cone`)
}

function ensureLfs(clone) {
  try {
    sh(`git -C "${clone}" lfs install --local`, {stdio: 'ignore'})
  } catch {
    console.warn('publish-deps: WARNING git-lfs not available; large libs may exceed GitHub limits')
    return
  }
  const file = path.join(clone, '.gitattributes')
  const have = fs.existsSync(file) ? fs.readFileSync(file, 'utf-8').split(/\r?\n/) : []
  const want = LFS_PATTERNS.map((p) => `${p} filter=lfs diff=lfs merge=lfs -text`)
  const missing = want.filter((l) => !have.includes(l))
  if (missing.length) {
    fs.writeFileSync(file, [...have.filter(Boolean), ...missing].join('\n') + '\n')
  }
}

function checkoutCombo(clone, rel) {
  sh(`git -C "${clone}" sparse-checkout add "${rel}"`)
  try {
    sh(`git -C "${clone}" checkout`, {stdio: 'ignore'})
  } catch {
    /* empty repo: no branch to check out yet */
  }
}

function readCloneManifest(clone, rel) {
  const file = path.join(clone, rel, 'manifest.json')
  if (!fs.existsSync(file)) {
    return null
  }
  try {
    return JSON.parse(fs.readFileSync(file, 'utf-8'))
  } catch {
    return null
  }
}

function publishCombo(opts, combo) {
  const {clone} = opts
  checkoutCombo(clone, combo.rel)
  const existing = readCloneManifest(clone, combo.rel)
  if (manifestsEqual(existing, combo.manifest)) {
    console.log(`publish-deps: ${combo.rel} already published (manifest matches); skipping`)
    return false
  }
  if (!combo.manifest) {
    console.warn(`publish-deps: ${combo.rel} has no readable manifest; skipping`)
    return false
  }
  console.log(`publish-deps: ${combo.rel} is ${existing ? 'out of date' : 'new'}`)
  if (opts.dryRun) {
    console.log(`publish-deps: [dry-run] would copy + commit ${combo.rel}`)
    return true
  }

  const dst = path.join(clone, combo.rel)
  fs.rmSync(dst, {recursive: true, force: true})
  copyTree(combo.dir, dst)

  if (!opts.commit) {
    console.log(`publish-deps: staged ${combo.rel} into the clone (pass --commit to commit)`)
    return true
  }

  ensureLfs(clone)
  sh(`git -C "${clone}" add .gitattributes "${combo.rel}"`)
  // Nothing to commit if the worktree matched despite a manifest diff (rare).
  const status = shQuiet(`git -C "${clone}" status --porcelain`)
  if (!status.trim()) {
    console.log(`publish-deps: ${combo.rel} produced no changes to commit`)
    return false
  }
  sh(`git -C "${clone}" commit -m "add ${combo.rel}"`)
  if (opts.push) {
    sh(`git -C "${clone}" push`)
    console.log(`publish-deps: pushed ${combo.rel}`)
  } else {
    console.log(`publish-deps: committed ${combo.rel} (pass --push to push)`)
  }
  return true
}

function main() {
  const opts = parseArgs(process.argv.slice(2))

  const isZip = opts.input.toLowerCase().endsWith('.zip')
  let workRoot = opts.input
  let tmp = null
  if (isZip) {
    tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'scdeps-'))
    console.log(`publish-deps: extracting ${opts.input}`)
    workRoot = extractZip(opts.input, tmp)
  }

  try {
    const depsRoot = findDepsRoot(workRoot)
    const combos = findCombos(depsRoot)
    if (combos.length === 0) {
      console.log(`publish-deps: no deps combos found under ${depsRoot} (nothing was built from source); done`)
      return
    }
    console.log(`publish-deps: found ${combos.length} combo(s): ${combos.map((c) => c.rel).join(', ')}`)

    ensureClone(opts.repo, opts.clone)
    let published = 0
    for (const combo of combos) {
      if (publishCombo(opts, combo)) {
        published++
      }
    }
    console.log(`publish-deps: ${published} combo(s) new/updated, ${combos.length - published} already published`)

    if (opts.strip) {
      const stripDir = path.join(workRoot, 'sculptcore-deps')
      if (opts.dryRun) {
        console.log(`publish-deps: [dry-run] would strip ${stripDir}`)
      } else if (fs.existsSync(stripDir)) {
        fs.rmSync(stripDir, {recursive: true, force: true})
        console.log(`publish-deps: stripped ${stripDir}`)
      }
      if (isZip && !opts.dryRun) {
        const out = opts.out || opts.input
        makeZip(workRoot, out)
        console.log(`publish-deps: repacked -> ${out}`)
      }
    }
  } finally {
    if (tmp) {
      fs.rmSync(tmp, {recursive: true, force: true})
    }
  }
}

main()
