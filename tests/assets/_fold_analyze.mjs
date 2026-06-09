// TEMP: analyze pre-pass diagnostic OBJs for folds/degeneracies. Delete with the
// _diag_*.obj artifacts once the shred is fixed.
import fs from 'node:fs'

function load(path) {
  const V = [], F = []
  for (const line of fs.readFileSync(path, 'utf8').split('\n')) {
    if (line.startsWith('v ')) {
      const [, x, y, z] = line.split(/\s+/)
      V.push([+x, +y, +z])
    } else if (line.startsWith('f ')) {
      const idx = line.trim().split(/\s+/).slice(1).map((t) => parseInt(t.split('/')[0], 10) - 1)
      F.push(idx)
    }
  }
  return { V, F }
}
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
const len = (a) => Math.hypot(a[0], a[1], a[2])

function analyze(tag, path) {
  const { V, F } = load(path)
  let nDegen = 0, minArea = Infinity
  const nrm = [], area = []
  for (const f of F) {
    const n = cross(sub(V[f[1]], V[f[0]]), sub(V[f[2]], V[f[0]]))
    const a = 0.5 * len(n)
    area.push(a)
    nrm.push(a > 1e-12 ? [n[0] / (2 * a), n[1] / (2 * a), n[2] / (2 * a)] : [0, 0, 0])
    if (a < 1e-9) nDegen++
    if (a < minArea) minArea = a
  }
  // edge -> incident face list (triangles only, but handle any arity by edge pairs)
  const em = new Map()
  F.forEach((f, fi) => {
    for (let i = 0; i < f.length; i++) {
      const a = f[i], b = f[(i + 1) % f.length]
      const key = a < b ? a + '_' + b : b + '_' + a
      if (!em.has(key)) em.set(key, [])
      em.get(key).push(fi)
    }
  })
  let nManifold = 0, nBoundary = 0, nNonman = 0
  let fold90 = 0, fold120 = 0, fold150 = 0
  let sumDev = 0
  for (const faces of em.values()) {
    if (faces.length === 1) { nBoundary++; continue }
    if (faces.length !== 2) { nNonman++; continue }
    nManifold++
    const d = Math.max(-1, Math.min(1, dot(nrm[faces[0]], nrm[faces[1]])))
    const devDeg = (Math.acos(d) * 180) / Math.PI // 0 = coplanar, 180 = folded back
    sumDev += devDeg
    if (devDeg > 90) fold90++
    if (devDeg > 120) fold120++
    if (devDeg > 150) fold150++
  }
  const pct = (n) => ((100 * n) / nManifold).toFixed(2)
  console.log(
    `${tag.padEnd(14)} V=${V.length} F=${F.length}  degenF=${nDegen} minArea=${minArea.toExponential(2)}\n` +
    `   edges: manifold=${nManifold} boundary=${nBoundary} nonman=${nNonman}\n` +
    `   normal-dev: mean=${(sumDev / nManifold).toFixed(1)}deg  >90=${fold90}(${pct(fold90)}%) >120=${fold120}(${pct(fold120)}%) >150=${fold150}(${pct(fold150)}%)`,
  )
}

const dir = new URL('.', import.meta.url).pathname.replace(/^\//, '')
for (const tag of ['input','A_bk_only','C_field','D_field_repr','E_step_repr','F_step_sm2']) {
  analyze(tag, dir + '_diag_' + tag + '.obj')
}
