#!/usr/bin/env node
// Download the pinned wgpu-native prebuilt (webgpu.h impl for the *native*
// build — emdawnwebgpu supplies it under WASM). Extracts include/ + lib/ next
// to this script. Idempotent: skips the download if the pinned tag is already
// present. Pin tracked in wgpu-native-meta/wgpu-native-git-tag.
import fs from 'fs'
import os from 'os'
import path from 'path'
import https from 'https'
import child_process from 'child_process'
import {fileURLToPath} from 'url'

const here = path.dirname(fileURLToPath(import.meta.url))
const TAG = fs.readFileSync(path.join(here, 'wgpu-native-meta/wgpu-native-git-tag'), 'utf-8').trim()

// win64 + linux-x64 release prebuilts are wired into the native build. macOS is
// not yet (the OpenBLAS dep build needs work there first).
function assetName() {
  if (process.platform === 'win32' && process.arch === 'x64') {
    return 'wgpu-windows-x86_64-msvc-release.zip'
  }
  if (process.platform === 'linux' && process.arch === 'x64') {
    return 'wgpu-linux-x86_64-release.zip'
  }
  throw new Error(`wgpu-native fetch: unsupported platform ${process.platform}/${process.arch}`)
}

const stamp = path.join(here, 'lib', '.fetched-tag')
if (
  fs.existsSync(stamp) &&
  fs.readFileSync(stamp, 'utf-8').trim() === TAG &&
  fs.existsSync(path.join(here, 'include/webgpu/webgpu.h'))
) {
  console.log(`wgpu-native ${TAG} already present`)
  process.exit(0)
}

const asset = assetName()
const url = `https://github.com/gfx-rs/wgpu-native/releases/download/${TAG}/${asset}`
console.log(`Fetching wgpu-native ${TAG}\n  ${url}`)

function download(u, dest, redirects = 0) {
  return new Promise((accept, reject) => {
    if (redirects > 10) return reject(new Error('too many redirects'))
    https
      .get(u, (res) => {
        if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
          res.resume()
          return accept(download(res.headers.location, dest, redirects + 1))
        }
        if (res.statusCode !== 200) {
          res.resume()
          return reject(new Error(`HTTP ${res.statusCode} for ${u}`))
        }
        const out = fs.createWriteStream(dest)
        res.pipe(out)
        out.on('finish', () => out.close(accept))
        out.on('error', reject)
      })
      .on('error', reject)
  })
}

const tmp = path.join(os.tmpdir(), `wgpu-native-${TAG}-${asset}`)
await download(url, tmp)
console.log(`Downloaded ${(fs.statSync(tmp).size / 1e6).toFixed(1)} MB, extracting...`)

// The zip lays out include/ and lib/ at its root — extract straight here.
for (const d of ['include', 'lib']) {
  fs.rmSync(path.join(here, d), {recursive: true, force: true})
}
if (process.platform === 'win32') {
  child_process.execSync(
    `powershell -NoProfile -Command "Expand-Archive -Force -LiteralPath '${tmp}' -DestinationPath '${here}'"`,
    {stdio: 'inherit'}
  )
} else {
  child_process.execSync(`unzip -o "${tmp}" -d "${here}"`, {stdio: 'inherit'})
}
fs.rmSync(tmp, {force: true})

fs.mkdirSync(path.join(here, 'lib'), {recursive: true})
fs.writeFileSync(stamp, TAG + '\n')
console.log(`wgpu-native ${TAG} ready at ${here}`)
