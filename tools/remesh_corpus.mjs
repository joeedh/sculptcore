#!/usr/bin/env node
// Quad-remesh corpus batch runner (Tier 0c, plans/quad-remeshing-filtering.md).
//
// Runs remesh_cli over every available asset in tests/corpus/corpus.json and
// aggregates their manifests into a metrics table so each tier's review gate has
// a baseline to diff against. Two artifacts per run:
//   metrics.csv  — one row per asset, the DETERMINISTIC quality columns only
//                  (no timestamps / paths / durations); fixed seed -> identical
//                  file, so this is the thing to diff run-over-run.
//   results.json — the full per-asset manifest (incl. durations + the min-angle
//                  histogram) for deep inspection.
//
// Assets that can't be resolved (the `available:false` placeholders, or a path
// that isn't checked in) are SKIPPED with a logged reason — never silently
// dropped, so the table never reads as "covered everything" when it didn't.
//
// Usage:
//   node tools/remesh_corpus.mjs [--corpus <json>] [--cli <exe>] [--out <dir>]
//                                [--seed <uint>] [--list] [--only name,name]
//                                [--triage] [--triage-weld-rel <f>]
//                                [--triage-min-component-frac <f>]
//
// Triage defaults ON in the engine (review gate 1), so a plain run already
// triages. The --triage* flags force/tune it explicitly on EVERY asset
// (per-asset corpus.json `params` still override); pass `--triage-weld-rel` etc.
// to sweep tolerances, or a per-asset `params.triage=0` to A/B a single asset.
import fs from 'fs'
import path from 'path'
import child_process from 'child_process'
import {fileURLToPath} from 'url'

const here = path.dirname(fileURLToPath(import.meta.url))
const ROOT = path.resolve(here, '..') // sculptcore/

const DEFAULTS = {
  corpus: path.join(ROOT, 'tests', 'corpus', 'corpus.json'),
  cli: path.join(ROOT, 'build', 'native', 'source', 'remesh', 'cli',
                 process.platform === 'win32' ? 'remesh_cli.exe' : 'remesh_cli'),
  assetsDir: path.join(ROOT, 'tests', 'assets'),
  out: path.join(ROOT, 'tests', 'remesher-results', 'corpus'),
  seed: null, // null => take from corpus.json
  triage: false, // apply Tier-1 triage to every asset
  triageWeldRel: null, // null => CLI default
  triageMinComponentFrac: null,
  curvatureSmoothIters: null, // Tier-2a: null => CLI default (0, no smoothing)
  curvatureSmoothLambda: null,
  autoDensity: null, // Tier-3a: null => CLI default (off)
  densityMin: null,
  densityMax: null,
  densityGradation: null, // Tier-3b: null => CLI default (0, off)
  densityGradationIters: null,
}

function parseArgs(argv) {
  const a = {...DEFAULTS, list: false, only: null}
  for (let i = 2; i < argv.length; i++) {
    const k = argv[i]
    const next = () => {
      if (i + 1 >= argv.length) { console.error(`missing value for ${k}`); process.exit(2) }
      return argv[++i]
    }
    if (k === '--corpus') a.corpus = path.resolve(next())
    else if (k === '--cli') a.cli = path.resolve(next())
    else if (k === '--out') a.out = path.resolve(next())
    else if (k === '--assets') a.assetsDir = path.resolve(next())
    else if (k === '--seed') a.seed = parseInt(next(), 10)
    else if (k === '--only') a.only = next().split(',').map(s => s.trim()).filter(Boolean)
    else if (k === '--triage') a.triage = true
    else if (k === '--triage-weld-rel') a.triageWeldRel = parseFloat(next())
    else if (k === '--triage-min-component-frac') a.triageMinComponentFrac = parseFloat(next())
    else if (k === '--curvature-smooth-iters') a.curvatureSmoothIters = parseInt(next(), 10)
    else if (k === '--curvature-smooth-lambda') a.curvatureSmoothLambda = parseFloat(next())
    else if (k === '--auto-density') a.autoDensity = next() !== '0'
    else if (k === '--density-min') a.densityMin = parseFloat(next())
    else if (k === '--density-max') a.densityMax = parseFloat(next())
    else if (k === '--density-gradation') a.densityGradation = parseFloat(next())
    else if (k === '--density-gradation-iters') a.densityGradationIters = parseInt(next(), 10)
    else if (k === '--list') a.list = true
    else if (k === '--help' || k === '-h') { usage(); process.exit(0) }
    else { console.error(`unknown arg ${k}`); usage(); process.exit(2) }
  }
  return a
}

