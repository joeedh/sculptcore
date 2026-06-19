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
      if (arr) {
        pick = {name, info, arrayMember: arr.name}
        break
      }
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
        allOk         : after.every((v, i) => v === expected[i]),
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

    // [Symbol.dispose]() on a bound owning instance: present, callable, and
    // idempotent (a second call is a safe no-op — the pointer was nulled).
    // Mirrors the WASM bound-class dispose (destructor + free). Use a struct
    // with a real destructor (MeshLog owns Vectors) when available.
    try {
      let firstCtor = null
      for (const name of ['sculptcore::meshlog::MeshLog']) {
        const info = addon.structInfo(name)
        if (info && info.hasDefaultCtor) {
          firstCtor = name
          break
        }
      }
      if (!firstCtor) {
        for (const name of names) {
          const info = addon.structInfo(name)
          if (info && info.hasDefaultCtor) {
            firstCtor = name
            break
          }
        }
      }
      if (firstCtor) {
        const obj = addon.construct(firstCtor)
        const hasDispose = typeof obj[Symbol.dispose] === 'function'
        let threw = false
        try {
          obj[Symbol.dispose]() // destruct + free
          obj[Symbol.dispose]() // idempotent: no double-free / crash
        } catch (e) {
          threw = true
        }
        result.dispose = {struct: firstCtor, hasDispose, idempotent: !threw}
      }
    } catch (e) {
      result.dispose = {error: String((e && e.stack) || e)}
    }

    // Native factory + free lifecycle (no leak / no crash on free).
    try {
      const mesh = addon.meshCreateCube(8, 1, 1)
      const cap = mesh.v && typeof mesh.v === 'object' ? mesh.v.capacity_ : undefined
      const tree = addon.meshBuildSpatialTree(mesh, 0, 0, 0)
      // One GPUManager shared by both the bulk-data read and the sculpt-stroke
      // verification: the tree fills buffers for the manager that owns the
      // update, so re-updating after a dab (with the SAME manager) regenerates
      // only the dirtied nodes' buffers — that's what makes the checksum move.
      const gpu = addon.construct('sculptcore::gpu::GPUManager')

      // Native GPU bulk-data path: run the real frontend pipeline
      // (tree.update(gpu) allocates + fills host vertex buffers C++-side), then
      // read a buffer's bytes via pointerBytes + identity via objectAddress —
      // exactly what gpuExecutor needs on the native backend.
      try {
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
            name : typeof buf.name === 'string' ? buf.name : undefined,
            size,
            elemsize,
            requestedBytes: bytes,
            viewIsUint8   : view instanceof Uint8Array,
            viewLength    : view ? view.length : 0,
            viewLenOk     : !!view && view.length === bytes,
            nonzeroBytes  : nonzero,
            addr,
            addrStable: typeof addr === 'number' && addr !== 0 && addr === addr2,
          }
          break
        }
        result.gpuBulkData = {updated, bufferCount: nbuf, sample: bufInfo}
      } catch (e) {
        result.gpuBulkData = {error: String((e && e.stack) || e)}
      }

      // Native sculpt-stroke primitives: constructWith (parameterized ctor with
      // pointer args), enum + Vector* + by-value-float3 method args (execBrush),
      // pointer-member set (exec.meshLog), and makeNodeVector (the filterNodes
      // out-param). Proven by a DRAW dab actually moving a vertex.
      try {
        // A native abort() (e.g. in a brush kernel) bypasses JS try/catch and
        // kills the process before the final write — so flush a stage marker to
        // disk after each risky call to pinpoint where a crash lands.
        const stage = (s) => {
          result.sculptStage = s
          fs.writeFileSync(path.join(__dirname, 'smoke-result.json'), JSON.stringify(result, null, 2))
        }
        const f3 = (x, y, z) => {
          const v = addon.construct('litestl::math::float3')
          v.vec[0] = x
          v.vec[1] = y
          v.vec[2] = z
          return v
        }
        // Checksum of every populated GPU buffer's bytes — a vertex move shows up
        // here once the dirtied nodes' buffers are regenerated by tree.update
        // (reusing the same `gpu` manager from the bulk-data block above).
        const bufferChecksum = () => {
          tree.update(gpu)
          const buffers = gpu.buffers
          const nbuf = addon.vectorLength(buffers) | 0
          let sum = 0
          for (let i = 0; i < nbuf; i++) {
            const buf = addon.vectorGet(buffers, i)
            const size = buf && buf.size | 0
            const elemsize = buf && buf.elemsize | 0
            if (!size || !elemsize) continue
            const view = addon.pointerBytes(buf, 'data', size * elemsize * 4)
            if (view) for (let k = 0; k < view.length; k++) sum = (sum + view[k] * (k + 1)) >>> 0
          }
          return sum
        }

        stage('start')
        const brush = addon.construct('sculptcore::brush::Brush')
        brush.strength = 1.0
        brush.radius = 2.0
        brush.invert = false
        if (typeof brush.writeProps === 'function') brush.writeProps()
        stage('brush')

        const meshLog = addon.construct('sculptcore::meshlog::MeshLog')
        meshLog.beginStep(false)
        stage('meshLog')

        const exec = addon.constructWith('sculptcore::brush::CommandExecutor', 'main', tree, brush)
        stage('constructWith')
        exec.meshLog = meshLog
        const meshLogBound = exec.meshLog !== undefined && exec.meshLog !== null
        stage('meshLogSet')

        const checksumBefore = bufferChecksum()
        stage('checksumBefore')

        // Dab centered at a cube corner; radius 2 covers the unit cube.
        const center = f3(0.5, 0.5, 0.5)
        const nodes = addon.makeNodeVector()
        stage('makeNodeVector')
        const filtered = tree.filterNodes(center, 2.0, nodes)
        const nodeCount = addon.vectorLength(nodes) | 0
        stage('filterNodes:' + nodeCount)

        exec.execBrush(mesh.mesh, 0 /* DRAW */, nodes, f3(0.5, 0.5, 0.5), f3(0.577, 0.577, 0.577))
        stage('execBrush')
        meshLog.endStep()
        stage('endStep')

        // execBrush froze the mesh topology (DRAW needs no live links); thaw it
        // (recalc_normals auto-thaws) before walking the tree again to regen GPU
        // buffers, mirroring the app's post-stroke teardown.
        mesh.recalc_normals()
        stage('thaw')
        const checksumAfter = bufferChecksum()
        stage('checksumAfter')
        result.sculptStroke = {
          meshLogBound,
          filtered,
          nodeCount,
          checksumBefore,
          checksumAfter,
          geometryChanged: checksumBefore !== checksumAfter,
        }
      } catch (e) {
        result.sculptStroke = {error: String((e && e.stack) || e)}
      }

      addon.spatialTreeFree(tree)
      addon.meshFree(mesh)
      result.meshLifecycle = {createdCapacity: cap, freed: true}
    } catch (e) {
      result.meshLifecycle = {error: String((e && e.stack) || e)}
    }

    result.ok = true
  } catch (err) {
    result.error = String((err && err.stack) || err)
    result.ok = false
  }

  fs.writeFileSync(path.join(__dirname, 'smoke-result.json'), JSON.stringify(result, null, 2))
  app.quit()
})
