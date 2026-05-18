#!/usr/bin/env node
import Path from 'path'
import fs from 'fs'
import child_process from 'child_process'
import yargs from 'yargs'
import {hideBin} from 'yargs/helpers'
import {termColor} from './source/litestl/tests/termColor.js'
import {syntaxHighlight} from './tools/syntaxHighlight.mjs'

const CMAKE_BUILD_TYPE = 'Debug' //'RelWithDebInfo'
const EMSDK_VERSION = fs.readFileSync('./emsdkVersion.txt', 'utf-8').trim()
const CMAKE_ARGS = `-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DBUILD_WASM=ON -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`

/**
 * Ensurse final linked files are destroyed,
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

function run(cmd) {
  try {
    return child_process.execSync(cmd, {shell: true, stdio: 'inherit'})
  } catch (error) {
    process.stderr.write(error.message + '\n')
    process.exit(1)
  }
}

function summarizeErrors(buf) {
  return new Promise((accept, reject) => {
    if (buf.length < 2048) {
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

yargs(hideBin(process.argv))
  .scriptName('make.mjs')
  .command('configure [target]', 'Configure the build', targetPositional, ({target}) => {
    setupPNPM()
    ensureDir('build')
    const dir = buildDir(target)
    ensureDir(dir)
    const env = envPrefix(target)
    if (target === 'native') {
      run(
        `cd ${dir} && ${env} cmake ../.. -G Ninja --toolchain ./build_files/native-clang.cmake -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} `
      )
    } else {
      run(`cd ${dir} && ${env} emcmake cmake .. ${CMAKE_ARGS}`)
    }
  })
  .command('build [target]', 'Build', targetPositional, async ({target}) => {
    console.log('Building...')
    const dir = buildDir(target)
    const env = envPrefix(target)
    if (target === 'wasm') {
      deleteFinalWasmFiles()
    }

    await runBuild(`cd ${dir} && ${env} cmake --build . `)

    // copy wasm to typescript/
    fs.mkdirSync('typescript/build', {recursive: true})
    fs.copyFileSync('build/sculptcore.js', 'typescript/build/sculptcore.js')
    fs.copyFileSync('build/sculptcore.wasm', 'typescript/build/sculptcore.wasm')
    fs.copyFileSync('build/sculptcore-browser.js', 'typescript/build/sculptcore-browser.js')
    fs.copyFileSync('build/sculptcore-browser.wasm', 'typescript/build/sculptcore-browser.wasm')

    if (target === 'wasm') {
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
      targetTest += '.cc_out.exe'

      const dirs = ['build/native/tests', 'build/native/source/litestl/tests']
      for (const dir of dirs) {
        let path = Path.join(dir, targetTest)
        if (fs.existsSync(path)) {
          run(path)
          return
        } else if (fs.existsSync(path + '.exe')) {
          run(path + '.exe')
          return
        }
      }
      process.stderr.write(`Could not find test ${targetTest}\n`)
      process.exit(-1)
    }
  })
  .command('install-emsdk', 'Install pinned emsdk', {}, () => {
    console.log('Installing emsdk...')
    run('git submodule init')
    run('git submodule update')
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
