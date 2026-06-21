#!/usr/bin/env node
/**
 * Crash-dump toolkit for the NW.js app — the "easy way" to analyze and
 * manipulate the Crashpad minidumps written into <repo>/build/crashdumps.
 *
 * One dependency-free CLI wrapping the Windows SDK debugger (cdb.exe) plus the
 * local build artifacts (sculptcore_node.node + its CodeView PDB). See
 * documentation/plans/crashpad.md.
 *
 *   node sculptcore/crash/dump.mjs <command> [dump] [options]
 *
 * Dumps are read from NW.js's Crashpad dir (default
 * %LOCALAPPDATA%\<manifest>\User Data\Crashpad\reports, override $SC_CRASHDUMP_DIR);
 * our PDB symbol store + packaged zips live in <repo>/build/crashdumps.
 *
 * Commands (omit [dump] to use the newest *.dmp in the Crashpad dir):
 *   list                      table of dumps: mtime, size, faulting top frame
 *   walk   [dump]             symbolicated stack of the faulting thread (default)
 *   info   [dump]             exception record, registers, module + PDB-match
 *   threads[dump]             all thread stacks (~*kn)
 *   open   [dump]             open interactively in WinDbg/cdb (syms pre-wired)
 *   eval   [dump] "<cmds>"    run arbitrary cdb command(s) against the dump
 *   package[dump]             zip dump + matching PDB + build-info.json
 *   symcheck[dump]            verify the on-disk PDB matches (exit!=0 on miss)
 *   prune  [--keep N]         delete all but the newest N dumps (+ their .zip)
 *
 * Options: --json (structured output for read cmds), --public-syms (add the
 * Microsoft symbol server to the path for system DLLs).
 */
import {spawnSync} from 'node:child_process'
import {fileURLToPath} from 'node:url'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

const here = path.dirname(fileURLToPath(import.meta.url)) // sculptcore/crash
export const repoRoot = path.resolve(here, '..', '..')

// Repo-local store for OUR artifacts (the content-addressed PDB symbol store +
// packaged crash zips). Gitignored (build/ is). NOT where NW.js writes dumps.
export const artifactDir = path.join(repoRoot, 'build', 'crashdumps')
export const symStore = path.join(artifactDir, 'syms')

/** The app's manifest name (the Crashpad dir is keyed on it). */
function manifestName() {
  try {
    return JSON.parse(fs.readFileSync(path.join(repoRoot, 'package.json'), 'utf8')).name || 'webgl-app-framework'
  } catch {
    return 'webgl-app-framework'
  }
}

/**
 * Where NW.js Crashpad actually writes renderer minidumps. NW.js 0.112 has no
 * setCrashDumpDir, so this is the default Chromium location keyed on the
 * manifest name. Override with $SC_CRASHDUMP_DIR.
 */
function resolveCrashpadDir() {
  if (process.env.SC_CRASHDUMP_DIR) return process.env.SC_CRASHDUMP_DIR
  if (process.env.LOCALAPPDATA) {
    return path.join(process.env.LOCALAPPDATA, manifestName(), 'User Data', 'Crashpad', 'reports')
  }
  return artifactDir
}

export const dumpDir = resolveCrashpadDir()
export const nodeAddonDir = path.join(repoRoot, 'sculptcore', 'build', 'native-node')
export const addonNode = path.join(nodeAddonDir, 'sculptcore_node.node')
export const addonPdb = path.join(nodeAddonDir, 'sculptcore_node.pdb')

const ADDON_MODULE = 'sculptcore_node'

// ---------------------------------------------------------------------------
// PE / CodeView (RSDS) parsing — extract a module's PDB GUID+age so we can
// content-address the PDB in a symstore-style downstream store. Used here and
// by sculptcore/make.mjs (archivePdb) after a node build.
// ---------------------------------------------------------------------------