function usage() {
  console.log(`remesh_corpus.mjs — batch-run remesh_cli over the corpus
  --corpus <json>   corpus list (default tests/corpus/corpus.json)
  --cli <exe>       remesh_cli binary (default build/native/.../remesh_cli)
  --out <dir>       output dir for metrics.csv + results.json
  --assets <dir>    base dir for relative asset names (default tests/assets)
  --seed <uint>     override the corpus-wide determinism seed
  --only a,b,c      run only the named assets
  --triage          apply Tier-1 input triage to every asset
  --triage-weld-rel <f>            triage weld tol (frac of bbox diag)
  --triage-min-component-frac <f>  triage drop-component threshold
  --curvature-smooth-iters <int>   Tier-2a curvature tensor Jacobi sweeps
  --curvature-smooth-lambda <f>    Tier-2a per-sweep blend 0..1
  --auto-density <0|1>             Tier-3a curvature-driven sizing field
  --density-min <f>                Tier-3a density clamp floor
  --density-max <f>                Tier-3a density clamp ceiling
  --density-gradation <f>          Tier-3b bound size growth rate (0=off)
  --density-gradation-iters <int>  Tier-3b limiter sweep cap
  --list            list the corpus and resolution status, then exit`)
}

// Resolve an asset reference to an existing absolute path, or null.
function resolveAsset(asset, assetsDir) {
  if (path.isAbsolute(asset) && fs.existsSync(asset)) return asset
  if (fs.existsSync(asset)) return path.resolve(asset)
  const inAssets = path.join(assetsDir, asset)
  if (fs.existsSync(inAssets)) return inAssets
  return null
}

// The deterministic columns (the diff-stable table). Drawn from the manifest's
// input/output/validation/run blocks; volatile fields (timestamp, paths,
// durations) are intentionally excluded so a fixed seed reproduces the file.
const COLUMNS = [
  ['name', m => m.__name],
  ['category', m => m.__category],
  ['success', m => m.run.success],
  ['failure_reason', m => m.run.failure_reason],
  ['in_verts', m => m.input.verts],
  ['in_faces', m => m.input.faces],
  ['in_components', m => m.input.components],
  ['in_holes', m => m.input.holes],
  ['in_manifold', m => m.input.manifold],
  ['out_verts', m => m.output.verts],
  ['out_faces', m => m.output.faces],
  ['out_quads', m => m.output.quads],
  ['out_tris', m => m.output.tris],
  ['out_ngons', m => m.output.ngons],
  ['all_quad', m => m.validation.all_quad],
  ['manifold', m => m.validation.manifold],
  ['euler', m => m.validation.euler],
  ['consistent_winding', m => m.validation.consistent_winding],
  ['non_manifold_edges', m => m.validation.non_manifold_edges],
  ['boundary_edges', m => m.validation.boundary_edges],
  ['degenerate_faces', m => m.validation.degenerate_faces],
  ['inverted_faces', m => m.validation.inverted_faces],
  ['irregular_interior', m => m.validation.irregular_interior_verts],
  ['interior_verts', m => m.validation.interior_vert_count],
  ['regular_frac', m => m.validation.regular_interior_frac],
  ['components', m => m.validation.component_count],
  ['holes', m => m.validation.boundary_loop_count],
  ['max_comp_irregular', m => m.validation.max_component_irregular],
  ['max_area_ratio', m => m.validation.max_adjacent_area_ratio],
  ['max_edge_ratio', m => m.validation.max_adjacent_edge_ratio],
  ['min_angle', m => m.validation.min_interior_angle],
  ['param_folds', m => m.validation.parametrization_folds],
  ['num_singularities', m => m.run.num_singularities],
  ['index_sum', m => m.run.index_sum],
  ['field_eigen', m => m.run.field_solved_eigen],
  ['quantize_feasible', m => m.run.quantize_feasible],
  ['spiral_isolines', m => m.validation.spiral_isolines],
  ['open_isolines', m => m.validation.open_isolines],
  ['closed_isolines', m => m.validation.closed_isolines],
  // Tier-1 triage counts (manifest "triage" block; absent on pre-Tier-1 runs).
  ['triage_ran', m => m.triage?.ran ?? false],
  ['triage_welded', m => m.triage?.welded_verts ?? 0],
  ['triage_degenerate', m => m.triage?.removed_degenerate_faces ?? 0],
  ['triage_dup_faces', m => m.triage?.removed_duplicate_faces ?? 0],
  ['triage_wire_edges', m => m.triage?.removed_wire_edges ?? 0],
  ['triage_components', m => m.triage?.removed_components ?? 0],
  ['triage_comp_verts', m => m.triage?.removed_component_verts ?? 0],
  ['triage_nm_edges', m => m.triage?.non_manifold_edges ?? 0],
  ['triage_nm_verts', m => m.triage?.non_manifold_verts ?? 0],
]

