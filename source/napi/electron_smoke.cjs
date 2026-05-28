const {app} = require('electron')
const path = require('path')
const fs = require('fs')

const SCALARS = ['boolean', 'number']

app.whenReady().then(() => {
  const result = {}
  try {
    const addon = require(path.join(__dirname, '..', '..', 'build', 'native-node', 'sculptcore_node.node'))
    result.version = addon.version()
    result.bindingCount = addon.bindingCount()

    // Workstream B reflection surface: construct() returns a live wrapped
    // instance whose members are real JS accessors backed by the C++ object.
    const names = addon.structNames()
    result.structCount = names.length

    // Pick a constructible struct that has a numeric inline-array member — this
    // is what exercises the array-element accessor (the element[0] garbage bug).
    let pick = null
    for (const name of names) {
      const info = addon.structInfo(name)
      if (!info || !info.hasDefaultCtor) continue
      const arr = (info.members || []).find((m) => m.type === 'array')
      if (arr) { pick = {name, info, arrayMember: arr.name}; break }
    }
    result.arrayPick = pick && {name: pick.name, member: pick.arrayMember}

    if (pick) {
      const obj = addon.construct(pick.name)
      const arr = obj[pick.arrayMember]
      const len = arr.length | 0
      const before = []
      for (let i = 0; i < len; i++) before.push(arr[i])
      for (let i = 0; i < len; i++) arr[i] = (i + 1) * 1.5
      const after = []
      for (let i = 0; i < len; i++) after.push(arr[i])
      // The bug signature: element[0] reads back as garbage after a clean write.
      const expected = []
      for (let i = 0; i < len; i++) expected.push((i + 1) * 1.5)
      result.arrayRoundTrip = {
        member: pick.arrayMember,
        length: len,
        before,
        after,
        expected,
        firstElementOk: after.length > 0 && after[0] === expected[0],
        allOk: after.every((v, i) => v === expected[i]),
      }
    }

    // Also exercise a plain scalar member round-trip if one exists, on any
    // constructible struct.
    for (const name of names) {
      const info = addon.structInfo(name)
      if (!info || !info.hasDefaultCtor) continue
      const scalar = (info.members || []).find((m) => SCALARS.includes(m.type))
      if (!scalar) continue
      const obj = addon.construct(name)
      const wrote = scalar.type === 'boolean' ? true : 7
      const before = obj[scalar.name]
      obj[scalar.name] = wrote
      const after = obj[scalar.name]
      result.scalarRoundTrip = {struct: name, member: scalar.name, type: scalar.type, before, wrote, after}
      break
    }

    // Native factory + free lifecycle (no leak / no crash on free).
    try {
      const mesh = addon.meshCreateCube(8, 1, 1)
      const cap = (mesh.v && typeof mesh.v === 'object') ? mesh.v.capacity_ : undefined
      const tree = addon.meshBuildSpatialTree(mesh, 0, 0)

      // Native GPU bulk-data path: run the real frontend pipeline
      // (tree.update(gpu) allocates + fills host vertex buffers C++-side), then
      // read a buffer's bytes via pointerBytes + identity via objectAddress —
      // exactly what gpuExecutor needs on the native backend.
      try {
        const gpu = addon.construct('sculptcore::gpu::GPUManager')
        const updated = tree.update(gpu)
        const buffers = gpu.buffers
        const nbuf = addon.vectorLength(buffers) | 0
        let bufInfo = null
        for (let i = 0; i < nbuf; i++) {
          const buf = addon.vectorGet(buffers, i)
          if (!buf || typeof buf !== 'object') continue
          const size = buf.size | 0
          const elemsize = buf.elemsize | 0
          if (size <= 0 || elemsize <= 0) continue
          // Position/normal attrs are float3 (FLOAT32). Request that many bytes.
          const bytes = size * elemsize * 4
          const addr = addon.objectAddress(buf)
          const addr2 = addon.objectAddress(buf) // stability: same object -> same key
          const view = addon.pointerBytes(buf, 'data', bytes)
          let nonzero = 0
          if (view) for (let k = 0; k < view.length; k++) if (view[k] !== 0) nonzero++
          bufInfo = {
            index: i,
            name: typeof buf.name === 'string' ? buf.name : undefined,
            size,
            elemsize,
            requestedBytes: bytes,
            viewIsUint8: view instanceof Uint8Array,
            viewLength: view ? view.length : 0,
            viewLenOk: !!view && view.length === bytes,
            nonzeroBytes: nonzero,
            addr,
            addrStable: typeof addr === 'number' && addr !== 0 && addr === addr2,
          }
          break
        }
        result.gpuBulkData = {updated, bufferCount: nbuf, sample: bufInfo}
      } catch (e) {
        result.gpuBulkData = {error: String(e && e.stack || e)}
      }

      addon.spatialTreeFree(tree)
      addon.meshFree(mesh)
      result.meshLifecycle = {createdCapacity: cap, freed: true}
    } catch (e) {
      result.meshLifecycle = {error: String(e && e.stack || e)}
    }

    result.ok = true
  } catch (err) {
    result.error = String(err && err.stack || err)
    result.ok = false
  }

  fs.writeFileSync(path.join(__dirname, 'smoke-result.json'), JSON.stringify(result, null, 2))
  app.quit()
})