/** Parse a PE file's CodeView debug record → {guid:Buffer, age, pdbPath, key}. */
export function readPeDebugInfo(file) {
  const buf = fs.readFileSync(file)
  if (buf.readUInt16LE(0) !== 0x5a4d) return null // 'MZ'
  const peOff = buf.readUInt32LE(0x3c)
  if (buf.readUInt32LE(peOff) !== 0x00004550) return null // 'PE\0\0'
  const coff = peOff + 4
  const numSections = buf.readUInt16LE(coff + 2)
  const optSize = buf.readUInt16LE(coff + 16)
  const opt = coff + 20
  const magic = buf.readUInt16LE(opt)
  const dirStart = magic === 0x20b ? opt + 112 : opt + 96 // PE32+ vs PE32
  const debugRva = buf.readUInt32LE(dirStart + 6 * 8)
  const debugSize = buf.readUInt32LE(dirStart + 6 * 8 + 4)
  if (!debugRva || !debugSize) return null

  // Section table → RVA-to-file-offset map.
  const secStart = opt + optSize
  const sections = []
  for (let i = 0; i < numSections; i++) {
    const s = secStart + i * 40
    sections.push({
      va: buf.readUInt32LE(s + 12),
      vsize: buf.readUInt32LE(s + 8),
      raw: buf.readUInt32LE(s + 20),
      rawSize: buf.readUInt32LE(s + 16),
    })
  }
  const rvaToOff = (rva) => {
    for (const s of sections) {
      const span = Math.max(s.vsize, s.rawSize)
      if (rva >= s.va && rva < s.va + span) return rva - s.va + s.raw
    }
    return -1
  }

  const debugOff = rvaToOff(debugRva)
  if (debugOff < 0) return null
  for (let off = debugOff; off < debugOff + debugSize; off += 28) {
    const type = buf.readUInt32LE(off + 12)
    if (type !== 2) continue // IMAGE_DEBUG_TYPE_CODEVIEW
    const cvOff = buf.readUInt32LE(off + 24) // PointerToRawData
    if (buf.toString('ascii', cvOff, cvOff + 4) !== 'RSDS') continue
    const guid = buf.subarray(cvOff + 4, cvOff + 20)
    const age = buf.readUInt32LE(cvOff + 20)
    let end = cvOff + 24
    while (end < buf.length && buf[end] !== 0) end++
    const pdbPath = buf.toString('utf8', cvOff + 24, end)
    return {guid, age, pdbPath, key: guidToSymKey(guid, age)}
  }
  return null
}

/** Format a CodeView GUID+age as the symstore directory key (32 hex + age). */
export function guidToSymKey(guid, age) {
  const d1 = guid.readUInt32LE(0).toString(16).padStart(8, '0')
  const d2 = guid.readUInt16LE(4).toString(16).padStart(4, '0')
  const d3 = guid.readUInt16LE(6).toString(16).padStart(4, '0')
  let d4 = ''
  for (let i = 8; i < 16; i++) d4 += guid[i].toString(16).padStart(2, '0')
  return (d1 + d2 + d3 + d4 + age.toString(16)).toUpperCase()
}

/**
 * Copy a freshly built PDB into the content-addressed downstream symbol store
 * (build/crashdumps/syms/<pdb>/<KEY>/<pdb>), so old dumps stay symbolizable
 * after a rebuild. Called from make.mjs buildNodeAddon. Best-effort: returns
 * the stored path or null (never throws into the build).
 */
export function archivePdb(nodeFile = addonNode, pdbFile = addonPdb, store = symStore) {
  try {
    if (!fs.existsSync(nodeFile) || !fs.existsSync(pdbFile)) return null
    const info = readPeDebugInfo(nodeFile)
    if (!info) return null
    const pdbName = path.basename(pdbFile)
    const dest = path.join(store, pdbName, info.key, pdbName)
    fs.mkdirSync(path.dirname(dest), {recursive: true})
    fs.copyFileSync(pdbFile, dest)
    return dest
  } catch (err) {
    console.error('crashpad: archivePdb failed', err)
    return null
  }
}

// ---------------------------------------------------------------------------
// Toolchain + symbol path
// ---------------------------------------------------------------------------

