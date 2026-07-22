#!/usr/bin/env node
// Fetch CI-built native deps into the sparse clone extern/sculptcore-deps.
//
// The native-nightly workflow uploads `deps-<os>-<config>` artifacts, each
// containing a freshly-built combo laid out as <platform>/<toolchain>/<config>/
// {openblas,suitesparse,manifest.json} (from `bundle --publish-deps-to`). This
// helper downloads them (via the gh CLI) and merges each artifact's tree into
// extern/sculptcore-deps, so a subsequent native build gets a cache hit instead
// of rebuilding OpenBLAS/SuiteSparse from source.
//
// Usage:
//   node tools/fetch-deps.mjs [--run <id>] [--branch master] [--pattern 'deps-*']
//
// Requires the `gh` CLI authenticated with read access to this repo. A combo is
// only reusable when its toolchain key ({platform}/clang-<major>-<arch>/{config})
// matches the machine that will consume it — mismatched combos are still copied
// but simply won't be picked up by ensureDeps().
import fs from 'fs'
import path from 'path'
import os from 'os'
import child_process from 'child_process'
import {fileURLToPath} from 'url'

const here = path.dirname(fileURLToPath(import.meta.url))
const ROOT = path.resolve(here, '..') // sculptcore/
const DEPS_CLONE = path.join(ROOT, 'extern', 'sculptcore-deps')
const WORKFLOW = 'native-nightly.yml'

function parseArgs(argv) {
  const out = {branch: 'master', pattern: 'deps-*', run: null}
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]
    if (a === '--run') out.run = argv[++i]
    else if (a === '--branch') out.branch = argv[++i]
    else if (a === '--pattern') out.pattern = argv[++i]
    else {
      console.error(`fetch-deps: unknown arg "${a}"`)
      process.exit(2)
    }
  }
  return out
}

function sh(cmd, opts = {}) {
  child_process.execSync(cmd, {stdio: 'inherit', shell: true, ...opts})
}
function capture(cmd) {
  return child_process.execSync(cmd, {encoding: 'utf-8', shell: true}).trim()
}

function requireGh() {
  try {
    capture('gh --version')
  } catch {
    console.error('fetch-deps: the GitHub CLI (gh) is required and must be authenticated')
    process.exit(1)
  }
}

function latestRunId(branch) {
  const json = capture(
    `gh run list --workflow ${WORKFLOW} --branch ${branch} --status success ` +
      `--limit 1 --json databaseId`
  )
  const arr = JSON.parse(json)
  if (!arr.length) {
    console.error(`fetch-deps: no successful ${WORKFLOW} run on branch ${branch}`)
    process.exit(1)
  }
  return String(arr[0].databaseId)
}

// Recursively copy src tree onto dst (merge; overwrite files).
function copyTree(src, dst) {
  for (const entry of fs.readdirSync(src, {withFileTypes: true})) {
    const s = path.join(src, entry.name)
    const d = path.join(dst, entry.name)
    if (entry.isDirectory()) {
      fs.mkdirSync(d, {recursive: true})
      copyTree(s, d)
    } else {
      fs.mkdirSync(path.dirname(d), {recursive: true})
      fs.copyFileSync(s, d)
    }
  }
}

function main() {
  const {run, branch, pattern} = parseArgs(process.argv.slice(2))
  requireGh()

  const runId = run || latestRunId(branch)
  console.log(`fetch-deps: downloading "${pattern}" artifacts from run ${runId}`)

  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'sculptcore-deps-'))
  try {
    sh(`gh run download ${runId} --pattern "${pattern}" --dir "${tmp}"`)

    // gh drops each artifact into <tmp>/<artifact-name>/... ; the useful payload
    // is the <platform>/<toolchain>/<config>/ subtree inside each. Merge them
    // all into the sparse clone.
    fs.mkdirSync(DEPS_CLONE, {recursive: true})
    const artifacts = fs.existsSync(tmp)
      ? fs.readdirSync(tmp, {withFileTypes: true}).filter((e) => e.isDirectory())
      : []
    if (!artifacts.length) {
      console.log('fetch-deps: no deps artifacts found (all combos were cache hits this run?)')
      return
    }
    for (const a of artifacts) {
      const src = path.join(tmp, a.name)
      console.log(`fetch-deps: merging ${a.name} -> ${DEPS_CLONE}`)
      copyTree(src, DEPS_CLONE)
    }
    console.log('fetch-deps: done. A native configure should now cache-hit any matching combo.')
  } finally {
    fs.rmSync(tmp, {recursive: true, force: true})
  }
}

main()
