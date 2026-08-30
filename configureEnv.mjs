import fs from 'fs'
import os from 'os'
import Path from 'path'
import child_process from 'child_process'
import {fileURLToPath} from 'url'

// PATH as inherited from the user's shell, captured before getVSEnv() rebuilds
// it from vcvars. Used to relocate an on-PATH sccache (below).
const INHERITED_PATH = process.env.PATH || ''

// The VS Installer dir, home of vswhere.exe. Needed both *before* vcvars (VS 18's
// vcvars64.bat shells out to vswhere and hangs when it isn't on PATH) and after
// (cmake-js probes for VS through it).
const VSWHERE_DIR = Path.join(
  process.env['ProgramFiles(x86)'] || 'C:\\Program Files (x86)',
  'Microsoft Visual Studio',
  'Installer'
)

// Return the directory of `name` on `pathStr`, or null. Used to keep sccache
// reachable after the Windows PATH is rebuilt from scratch by vcvars.
function findExeDir(name, pathStr) {
  if (!pathStr) return null
  const exts = process.platform === 'win32' ? ['.exe', '.cmd', '.bat', ''] : ['']
  for (const dir of pathStr.split(Path.delimiter)) {
    if (!dir) continue
    for (const ext of exts) {
      try {
        if (fs.existsSync(Path.join(dir, name + ext))) return dir
      } catch {
        /* unreadable dir entry on PATH; skip */
      }
    }
  }
  return null
}

