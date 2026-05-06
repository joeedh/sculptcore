import type {MeshSnapshot, ElemKind} from '../data/MeshSource'
import type {Mat4} from '../gl/mat4'

const project = (vp: Mat4, x: number, y: number, z: number): [number, number, number] => {
  const cx = vp[0] * x + vp[4] * y + vp[8] * z + vp[12]
  const cy = vp[1] * x + vp[5] * y + vp[9] * z + vp[13]
  const cz = vp[2] * x + vp[6] * y + vp[10] * z + vp[14]
  const cw = vp[3] * x + vp[7] * y + vp[11] * z + vp[15]
  if (cw <= 0) {
    return [Number.NaN, Number.NaN, 1]
  }
  return [cx / cw, cy / cw, cz / cw]
}

export const pickElement = (
  snap: MeshSnapshot,
  vp: Mat4,
  ndcX: number,
  ndcY: number,
  pixelToleranceNdc: number
): {kind: ElemKind; id: number} | null => {
  let best: {kind: ElemKind; id: number; dist: number; depth: number} | null = null

  const vAlive = snap.v.alive
  const co = snap.v.co
  const vProj: ([number, number, number] | null)[] = new Array(snap.caps.v).fill(null)

  const vertTol = pixelToleranceNdc * 1.5
  for (let vi = 0; vi < snap.caps.v; vi++) {
    if (!vAlive[vi]) {
      continue
    }
    const p = project(vp, co[vi * 3], co[vi * 3 + 1], co[vi * 3 + 2])
    vProj[vi] = p
    const dx = p[0] - ndcX
    const dy = p[1] - ndcY
    const d = Math.hypot(dx, dy)
    if (d < vertTol && (best === null || d < best.dist - 0.005 || (d < best.dist && p[2] < best.depth))) {
      best = {kind: 'v', id: vi, dist: d, depth: p[2]}
    }
  }

  const eAlive = snap.e.alive
  const evs = snap.e.vs
  for (let ei = 0; ei < snap.caps.e; ei++) {
    if (!eAlive[ei]) {
      continue
    }
    const v0 = evs[ei * 2]
    const v1 = evs[ei * 2 + 1]
    const p0 = vProj[v0]
    const p1 = vProj[v1]
    if (!p0 || !p1) {
      continue
    }
    const ax = p0[0]
    const ay = p0[1]
    const bx = p1[0]
    const by = p1[1]
    const dx = bx - ax
    const dy = by - ay
    const len2 = dx * dx + dy * dy
    if (len2 < 1e-12) {
      continue
    }
    let t = ((ndcX - ax) * dx + (ndcY - ay) * dy) / len2
    if (t < 0) t = 0
    if (t > 1) t = 1
    const cx = ax + dx * t
    const cy = ay + dy * t
    const d = Math.hypot(ndcX - cx, ndcY - cy)
    const depth = p0[2] + (p1[2] - p0[2]) * t
    if (d < pixelToleranceNdc && (best === null || d < best.dist - 0.005 || (d < best.dist && depth < best.depth))) {
      best = {kind: 'e', id: ei, dist: d, depth}
    }
  }

  if (!best) {
    return null
  }
  return {kind: best.kind, id: best.id}
}
