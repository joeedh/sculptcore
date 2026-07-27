// Dawn/WebGPU replay harness for the sbrush GPU compute kernels.
//
// Feeds a fixture captured by `debug_app --gpu-capture` (the exact per-binding
// buffer bytes a native wgsl stroke uploaded) through a real WebGPU runtime
// (Dawn, via @kmamal/gpu — runs headless on software Vulkan in CI), then diffs
// the readback against the native reference. This validates the SAME WGSL the
// native SPIR-V path runs (sourced from build/native/sbrush_out/spirv/<k>.wgsl)
// on a WebGPU runtime, without reimplementing the engine's mesh/spatial setup.
//
// CLI:  node tests/webgpu/replay.mjs --wgsl-dir DIR --fixture FILE.json
// Lib:  import { replayFixture } from './replay.mjs'

import gpu from '@kmamal/gpu'
import fs from 'node:fs'
import path from 'node:path'
import {pathToFileURL} from 'node:url'

// @kmamal/gpu does not inject the WebGPU enum globals; use the numeric values.
const BufferUsage = {MAP_READ: 1, COPY_SRC: 4, COPY_DST: 8, UNIFORM: 0x40, STORAGE: 0x80}
const MapMode = {READ: 1}

const ATOL = 1e-5
const RTOL = 1e-4

function b64bytes(s) {
  if (s === null || s === undefined) return null
  return Buffer.from(s, 'base64')
}

// Parse `@group(0) @binding(N) var<...> name: type;` lines into a binding map
// describing each kernel's actual layout, so the WebGPU bind group layout we
// build matches exactly what the shader declares (access qualifiers included).
function parseBindings(wgsl) {
  const out = []
  const re = /@group\(0\)\s*@binding\((\d+)\)\s*(var<[^>]*>|var)\s+([A-Za-z0-9_]+)\s*:\s*([^;]+);/g
  let m
  while ((m = re.exec(wgsl)) !== null) {
    const binding = parseInt(m[1], 10)
    const decl = m[2].replace(/\s+/g, '')
    const type = m[4].trim()
    let kind
    if (decl === 'var<uniform>') kind = 'uniform'
    else if (decl === 'var<storage,read_write>') kind = 'storage-rw'
    else if (decl === 'var<storage,read>') kind = 'storage-ro'
    else if (/^texture_2d/.test(type)) kind = 'texture'
    else if (/^sampler/.test(type)) kind = 'sampler'
    else throw new Error(`unhandled binding decl @binding(${binding}): ${decl} ${type}`)
    out.push({binding, kind})
  }
  return out.sort((a, b) => a.binding - b.binding)
}

function layoutEntry(b) {
  const visibility = 4 // GPUShaderStage.COMPUTE
  switch (b.kind) {
    case 'uniform':
      return {binding: b.binding, visibility, buffer: {type: 'uniform'}}
    case 'storage-rw':
      return {binding: b.binding, visibility, buffer: {type: 'storage'}}
    case 'storage-ro':
      return {binding: b.binding, visibility, buffer: {type: 'read-only-storage'}}
    case 'texture':
      return {binding: b.binding, visibility, texture: {sampleType: 'unfilterable-float', viewDimension: '2d'}}
    case 'sampler':
      return {binding: b.binding, visibility, sampler: {type: 'non-filtering'}}
  }
}

function makeBuffer(device, bytes, usage) {
  const size = Math.max(4, (bytes.byteLength + 3) & ~3)
  const buf = device.createBuffer({size, usage, mappedAtCreation: true})
  new Uint8Array(buf.getMappedRange()).set(new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength))
  buf.unmap()
  return buf
}

