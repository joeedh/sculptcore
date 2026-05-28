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

    result.ok = true
  } catch (err) {
    result.error = String(err && err.stack || err)
    result.ok = false
  }

  fs.writeFileSync(path.join(__dirname, 'smoke-result.json'), JSON.stringify(result, null, 2))
  app.quit()
})