/** Locate cdb.exe under the Windows SDK (or PATH). Returns path or null. */
function findCdb() {
  const roots = [
    process.env['ProgramFiles(x86)'] || 'C:\\Program Files (x86)',
    process.env['ProgramFiles'] || 'C:\\Program Files',
  ]
  for (const root of roots) {
    const p = path.join(root, 'Windows Kits', '10', 'Debuggers', 'x64', 'cdb.exe')
    if (fs.existsSync(p)) return p
  }
  const w = spawnSync('where', ['cdb.exe'], {encoding: 'utf8'})
  if (w.status === 0) return w.stdout.split(/\r?\n/)[0].trim()
  return null
}

/** Locate an interactive debugger (WinDbgX preferred, else windbg, else cdb). */
function findWinDbg() {
  const w = spawnSync('where', ['windbgx.exe', 'windbg.exe'], {encoding: 'utf8'})
  if (w.status === 0) {
    const hit = w.stdout.split(/\r?\n/).map((s) => s.trim()).find(Boolean)
    if (hit) return hit
  }
  return findCdb()
}

/** Build the cdb `-y` symbol path: addon dir + content-addressed store [+ MS srv]. */
function resolveSymPath(opts) {
  const parts = [nodeAddonDir, symStore].filter((p) => fs.existsSync(p))
  if (opts.publicSyms) {
    const cache = path.join(os.tmpdir(), 'sc-symcache')
    parts.push(`srv*${cache}*https://msdl.microsoft.com/download/symbols`)
  }
  return parts.join(';')
}

// ---------------------------------------------------------------------------
// Dump discovery
// ---------------------------------------------------------------------------

function listDumps() {
  if (!fs.existsSync(dumpDir)) return []
  return fs
    .readdirSync(dumpDir)
    .filter((f) => f.toLowerCase().endsWith('.dmp'))
    .map((f) => {
      const full = path.join(dumpDir, f)
      const st = fs.statSync(full)
      return {name: f, path: full, mtime: st.mtimeMs, size: st.size}
    })
    .sort((a, b) => b.mtime - a.mtime)
}

/** Resolve a dump argument (explicit path or newest); throws if none. */
function resolveDump(arg) {
  if (arg) {
    const p = path.isAbsolute(arg) ? arg : path.resolve(arg)
    if (!fs.existsSync(p)) fail(`dump not found: ${arg}`)
    return p
  }
  const all = listDumps()
  if (!all.length) fail(`no *.dmp found in ${dumpDir}`)
  return all[0].path
}

// ---------------------------------------------------------------------------
// cdb execution
// ---------------------------------------------------------------------------

/** Run cdb against a dump with a script, return its stdout. */
function runCdb(dump, script, opts) {
  const cdb = findCdb()
  if (!cdb) {
    fail(
      'cdb.exe not found. Install the "Debugging Tools for Windows" (Windows SDK) ' +
        'component, or add cdb.exe to PATH.',
    )
  }
  const sym = resolveSymPath(opts)
  const args = ['-z', dump, '-y', sym, '-c', `${script}; q`]
  const r = spawnSync(cdb, args, {encoding: 'utf8', maxBuffer: 64 * 1024 * 1024})
  if (r.error) fail(`cdb failed: ${r.error.message}`)
  return r.stdout || ''
}

/** Parse `kn` frame lines into {n, module, symbol, file, line}. */
function parseFrames(text) {
  const frames = []
  const re = /^\s*([0-9a-f]{2})\s+[`0-9a-f]+\s+[`0-9a-f]+\s+([^\s!]+)!([^\s+]+)(?:\+0x[0-9a-f]+)?(?:\s+\[([^\]]+?)\s+@\s+(\d+)\])?/i
  for (const ln of text.split(/\r?\n/)) {
    const m = re.exec(ln)
    if (m) frames.push({n: parseInt(m[1], 16), module: m[2], symbol: m[3], file: m[4], line: m[5] ? +m[5] : undefined})
  }
  return frames
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