function getVSEnv() {
  const PROGFILES = process.env.ProgramFiles
  const APPDATA = process.env.APPDATA
  const USERPROFILE = process.env.USERPROFILE

  const {join} = Path

  const VSPath = join(PROGFILES, 'Microsoft Visual Studio')

  if (!fs.existsSync(VSPath)) {
    process.stderr.write('Visual Studio is not installed\n')
    process.exit(-1)
  }

  const CWD = process.cwd()

  const dir = fs.readdirSync(VSPath).filter((d) => fs.statSync(join(VSPath, d)).isDirectory())

  const versionMap = new Map([
    [2022, 17],
    [2026, 18],
    [2020, 16],
  ])

  const getVersion = (d) => versionMap.get(d) ?? d

  const isYear = (d) => /^\d{4}$/.test(d)
  const sorted = dir
    .filter((d) => !isNaN(parseInt(d)))
    .sort((a, b) => getVersion(parseInt(b)) - getVersion(parseInt(a)))

  // earlier version may have stale asan, use latest one
  const version = sorted[0] //sorted.filter(isYear)[0] ?? sorted[0]
  if (version === undefined) {
    process.stderr.write('No Visual Studio version found\n')
    process.exit(-1)
  }

  //C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build

  // The edition subdir is not always "Community" (GitHub runners ship
  // Enterprise; devs may have Professional/BuildTools/Preview). Pick the first
  // edition under this version that actually has vcvars64.bat.
  const versionDir = join(VSPath, '' + version)
  const editions = ['Community', 'Professional', 'Enterprise', 'BuildTools', 'Preview']
  let edition = null
  let path = null
  for (const ed of editions) {
    const cand = join(versionDir, ed, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
    if (fs.existsSync(cand)) {
      edition = ed
      path = cand
      break
    }
  }

  if (!path) {
    process.stderr.write('Could not find vcvars64.bat\n')
    process.stderr.write(`  tried editions [${editions.join(', ')}] under "${versionDir}"\n`)
    process.exit(-1)
  }
  process.chdir(join(versionDir, edition))

  // vcvars64.bat chokes with "\Windows was unexpected at this time." when
  // the inherited PATH is in Git-Bash form (":"-separated, "/c/..." paths).
  // Give it a clean, minimal Windows PATH — vcvars builds its own PATH from
  // scratch anyway, so this is sufficient. Also drop env var names cmd
  // can't parse.
  const systemRoot = process.env.SystemRoot || 'C:\\Windows'
  const childEnv = {}
  for (const [k, v] of Object.entries(process.env)) {
    if (!k || !/^[A-Za-z_][A-Za-z0-9_()]*$/.test(k)) continue
    childEnv[k] = v
  }
  // Keep Node.js on PATH so cmake's find_program(NODE_EXECUTABLE) finds it
  // (needed for the WGSL->SPIR-V codegen step in source/spatial). Also keep
  // the user's cargo bin dir so `naga` is reachable during the codegen step.
  const nodeDir = Path.dirname(process.execPath)
  const cargoBin = process.env.USERPROFILE ? Path.join(process.env.USERPROFILE, '.cargo', 'bin') : ''
  // sbrush WGSL/SPIR-V backends need `tint` and `spirv-val` on PATH. spirv-val
  // ships in the Vulkan SDK's Bin; tint has no canonical install dir, so accept
  // extra tool dirs via SBRUSH_TOOL_PATH (";"-separated) as an escape hatch.
  const vulkanBin = process.env.VULKAN_SDK ? Path.join(process.env.VULKAN_SDK, 'Bin') : ''
  const toolDirs = process.env.SBRUSH_TOOL_PATH ? process.env.SBRUSH_TOOL_PATH.split(';') : []

  let tintPath = findExeDir('tint')
  if (tintPath) {
    toolDirs.push(tintPath)
  }

  childEnv.PATH = [
    `${systemRoot}\\System32`,
    systemRoot,
    `${systemRoot}\\System32\\Wbem`,
    nodeDir,
    cargoBin,
    vulkanBin,
    fs.existsSync(Path.join(VSWHERE_DIR, 'vswhere.exe')) ? VSWHERE_DIR : '',
    ...toolDirs,
  ]
    .filter(Boolean)
    .join(';')
  // `set` writes to a temp file rather than a pipe: VS 2026's vcvars leaves a
  // background child holding the inherited stdout handle, so a piped execSync
  // never sees EOF and hangs long after vcvars itself has finished. The timeout
  // is a backstop for a vcvars that fails by hanging (an unreachable helper).
  const dumpPath = Path.join(fs.mkdtempSync(Path.join(os.tmpdir(), 'sculptcore-vcvars-')), 'env.txt')
  let dump
  try {
    child_process.execSync(`cmd /s /c \"call \"${path}\" && set > \"${dumpPath}\"\"`, {
      env    : childEnv,
      stdio  : 'ignore',
      timeout: 120000,
    })
    dump = fs.readFileSync(dumpPath, 'latin1')
  } finally {
    fs.rmSync(Path.dirname(dumpPath), {recursive: true, force: true})
  }
  const env = dump
    .split('\n')
    .filter((l) => !l.startsWith('**') && !l.trim().toLowerCase().startsWith('[vcvarsall.bat]'))
    .join('\n')

  process.chdir(CWD)
  return env
}

function getEmsdkEnv() {
  const CWD = process.cwd()
  const scriptPath = fileURLToPath(import.meta.url)

  // SCULPTCORE_EMSDK_DIR lets a worktree borrow another worktree's emsdk install
  // instead of re-running the expensive `install-emsdk`. construct_env records
  // absolute paths into whichever emsdk dir it runs in, so pointing here at e.g.
  // the main worktree's emsdk makes emcc/node/etc. resolve there. The
  // create-worktree skill sets this; unset, the in-tree ./emsdk is used.
  process.chdir(Path.dirname(scriptPath))
  if (process.env.SCULPTCORE_EMSDK_DIR) {
    if (!fs.existsSync(process.env.SCULPTCORE_EMSDK_DIR)) {
      process.stderr.write(`SCULPTCORE_EMSDK_DIR does not exist: ${process.env.SCULPTCORE_EMSDK_DIR}\n`)
      process.exit(-1)
    }
    process.chdir(process.env.SCULPTCORE_EMSDK_DIR)
  } else {
    process.chdir('emsdk')
  }

  const childEnv = {...process.env}
  // construct_env prints informational banners ("Setting up EMSDK
  // environment ...") on stdout unless EMSDK_QUIET is set. The Linux/macOS
  // path parses stdout directly, so silence the banners or they'll be
  // mistaken for env assignments.
  childEnv.EMSDK_QUIET = '1'

  if (process.platform === 'win32') {
    // we do not want cygpaths
    delete childEnv.MSYSTEM
  }

  const result = child_process.execSync('python emsdk.py construct_env', {
    stdio   : 'pipe',
    shell   : false,
    detached: false,
    env     : childEnv,
  })

  let env
  if (process.platform === 'win32') {
    env = fs
      .readFileSync('emsdk_set_env.bat', 'utf8')
      .replace(/\r/g, '')
      .replace(/\n\n+/g, '\n')
      .split('\n')
      .map((l) => {
        if (l.toLowerCase().startsWith('set ')) {
          l = l.slice(4)
        }
        return l
      })
      .join('\n')
  } else {
    if (!result) {
      process.stderr.write('Failed to get emsdk environment\n')
      process.exit(-1)
    }
    env = result
      .toString('utf8')
      .replace(/\r/g, '')
      .split('\n')
      .map((l) => l.trim())
      .filter((l) => l.toLowerCase().startsWith('export '))
      .map((l) => {
        l = l.slice(7).trim()
        if (l.endsWith(';')) l = l.slice(0, -1).trim()
        const eq = l.indexOf('=')
        if (eq <= 0) return ''
        const key = l.slice(0, eq).trim()
        let val = l.slice(eq + 1).trim()
        if ((val.startsWith('"') && val.endsWith('"')) || (val.startsWith("'") && val.endsWith("'"))) {
          val = val.slice(1, -1)
        }
        return `${key}=${val}`
      })
      .filter((l) => l.length > 0)
      .join('\n')
  }

  process.chdir(CWD)
  return env
}

let args = process.argv.slice(2)
let target = 'native'

if (args[0] === '--emsdk') {
  target = 'emsdk'
  args = args.slice(1)
}

let env = ''
if (target === 'native') {
  // On non-Windows, the system toolchain is already on PATH; nothing to do.
  if (process.platform === 'win32') {
    env = getVSEnv()
  }
} else {
  env = getEmsdkEnv()
}

if (process.argv.includes('--output-env')) {
  process.stdout.write(env)
  process.exit(0)
}

for (const line of env.replace(/\r/g, '').split('\n')) {
  if (!line) continue
  const eq = line.indexOf('=')
  if (eq <= 0) continue
  process.env[line.slice(0, eq)] = line.slice(eq + 1)
}

// vcvars rebuilds PATH from scratch on Windows, dropping anything that was on
// the user's PATH (including sccache). Re-add sccache's dir so the native
// toolchain's find_program(sccache) can find it. SCCACHE_* env vars
// (SCCACHE_DIR, SCCACHE_BASEDIRS, ...) survive untouched — we never strip the
// inherited environment, only PATH gets overwritten above.
if (target === 'native' && process.platform === 'win32') {
  const sccacheDir = findExeDir('sccache', INHERITED_PATH)
  const curPath = process.env.PATH || ''
  const already = curPath.split(';').some((p) => p && p.toLowerCase() === sccacheDir?.toLowerCase())
  if (sccacheDir && !already) {
    process.env.PATH = curPath ? `${curPath};${sccacheDir}` : sccacheDir
  }
  // vcvars' rebuilt PATH also lacks the VS Installer dir, so `vswhere.exe` is
  // unreachable — cmake-js (the node-addon configure) aborts on its VS probe
  // without it. Re-add it the same way.
  if (fs.existsSync(Path.join(VSWHERE_DIR, 'vswhere.exe'))) {
    const p = process.env.PATH || ''
    if (!p.split(';').some((d) => d && d.toLowerCase() === VSWHERE_DIR.toLowerCase())) {
      process.env.PATH = p ? `${p};${VSWHERE_DIR}` : VSWHERE_DIR
    }
  }
}

const defaultShell = process.platform === 'win32' ? 'cmd' : process.env.SHELL || '/bin/sh'

// We run the wrapped command through a shell (to resolve .bat wrappers like
// emcmake on Windows), but our argv has already been split by the *outer* shell
// that invoked us — the original quoting is gone. Re-quote each arg so shell
// metacharacters inside a single argument (e.g. the ';' in a CMake list like
// -DSUITESPARSE_ENABLE_PROJECTS="a;b;c", or the '|' in a ctest -E regex) stay
// literal instead of being re-parsed as command separators / pipes.
function quoteArg(a) {
  if (process.platform === 'win32') {
    // cmd.exe: wrap in double quotes when the arg holds a space or metachar.
    return /[\s;,&|<>^()"]/.test(a) ? '"' + a.replace(/"/g, '""') + '"' : a
  }
  // POSIX sh: single-quote anything outside a safe set, escaping embedded quotes.
  return /[^A-Za-z0-9_\/:=@%+.,-]/.test(a) ? "'" + a.replace(/'/g, "'\\''") + "'" : a
}

try {
  child_process.execSync(args[0] ? args.map(quoteArg).join(' ') : defaultShell, {
    stdio: 'inherit',
    shell: true,
  })
} catch (e) {
  process.exit(typeof e.status === 'number' ? e.status : 1)
}
