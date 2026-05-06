import {ipcRenderer} from 'electron'
import {decodeF32, decodeI32, decodeI16, decodeU8} from './decode'
import type {MeshLog, MeshSnapshot, MeshSource} from './MeshSource'

interface RawSnapshot {
  caps: MeshSnapshot['caps']
  counts: MeshSnapshot['counts']
  v: {co: string; no: string; e: string; alive: string}
  e: {vs: string; c: string; disk: string; alive: string}
  c: {
    v: string
    e: string
    l: string
    next: string
    prev: string
    radial_next: string
    radial_prev: string
    alive: string
  }
  l: {c: string; f: string; next: string; size: string; alive: string}
  f: {l: string; no: string; list_count: string; alive: string}
}

interface RawLog {
  version: number
  tag: string
  initial: RawSnapshot
  steps: {
    op: string
    highlight?: {kind: 'v' | 'e' | 'f' | 'c'; ids: number[]}
    note?: string
    snapshot: RawSnapshot
  }[]
}

const decodeSnap = (s: RawSnapshot): MeshSnapshot => ({
  caps  : s.caps,
  counts: s.counts,
  v: {
    co   : decodeF32(s.v.co),
    no   : decodeF32(s.v.no),
    e    : decodeI32(s.v.e),
    alive: decodeU8(s.v.alive),
  },
  e: {
    vs   : decodeI32(s.e.vs),
    c    : decodeI32(s.e.c),
    disk : decodeI32(s.e.disk),
    alive: decodeU8(s.e.alive),
  },
  c: {
    v          : decodeI32(s.c.v),
    e          : decodeI32(s.c.e),
    l          : decodeI32(s.c.l),
    next       : decodeI32(s.c.next),
    prev       : decodeI32(s.c.prev),
    radial_next: decodeI32(s.c.radial_next),
    radial_prev: decodeI32(s.c.radial_prev),
    alive      : decodeU8(s.c.alive),
  },
  l: {
    c    : decodeI32(s.l.c),
    f    : decodeI32(s.l.f),
    next : decodeI32(s.l.next),
    size : decodeI32(s.l.size),
    alive: decodeU8(s.l.alive),
  },
  f: {
    l         : decodeI32(s.f.l),
    no        : decodeF32(s.f.no),
    list_count: decodeI16(s.f.list_count),
    alive     : decodeU8(s.f.alive),
  },
})

export class JsonFileSource implements MeshSource {
  constructor(public dir: string) {}

  async listLogs(): Promise<string[]> {
    return await ipcRenderer.invoke('list-logs', this.dir)
  }

  async loadLog(name: string): Promise<MeshLog> {
    const text = (await ipcRenderer.invoke('read-log', this.dir, name)) as string
    const raw = JSON.parse(text) as RawLog
    return {
      version: raw.version,
      tag    : raw.tag,
      initial: decodeSnap(raw.initial),
      steps: raw.steps.map((s) => ({
        op       : s.op,
        highlight: s.highlight,
        note     : s.note,
        snapshot : decodeSnap(s.snapshot),
      })),
    }
  }
}

export const pickLogDir = async (): Promise<string | null> => {
  return await ipcRenderer.invoke('pick-log-dir')
}