function cmdList(opts) {
  const dumps = listDumps()
  if (opts.json) return print(dumps)
  if (!dumps.length) return console.log(`(no *.dmp in ${dumpDir})`)
  console.log(`${dumps.length} dump(s) in ${dumpDir}:\n`)
  for (const d of dumps) {
    let top = ''
    try {
      const frames = parseFrames(runCdb(d.path, '.ecxr; .lines -e; kn 4', opts))
      const f = frames.find((x) => x.symbol)
      if (f) top = `${f.module}!${f.symbol}`
    } catch {
      /* leave top blank if cdb unavailable */
    }
    const when = new Date(d.mtime).toISOString().replace('T', ' ').slice(0, 19)
    const kb = (d.size / 1024).toFixed(0).padStart(7)
    console.log(`  ${when}  ${kb} KB  ${d.name}${top ? `  → ${top}` : ''}`)
  }
}

function cmdWalk(dump, opts) {
  const out = runCdb(dump, '.ecxr; .lines -e; kn', opts)
  if (opts.json) return print({dump, frames: parseFrames(out)})
  console.log(out.trim())
}

function cmdInfo(dump, opts) {
  const out = runCdb(dump, `.lastevent; .ecxr; r; lmDvm ${ADDON_MODULE}`, opts)
  if (opts.json) {
    const ex = /Exception (\w+).*?at[^\n]*/is.exec(out) || /code ([0-9a-fx]+)/i.exec(out)
    return print({
      dump,
      pdbMatch: pdbLoaded(out),
      exception: ex ? ex[0].trim() : undefined,
      frames: parseFrames(runCdb(dump, '.ecxr; .lines -e; kn', opts)),
    })
  }
  console.log(out.trim())
  console.log(`\nPDB match for ${ADDON_MODULE}: ${pdbLoaded(out) ? 'OK' : 'NO — symbols will not resolve'}`)
}

function cmdThreads(dump, opts) {
  const out = runCdb(dump, '.lines -e; ~*kn', opts)
  console.log(out.trim())
}

function cmdEval(dump, script, opts) {
  if (!script) fail('eval needs a cdb command string, e.g. eval "dx @$curprocess"')
  console.log(runCdb(dump, script, opts).trim())
}

function cmdOpen(dump) {
  const dbg = findWinDbg()
  if (!dbg) fail('no WinDbg/cdb found to open interactively')
  const sym = resolveSymPath({})
  const interactive = /cdb\.exe$/i.test(dbg)
  console.log(`opening ${path.basename(dump)} in ${path.basename(dbg)} …`)
  spawnSync(dbg, ['-z', dump, '-y', sym], {stdio: interactive ? 'inherit' : 'ignore', detached: !interactive})
}

/** Does cdb's module/output text show a real PDB loaded for the addon? */
function pdbLoaded(out) {
  return /\((?:private )?pdb symbols\)/i.test(out) || /Symbols Type:\s*PDB/i.test(out)
}

function cmdSymcheck(dump, opts) {
  const out = runCdb(dump, `.reload /f ${ADDON_MODULE}.node; lm m ${ADDON_MODULE}`, opts)
  const ok = pdbLoaded(out)
  if (opts.json) print({dump, pdbMatch: ok})
  else console.log(ok ? `OK: PDB matches ${ADDON_MODULE} in this dump` : `MISMATCH: no matching PDB for ${ADDON_MODULE}`)
  if (!ok) process.exitCode = 1
}

function gitSha() {
  const r = spawnSync('git', ['-C', repoRoot, 'rev-parse', 'HEAD'], {encoding: 'utf8'})
  return r.status === 0 ? r.stdout.trim() : 'unknown'
}

function nwVersion() {
  try {
    const pkg = JSON.parse(fs.readFileSync(path.join(repoRoot, 'package.json'), 'utf8'))
    return pkg.dependencies?.nw || pkg.devDependencies?.nw || 'unknown'
  } catch {
    return 'unknown'
  }
}

