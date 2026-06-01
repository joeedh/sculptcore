import fs from 'fs'
import Path from 'path'
import child_process from 'child_process'
import {fileURLToPath} from 'url'

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

  // Released VS installs put their year in the folder name (2019/2022/2026);
  // Insiders/preview builds instead use a bare IDE major (e.g. "18"), and that
  // env doesn't surface a bundled cmake the way a stable install does. Prefer a
  // year-named (stable) folder, newest first, and only fall back to a bare-major
  // folder if no stable one exists.
  const isYear = (d) => /^\d{4}$/.test(d)
  const sorted = dir
    .filter((d) => !isNaN(parseInt(d)))
    .sort((a, b) => getVersion(parseInt(b)) - getVersion(parseInt(a)))

  const version = sorted.filter(isYear)[0] ?? sorted[0]
  if (version === undefined) {
    process.stderr.write('No Visual Studio version found\n')
    process.exit(-1)
  }

  //C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build

  const path = join(VSPath, '' + version, 'Community', 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
  process.chdir(join(VSPath, '' + version, 'Community'))

  if (!fs.existsSync(path)) {
    process.stderr.write('Could not find vcvars64.bat\n')
    process.stderr.write(`  tried "${path}"\n`)
    process.exit(-1)
  }

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
  childEnv.PATH = [`${systemRoot}\\System32`, systemRoot, `${systemRoot}\\System32\\Wbem`,
    nodeDir, cargoBin, vulkanBin, ...toolDirs]
    .filter(Boolean)
    .join(';')
  const result = child_process.execSync(`cmd /s /c \"call \"${path}\" && set\"`, {env: childEnv})
  const env = result
    .toString('latin1')
    .split('\n')
    .filter((l) => !l.startsWith('**') && !l.trim().toLowerCase().startsWith('[vcvarsall.bat]'))
    .join('\n')

  process.chdir(CWD)
  return env
}

function getEmsdkEnv() {
  const CWD = process.cwd()
  const scriptPath = fileURLToPath(import.meta.url)

  process.chdir(Path.dirname(scriptPath))
  process.chdir('emsdk')

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
        if (
          (val.startsWith('"') && val.endsWith('"')) ||
          (val.startsWith("'") && val.endsWith("'"))
        ) {
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

const defaultShell = process.platform === 'win32' ? 'cmd' : process.env.SHELL || '/bin/sh'

try {
  child_process.execSync(args[0] ? args.join(' ') : defaultShell, {
    stdio: 'inherit',
    shell: true,
  })
} catch (e) {
  process.exit(typeof e.status === 'number' ? e.status : 1)
}