// Rewrite the brush_tex (binding 8) sampled texture, which @kmamal/gpu 0.2.1
// cannot bind: its native createView unconditionally emits a
// TextureComponentSwizzle the bundled Dawn gates behind an unrequestable
// toggle, making EVERY texture view (hence every kernel) invalid.
//
// Textured strokes: rebind binding 8 as a read-only storage buffer of the same
// row-major R32 floats and rewrite the two intrinsics to array indexing. The
// arithmetic is byte-identical (same f32 values at the same integer coords feed
// the same bilinear math); the shader already clamps coords to [0,w/h) before
// the load, so indexing needs no edge handling.
//
// Untextured strokes: the native 1x1 white dummy makes sampleBrushTex return
// 1.0, so constant-fold the loads to 1.0 and drop binding 8 entirely. This also
// keeps for_neighbor kernels (smooth: bindings 11/12/13) within the adapter's
// maxStorageBuffersPerShaderStage — a storage binding 8 would push them over.
function transformWgsl(wgsl, hasTexture, w, h) {
  if (hasTexture) {
    let out = wgsl.replace(
      /@group\(0\)\s*@binding\(8\)\s*var\s+brush_tex:\s*texture_2d<f32>;/,
      '@group(0) @binding(8) var<storage, read> brush_tex: array<f32>;'
    )
    out = out.replace(/textureDimensions\(brush_tex\)/g, `vec2<u32>(${w}u, ${h}u)`)
    out = out.replace(
      /textureLoad\(brush_tex,\s*vec2<i32>\(\s*([^,]+?)\s*,\s*([^)]+?)\s*\),\s*0\)\.r/g,
      (_m, x, y) => `brush_tex[u32(${y}) * ${w}u + u32(${x})]`
    )
    return out
  }
  let out = wgsl.replace(/@group\(0\)\s*@binding\(8\)\s*var\s+brush_tex:\s*texture_2d<f32>;\s*\n/, '')
  out = out.replace(/textureDimensions\(brush_tex\)/g, 'vec2<u32>(1u, 1u)')
  out = out.replace(/textureLoad\(brush_tex,\s*vec2<i32>\([^)]*\),\s*0\)\.r/g, '1.0')
  return out
}

// Pack a stride-16 (vec3 + pad) byte buffer down to tight xyz f32 triples.
function packVec3(stride16, vertCount) {
  const src = new Float32Array(stride16.buffer, stride16.byteOffset, vertCount * 4)
  const out = new Float32Array(vertCount * 3)
  for (let i = 0; i < vertCount; i++) {
    out[i * 3 + 0] = src[i * 4 + 0]
    out[i * 3 + 1] = src[i * 4 + 1]
    out[i * 3 + 2] = src[i * 4 + 2]
  }
  return out
}

function diff(actual, expected) {
  let maxAbs = 0
  let bad = 0
  let badIdx = -1
  for (let i = 0; i < expected.length; i++) {
    const a = actual[i]
    const e = expected[i]
    const ae = Math.abs(a - e)
    if (ae > maxAbs) maxAbs = ae
    if (ae > ATOL + RTOL * Math.abs(e)) {
      if (bad === 0) badIdx = i
      bad++
    }
  }
  return {maxAbs, bad, badIdx}
}

// Exact (integer) diff for int attr layers like the poly-group `group` id.
function diffInt(actual, expected) {
  let bad = 0,
    badIdx = -1,
    maxAbs = 0
  for (let i = 0; i < expected.length; i++) {
    const d = Math.abs(actual[i] - expected[i])
    if (d > maxAbs) maxAbs = d
    if (actual[i] !== expected[i]) {
      if (bad === 0) badIdx = i
      bad++
    }
  }
  return {maxAbs, bad, badIdx}
}

// Element stride (bytes) of a WGSL storage `array<T>` attr binding, for sizing
// the replay buffer + readback. scalars/i32/u32/f32 = 4; vec2 = 8; vec3/vec4 = 16.
function wgslElemSize(type) {
  if (/vec[34]/.test(type)) return 16
  if (/vec2/.test(type)) return 8
  return 4
}