/** Find a PDB (on-disk first, then the store) that this dump accepts. */
function matchingPdb(dump, opts) {
  if (fs.existsSync(addonPdb) && pdbLoaded(runCdb(dump, `.reload /f ${ADDON_MODULE}.node; lm m ${ADDON_MODULE}`, opts))) {
    return addonPdb
  }
  const base = path.join(symStore, 'sculptcore_node.pdb')
  if (fs.existsSync(base)) {
    for (const key of fs.readdirSync(base)) {
      const cand = path.join(base, key, 'sculptcore_node.pdb')
      if (fs.existsSync(cand)) return cand
    }
  }
  return null
}

function cmdPackage(dump, opts) {
  const stage = fs.mkdtempSync(path.join(os.tmpdir(), 'sc-crashpkg-'))
  fs.copyFileSync(dump, path.join(stage, path.basename(dump)))
  const pdb = matchingPdb(dump, opts)
  if (pdb) fs.copyFileSync(pdb, path.join(stage, 'sculptcore_node.pdb'))
  else console.warn('warning: no matching PDB found; packaging dump without symbols')
  fs.writeFileSync(
    path.join(stage, 'build-info.json'),
    JSON.stringify({dump: path.basename(dump), gitSha: gitSha(), nw: nwVersion(), pdb: pdb ? path.basename(pdb) : null}, null, 2),
  )
  fs.mkdirSync(artifactDir, {recursive: true})
  const outZip = path.join(artifactDir, path.basename(dump).replace(/\.dmp$/i, '') + '.zip')
  const ps = spawnSync(
    'powershell',
    ['-NoProfile', '-Command', `Compress-Archive -Path '${stage}\\*' -DestinationPath '${outZip}' -Force`],
    {encoding: 'utf8'},
  )
  fs.rmSync(stage, {recursive: true, force: true})
  if (ps.status !== 0) fail(`Compress-Archive failed: ${ps.stderr}`)
  console.log(`packaged → ${outZip}${pdb ? '' : ' (no PDB)'}`)
}

function cmdPrune(opts) {
  const keep = opts.keep ?? 10
  const dumps = listDumps()
  const drop = dumps.slice(keep)
  for (const d of drop) {
    fs.rmSync(d.path, {force: true})
    const zip = path.join(artifactDir, path.basename(d.path).replace(/\.dmp$/i, '.zip'))
    if (fs.existsSync(zip)) fs.rmSync(zip, {force: true})
  }
  console.log(`kept ${Math.min(keep, dumps.length)}, removed ${drop.length} dump(s)`)
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

function fail(msg) {
  console.error(`error: ${msg}`)
  process.exit(1)
}
function print(obj) {
  console.log(JSON.stringify(obj, null, 2))
}

function main(argv) {
  const opts = {json: false, publicSyms: false, keep: undefined}
  const rest = []
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]
    if (a === '--json') opts.json = true
    else if (a === '--public-syms') opts.publicSyms = true
    else if (a === '--keep') opts.keep = parseInt(argv[++i], 10)
    else rest.push(a)
  }
  const cmd = rest.shift() || 'walk'

  switch (cmd) {
    case 'list':
      return cmdList(opts)
    case 'walk':
      return cmdWalk(resolveDump(rest[0]), opts)
    case 'info':
      return cmdInfo(resolveDump(rest[0]), opts)
    case 'threads':
      return cmdThreads(resolveDump(rest[0]), opts)
    case 'open':
      return cmdOpen(resolveDump(rest[0]))
    case 'eval':
      return cmdEval(resolveDump(rest[0]), rest[1], opts)
    case 'symcheck':
      return cmdSymcheck(resolveDump(rest[0]), opts)
    case 'package':
      return cmdPackage(resolveDump(rest[0]), opts)
    case 'prune':
      return cmdPrune(opts)
    default:
      fail(`unknown command "${cmd}". Run with no args for usage, or see the header.`)
  }
}

// Only run the CLI when invoked directly (not when imported by make.mjs).
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2))
}