function csvCell(v) {
  if (typeof v === 'boolean') return v ? '1' : '0'
  if (typeof v === 'number') return Number.isInteger(v) ? String(v) : v.toPrecision(6)
  const s = String(v ?? '')
  return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s
}

function runOne(cli, asset, name, outdir, seed, params, globalParams) {
  const args = ['--input', asset, '--name', name, '--outdir', outdir,
                '--seed', String(seed)]
  // Global flags first, per-asset params second — the CLI is last-wins, so a
  // corpus.json `params` entry overrides a global --triage* flag.
  for (const [k, v] of Object.entries({...globalParams, ...(params || {})})) {
    if (k === 'seed') continue // seed is the runner's job
    args.push(`--${k}`, String(v))
  }
  const res = child_process.spawnSync(cli, args, {encoding: 'utf-8'})
  const stdout = res.stdout || ''
  let manifestPath = null
  for (const line of stdout.split(/\r?\n/)) {
    if (line.startsWith('MANIFEST ')) manifestPath = line.slice('MANIFEST '.length).trim()
  }
  return {code: res.status, stdout, stderr: res.stderr || '', manifestPath}
}

function main() {
  const args = parseArgs(process.argv)
  if (!fs.existsSync(args.corpus)) {
    console.error(`corpus not found: ${args.corpus}`)
    process.exit(1)
  }
  const corpus = JSON.parse(fs.readFileSync(args.corpus, 'utf-8'))
  const seed = args.seed != null ? args.seed : (corpus.seed ?? 1)
  let assets = corpus.assets || []
  if (args.only) assets = assets.filter(a => args.only.includes(a.name))

  // Deterministic order: sort by name so the table is stable regardless of JSON
  // key order or --only ordering.
  assets = assets.slice().sort((a, b) => a.name < b.name ? -1 : a.name > b.name ? 1 : 0)

  if (args.list) {
    for (const a of assets) {
      const resolved = a.available === false ? null : resolveAsset(a.asset, args.assetsDir)
      const status = a.available === false ? 'placeholder'
                   : resolved ? 'ready' : 'MISSING'
      console.log(`${a.name.padEnd(18)} ${String(a.category).padEnd(20)} ${status}\t${a.asset}`)
    }
    return
  }

  if (!fs.existsSync(args.cli)) {
    console.error(`remesh_cli not found: ${args.cli}\n  build it: node make.mjs build native`)
    process.exit(1)
  }
  fs.mkdirSync(args.out, {recursive: true})

  // Triage flags applied to every asset (keys are CLI flag names; --triage takes
  // a 0|1 value). Per-asset corpus.json `params` override these (last-wins).
  const globalParams = {}
  if (args.triage) globalParams['triage'] = 1
  if (args.triageWeldRel != null) globalParams['triage-weld-rel'] = args.triageWeldRel
  if (args.triageMinComponentFrac != null)
    globalParams['triage-min-component-frac'] = args.triageMinComponentFrac
  if (args.curvatureSmoothIters != null)
    globalParams['curvature-smooth-iters'] = args.curvatureSmoothIters
  if (args.curvatureSmoothLambda != null)
    globalParams['curvature-smooth-lambda'] = args.curvatureSmoothLambda
  if (args.autoDensity != null)
    globalParams['auto-density'] = args.autoDensity ? 1 : 0
  if (args.densityMin != null) globalParams['density-min'] = args.densityMin
  if (args.densityMax != null) globalParams['density-max'] = args.densityMax
  if (args.densityGradation != null)
    globalParams['density-gradation'] = args.densityGradation
  if (args.densityGradationIters != null)
    globalParams['density-gradation-iters'] = args.densityGradationIters

  const results = []
  const skipped = []
  for (const a of assets) {
    if (a.available === false) {
      skipped.push({name: a.name, reason: 'placeholder (available:false)'})
      console.log(`SKIP ${a.name} — placeholder (available:false)`)
      continue
    }
    const resolved = resolveAsset(a.asset, args.assetsDir)
    if (!resolved) {
      skipped.push({name: a.name, reason: `asset not found: ${a.asset}`})
      console.log(`SKIP ${a.name} — asset not found: ${a.asset}`)
      continue
    }
    console.log(`RUN  ${a.name}  (${a.asset}, seed=${seed})`)
    const r = runOne(args.cli, resolved, a.name, args.out, seed, a.params, globalParams)
    if (!r.manifestPath || !fs.existsSync(r.manifestPath)) {
      skipped.push({name: a.name, reason: `no manifest (exit ${r.code})`})
      console.log(`FAIL ${a.name} — no manifest emitted (exit ${r.code})`)
      if (r.stderr.trim()) console.log(`     ${r.stderr.trim().split(/\r?\n/).slice(-1)[0]}`)
      continue
    }
    const manifest = JSON.parse(fs.readFileSync(r.manifestPath, 'utf-8'))
    manifest.__name = a.name
    manifest.__category = a.category || ''
    results.push(manifest)
    const v = manifest.validation, run = manifest.run
    console.log(`  -> ${run.success ? 'ok' : 'FAILED:' + run.failure_reason}` +
                ` quads=${manifest.output.quads} manifold=${v.manifold ? 1 : 0}` +
                ` regular=${(v.regular_interior_frac * 100).toFixed(1)}%` +
                ` folds=${v.parametrization_folds} holes=${v.boundary_loop_count}`)
  }

  // Stable order in the table too (results already came in sorted order).
  const header = COLUMNS.map(c => c[0]).join(',')
  const rows = results.map(m => COLUMNS.map(([, fn]) => csvCell(fn(m))).join(','))
  const csv = [header, ...rows].join('\n') + '\n'
  const csvPath = path.join(args.out, 'metrics.csv')
  fs.writeFileSync(csvPath, csv)

  const jsonOut = {
    schema: 'remesh-corpus-results/1',
    seed,
    corpus: path.relative(ROOT, args.corpus).replace(/\\/g, '/'),
    git_commit: results[0]?.git_commit ?? null,
    skipped,
    results,
  }
  const jsonPath = path.join(args.out, 'results.json')
  fs.writeFileSync(jsonPath, JSON.stringify(jsonOut, null, 2) + '\n')

  console.log(`\n${results.length} asset(s) -> ${csvPath}`)
  console.log(`              -> ${jsonPath}`)
  if (skipped.length) console.log(`${skipped.length} skipped (see results.json "skipped")`)
}

main()
