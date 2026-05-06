export type ElemKind = 'v' | 'e' | 'f' | 'c'

export interface MeshSnapshot {
  caps: {v: number; e: number; c: number; l: number; f: number}
  counts: {v: number; e: number; c: number; l: number; f: number}
  v: {co: Float32Array; no: Float32Array; e: Int32Array; alive: Uint8Array}
  e: {vs: Int32Array; c: Int32Array; disk: Int32Array; alive: Uint8Array}
  c: {
    v: Int32Array
    e: Int32Array
    l: Int32Array
    next: Int32Array
    prev: Int32Array
    radial_next: Int32Array
    radial_prev: Int32Array
    alive: Uint8Array
  }
  l: {
    c: Int32Array
    f: Int32Array
    next: Int32Array
    size: Int32Array
    alive: Uint8Array
  }
  f: {
    l: Int32Array
    no: Float32Array
    list_count: Int16Array
    alive: Uint8Array
  }
}

export interface MeshLogStep {
  op: string
  highlight?: {kind: ElemKind; ids: number[]}
  note?: string
  snapshot: MeshSnapshot
}

export interface MeshLog {
  version: number
  tag: string
  initial: MeshSnapshot
  steps: MeshLogStep[]
}

export interface MeshSource {
  listLogs(): Promise<string[]>
  loadLog(name: string): Promise<MeshLog>
}