export async function replayFixture(fixturePath, wgslDir) {
  const fx = JSON.parse(fs.readFileSync(fixturePath, 'utf8'))
  const hasTexture = !!fx.texture
  const texW = hasTexture ? fx.texture.width : 1
  const texH = hasTexture ? fx.texture.height : 1
  const rawWgsl = fs.readFileSync(path.join(wgslDir, `${fx.kernel}.wgsl`), 'utf8')
  const wgsl = transformWgsl(rawWgsl, hasTexture, texW, texH)
  const bindings = parseBindings(wgsl)
  const has = (n) => bindings.some((b) => b.binding === n)

  const instance = gpu.create([])
  const adapter = await instance.requestAdapter()
  // for_neighbor kernels bind up to 10 storage buffers (incl. sb_disp at 25;
  // the falloff LUT is a uniform precisely to stay at the adapter ceiling of
  // 10), above WebGPU's default per-stage limit of 8; request the adapter's max.
  const device = await adapter.requestDevice({
    requiredLimits: {
      maxStorageBuffersPerShaderStage: adapter.limits.maxStorageBuffersPerShaderStage,
    },
  })

  const module = device.createShaderModule({code: wgsl})
  const bgl = device.createBindGroupLayout({entries: bindings.map(layoutEntry)})
  const pipelineLayout = device.createPipelineLayout({bindGroupLayouts: [bgl]})
  const pipeline = device.createComputePipeline({
    layout : pipelineLayout,
    compute: {module, entryPoint: 'main'},
  })

  const vc = fx.vertCount
  // Persistent across dabs (mirrors the native co/no/mask that carry the
  // previous dab's result into the next), matching the C++ executor.
  const coBuf = makeBuffer(device, b64bytes(fx.co), BufferUsage.STORAGE | BufferUsage.COPY_SRC | BufferUsage.COPY_DST)
  const noBuf = makeBuffer(device, b64bytes(fx.no), BufferUsage.STORAGE | BufferUsage.COPY_SRC | BufferUsage.COPY_DST)
  const maskBuf = makeBuffer(
    device,
    b64bytes(fx.mask),
    BufferUsage.STORAGE | BufferUsage.COPY_SRC | BufferUsage.COPY_DST
  )

  // binding 8 is now a storage buffer (see transformWgsl): the row-major R32
  // pixels, or a single 1.0 standing in for the native 1x1 white dummy.
  const texBytes = fx.texture
    ? b64bytes(fx.texture.pixels)
    : Buffer.from(new Uint8Array(new Float32Array([1.0]).buffer))
  const texBuf = has(8) ? makeBuffer(device, texBytes, BufferUsage.STORAGE) : null
  // binding 9 (sampler) is declared by every kernel but unused (no
  // textureSample); bind a real one so the explicit layout is satisfied.
  const sampler = has(9) ? device.createSampler({magFilter: 'nearest', minFilter: 'nearest'}) : null

  let coPrevBuf = null
  let nbrMetaBuf = null
  let nbrVertsBuf = null
  if (has(11)) {
    coPrevBuf = device.createBuffer({
      size : vc * 16,
      usage: BufferUsage.STORAGE | BufferUsage.COPY_DST,
    })
  }
  if (has(12)) nbrMetaBuf = makeBuffer(device, b64bytes(fx.nbrMeta), BufferUsage.STORAGE)
  if (has(13)) nbrVertsBuf = makeBuffer(device, b64bytes(fx.nbrVerts), BufferUsage.STORAGE)

  // binding 25 (kDispBinding): accumulated non-accumulate displacement, zero at
  // stroke begin like the native dispatchers (nothing deposited yet, so the
  // derived base `co - disp` is the stroke-start surface). Persistent across dabs.
  const dispBuf = has(25) ? makeBuffer(device, Buffer.alloc(vc * 16), BufferUsage.STORAGE) : null

  // binding 23 (kDabStampBinding): grab-class per-vertex first-touch stamps,
  // zero-filled at stroke begin exactly like the native dispatchers (gen 0
  // never matches — dab gens start at 1). Persistent across dabs.
  const dabStampBuf = has(23) ? makeBuffer(device, Buffer.alloc(vc * 4), BufferUsage.STORAGE) : null

  // binding 24 (kAutomaskBinding): per-vertex cavity automask factor. Use the
  // captured bytes when present; identity 1.0 otherwise (cavity off), matching
  // the native dispatchers' default fill.
  let automaskBuf = null
  if (has(24)) {
    const bytes = fx.automask ? b64bytes(fx.automask) : Buffer.from(new Float32Array(vc).fill(1.0).buffer)
    automaskBuf = makeBuffer(device, bytes, BufferUsage.STORAGE)
  }

  // Custom attribute layer (binding >=14, e.g. color's float4 or polygroup's
  // int "group"). Persistent across dabs like co/no — the kernel accumulates.
  // Seed from the captured input (fx.attrIn) or zeros; read back + diffed at the
  // end when the fixture carries expectAttr (attr-output kernels). Before this,
  // attr bindings were never bound, so attr kernels failed bind-group validation
  // and silently no-op'd (their unchanged co trivially passing).
  const attrSlot = fx.attrSlot ?? 14
  let attrBuf = null
  let attrElemSize = 0
  if (has(attrSlot)) {
    const ab = bindings.find((b) => b.binding === attrSlot)
    attrElemSize = wgslElemSize(ab ? ab.type : 'f32')
    const initBytes = fx.attrIn ? b64bytes(fx.attrIn) : Buffer.alloc(vc * attrElemSize)
    attrBuf = makeBuffer(device, initBytes, BufferUsage.STORAGE | BufferUsage.COPY_SRC | BufferUsage.COPY_DST)
  }

  for (const dab of fx.dabs) {
    const uniqueBuf = makeBuffer(device, b64bytes(dab.unique), BufferUsage.STORAGE)
    const nodesBuf = makeBuffer(device, b64bytes(dab.nodes), BufferUsage.STORAGE)
    const brushUBuf = makeBuffer(device, b64bytes(dab.brushU), BufferUsage.UNIFORM | BufferUsage.COPY_DST)
    const ctxUBuf = makeBuffer(device, b64bytes(dab.ctxU), BufferUsage.UNIFORM | BufferUsage.COPY_DST)
    const falloffBuf = makeBuffer(device, b64bytes(dab.falloff), BufferUsage.UNIFORM)
    const strokeBuf = makeBuffer(device, b64bytes(dab.stroke), BufferUsage.STORAGE)

    const entries = []
    const add = (n, resource) => {
      if (has(n)) entries.push({binding: n, resource})
    }
    add(0, {buffer: coBuf})
    add(1, {buffer: noBuf})
    add(2, {buffer: maskBuf})
    add(3, {buffer: uniqueBuf})
    add(4, {buffer: nodesBuf})
    add(5, {buffer: brushUBuf})
    add(6, {buffer: ctxUBuf})
    add(7, {buffer: falloffBuf})
    add(8, {buffer: texBuf})
    if (has(9)) entries.push({binding: 9, resource: sampler})
    add(10, {buffer: strokeBuf})
    add(11, {buffer: coPrevBuf})
    add(12, {buffer: nbrMetaBuf})
    add(13, {buffer: nbrVertsBuf})
    add(23, {buffer: dabStampBuf})
    add(24, {buffer: automaskBuf})
    add(25, {buffer: dispBuf})
    if (attrBuf) entries.push({binding: attrSlot, resource: {buffer: attrBuf}})
    const bindGroup = device.createBindGroup({layout: bgl, entries})

    const enc = device.createCommandEncoder()
    // for_neighbor kernels read the previous-dab snapshot from co_prev; the
    // native dispatcher copies co -> coPrev before each dab. Mirror that here.
    if (coPrevBuf) enc.copyBufferToBuffer(coBuf, 0, coPrevBuf, 0, vc * 16)
    const pass = enc.beginComputePass()
    pass.setPipeline(pipeline)
    pass.setBindGroup(0, bindGroup)
    pass.dispatchWorkgroups(dab.nodeCount)
    pass.end()
    device.queue.submit([enc.finish()])
  }

  // Read co (and mask, for mask kernels) back and diff against the reference.
  async function readback(buf, byteLen) {
    const staging = device.createBuffer({size: byteLen, usage: BufferUsage.MAP_READ | BufferUsage.COPY_DST})
    const enc = device.createCommandEncoder()
    enc.copyBufferToBuffer(buf, 0, staging, 0, byteLen)
    device.queue.submit([enc.finish()])
    await staging.mapAsync(MapMode.READ)
    const copy = Buffer.from(new Uint8Array(staging.getMappedRange()))
    staging.unmap()
    return copy
  }

  // Face/attr-output kernels (polygroup) don't move geometry — they carry no
  // expectCo, only expectAttr (checked below). Geometry kernels diff co.
  let coDiff = {maxAbs: 0, bad: 0, badIdx: -1}
  if (fx.expectCo) {
    const coBack = await readback(coBuf, vc * 16)
    const actualCo = packVec3(coBack, vc)
    const expectCo = new Float32Array(b64bytes(fx.expectCo).buffer.slice(0), 0, vc * 3)
    coDiff = diff(actualCo, expectCo)
  }

  let maskDiff = null
  if (fx.writesMask) {
    const maskBack = await readback(maskBuf, vc * 4)
    const actualMask = new Float32Array(maskBack.buffer, maskBack.byteOffset, vc)
    const eb = b64bytes(fx.expectMask)
    const expectMask = new Float32Array(eb.buffer, eb.byteOffset, vc)
    maskDiff = diff(actualMask, expectMask)
  }

  // Attr-output kernels (e.g. polygroup's int group) carry expectAttr: read the
  // slot back and compare. Int layers compare exactly; float layers (color) use
  // the fp tolerance. This is the actual GPU correctness check for face kernels.
  let attrDiff = null
  if (attrBuf && fx.expectAttr) {
    const back = await readback(attrBuf, vc * attrElemSize)
    const exp = b64bytes(fx.expectAttr)
    if (attrElemSize === 4 && /i32|u32/.test(bindings.find((b) => b.binding === attrSlot).type)) {
      const a = new Int32Array(back.buffer, back.byteOffset, vc)
      const e = new Int32Array(exp.buffer, exp.byteOffset, vc)
      attrDiff = diffInt(a, e)
    } else {
      const n = (vc * attrElemSize) / 4
      const a = new Float32Array(back.buffer, back.byteOffset, n)
      const e = new Float32Array(exp.buffer, exp.byteOffset, n)
      attrDiff = diff(a, e)
    }
  }

  const ok = coDiff.bad === 0 && (!maskDiff || maskDiff.bad === 0) && (!attrDiff || attrDiff.bad === 0)
  return {ok, kernel: fx.kernel, vertCount: vc, dabs: fx.dabs.length, coDiff, maskDiff, attrDiff}
}

