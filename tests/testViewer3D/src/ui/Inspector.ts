import type {MeshSnapshot, ElemKind} from '../data/MeshSource'

export type SelectionListener = (sel: {kind: ElemKind; id: number}) => void

const ELEM_NONE = -1

export class Inspector {
  private root: HTMLElement
  private onSelect: SelectionListener

  constructor(root: HTMLElement, onSelect: SelectionListener) {
    this.root = root
    this.onSelect = onSelect
  }

  render(snap: MeshSnapshot, sel: {kind: ElemKind; id: number} | null): void {
    this.root.innerHTML = ''
    const summary = document.createElement('div')
    summary.innerHTML =
      `<h3>Mesh</h3>` +
      `<div class='row'><span>verts</span><span>${snap.counts.v} / ${snap.caps.v}</span></div>` +
      `<div class='row'><span>edges</span><span>${snap.counts.e} / ${snap.caps.e}</span></div>` +
      `<div class='row'><span>corners</span><span>${snap.counts.c} / ${snap.caps.c}</span></div>` +
      `<div class='row'><span>faces</span><span>${snap.counts.f} / ${snap.caps.f}</span></div>`
    this.root.appendChild(summary)

    if (!sel) {
      const hint = document.createElement('div')
      hint.style.color = '#666'
      hint.style.marginTop = '8px'
      hint.textContent = 'Click a vertex or edge to inspect'
      this.root.appendChild(hint)
      return
    }

    const block = document.createElement('div')
    if (sel.kind === 'v') {
      const vi = sel.id
      const co = snap.v.co
      block.innerHTML =
        `<h3>vert ${vi}</h3>` +
        this.row('co', `${co[vi * 3].toFixed(3)}, ${co[vi * 3 + 1].toFixed(3)}, ${co[vi * 3 + 2].toFixed(3)}`) +
        this.rowLink('e (disk start)', 'e', snap.v.e[vi])
      const neighbors = this.diskNeighbors(snap, vi)
      block.innerHTML += `<h3>disk (${neighbors.length})</h3>`
      for (const n of neighbors) {
        block.innerHTML += this.rowLink(`e${n.e}`, 'v', n.other)
      }
    } else if (sel.kind === 'e') {
      const ei = sel.id
      const v0 = snap.e.vs[ei * 2]
      const v1 = snap.e.vs[ei * 2 + 1]
      block.innerHTML =
        `<h3>edge ${ei}</h3>` +
        this.rowLink('v0', 'v', v0) +
        this.rowLink('v1', 'v', v1) +
        this.rowLink('c (radial start)', 'c', snap.e.c[ei])
      const radial = this.radialFaces(snap, ei)
      block.innerHTML += `<h3>radial (${radial.length})</h3>`
      for (const r of radial) {
        block.innerHTML += this.rowLink(`c${r.c} → f${r.f} (l${r.l})`, 'f', r.f)
      }
    } else if (sel.kind === 'f') {
      const fi = sel.id
      block.innerHTML =
        `<h3>face ${fi}</h3>` +
        this.rowLink('l', 'c', snap.f.l[fi]) +
        this.row('list_count', String(snap.f.list_count[fi]))
      const corners = this.faceCorners(snap, fi)
      block.innerHTML += `<h3>corners (${corners.length})</h3>`
      for (const c of corners) {
        block.innerHTML += this.rowLink(`c${c.c}`, 'v', c.v)
      }
    } else {
      const ci = sel.id
      block.innerHTML =
        `<h3>corner ${ci}</h3>` +
        this.rowLink('v', 'v', snap.c.v[ci]) +
        this.rowLink('e', 'e', snap.c.e[ci]) +
        this.rowLink('l', 'c', snap.c.l[ci]) +
        this.rowLink('next', 'c', snap.c.next[ci]) +
        this.rowLink('prev', 'c', snap.c.prev[ci]) +
        this.rowLink('radial_next', 'c', snap.c.radial_next[ci]) +
        this.rowLink('radial_prev', 'c', snap.c.radial_prev[ci])
    }

    this.root.appendChild(block)

    block.querySelectorAll('a.elref').forEach((a) => {
      a.addEventListener('click', () => {
        const k = a.getAttribute('data-kind') as ElemKind
        const id = parseInt(a.getAttribute('data-id') ?? '-1', 10)
        if (id !== ELEM_NONE && id >= 0) {
          this.onSelect({kind: k, id})
        }
      })
    })
  }

  private row(k: string, v: string): string {
    return `<div class='row'><span>${k}</span><span>${v}</span></div>`
  }

  private rowLink(k: string, kind: ElemKind, id: number): string {
    if (id === ELEM_NONE) {
      return this.row(k, '—')
    }
    return `<div class='row'><span>${k}</span><a class='elref' data-kind='${kind}' data-id='${id}'>${kind}${id}</a></div>`
  }

  private diskNeighbors(snap: MeshSnapshot, vi: number): {e: number; other: number}[] {
    const out: {e: number; other: number}[] = []
    const e0 = snap.v.e[vi]
    if (e0 === ELEM_NONE) {
      return out
    }
    let ec = e0
    let safety = 0
    do {
      const v0 = snap.e.vs[ec * 2]
      const v1 = snap.e.vs[ec * 2 + 1]
      const side = v0 === vi ? 0 : 1
      out.push({e: ec, other: side === 0 ? v1 : v0})
      ec = snap.e.disk[ec * 4 + side * 2 + 1]
      if (++safety > 4096) {
        break
      }
    } while (ec !== e0)
    return out
  }

  private radialFaces(snap: MeshSnapshot, ei: number): {c: number; l: number; f: number}[] {
    const out: {c: number; l: number; f: number}[] = []
    const c0 = snap.e.c[ei]
    if (c0 === ELEM_NONE) {
      return out
    }
    let cc = c0
    let safety = 0
    do {
      const li = snap.c.l[cc]
      out.push({c: cc, l: li, f: snap.l.f[li]})
      cc = snap.c.radial_next[cc]
      if (++safety > 4096) {
        break
      }
    } while (cc !== c0)
    return out
  }

  private faceCorners(snap: MeshSnapshot, fi: number): {c: number; v: number}[] {
    const out: {c: number; v: number}[] = []
    const li = snap.f.l[fi]
    if (li === ELEM_NONE) {
      return out
    }
    const c0 = snap.l.c[li]
    let cc = c0
    let safety = 0
    do {
      out.push({c: cc, v: snap.c.v[cc]})
      cc = snap.c.next[cc]
      if (++safety > 4096) {
        break
      }
    } while (cc !== c0)
    return out
  }
}