async function main() {
  const args = process.argv.slice(2)
  let wgslDir = null
  let fixture = null
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--wgsl-dir') wgslDir = args[++i]
    else if (args[i] === '--fixture') fixture = args[++i]
  }
  if (!wgslDir || !fixture) {
    console.error('usage: replay.mjs --wgsl-dir DIR --fixture FILE.json')
    process.exit(2)
  }
  const r = await replayFixture(fixture, wgslDir)
  const tag = `${r.kernel} (${r.vertCount} verts, ${r.dabs} dabs)`
  if (r.ok) {
    console.log(`PASS ${tag}  maxAbsErr=${r.coDiff.maxAbs.toExponential(2)}`)
    process.exit(0)
  }
  console.error(`FAIL ${tag}`)
  console.error(
    `  co:   ${r.coDiff.bad} bad, maxAbsErr=${r.coDiff.maxAbs.toExponential(2)} (first at idx ${r.coDiff.badIdx})`
  )
  if (r.maskDiff) console.error(`  mask: ${r.maskDiff.bad} bad, maxAbsErr=${r.maskDiff.maxAbs.toExponential(2)}`)
  if (r.attrDiff)
    console.error(
      `  attr: ${r.attrDiff.bad} bad, maxAbsErr=${r.attrDiff.maxAbs.toExponential(2)} (first at idx ${r.attrDiff.badIdx})`
    )
  process.exit(1)
}

// Run main() when invoked as a CLI. Compare via pathToFileURL so this matches
// on Windows too (raw `file://${argv[1]}` keeps backslashes + drops the third
// slash, so it never equals import.meta.url's file:///C:/… form → main() never
// ran and the harness saw an empty result).
if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  main()
}
