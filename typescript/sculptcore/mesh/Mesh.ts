/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {EdgeData} from './EdgeData'
import type {CornerData} from './CornerData'
import type {VertexData} from './VertexData'
import type {FaceData} from './FaceData'

/** Auto-generated file */
/* eslint-disable @typescript-eslint/no-misused-new */
/* eslint-disable @typescript-eslint/no-unused-vars */

type pointer<T = any> = number
type int8 = number
type uint8 = number
type int16 = number
type uint16 = number
type int32 = number
type uint32 = number
type int64 = number
type uint64 = number
type float = number
type double = number

export interface Mesh {
  [Symbol.dispose](): void
  v: VertexData
  e: EdgeData
  c: CornerData
  f: FaceData
  recalc_normals(): void
  faceGroup(face: int32): int32
  maxFaceGroup(): int32
  setAttrUse(domain: int32, index: int32, use: int32): void
  addAttr(domain: int32, type: int32, use: int32): int32
  removeAttr(domain: int32, index: int32): void
  detachAttr(domain: int32, index: int32): int32
  reattachAttr(stashId: int32): int32
  markSeamPath(vStart: int32, vEnd: int32, state: int32): int32
  edgePathEdges(vStart: int32, vEnd: int32, out: int32[]): void
  edgeSeam(e: int32): int32
  setEdgeSeam(e: int32, state: int32): void
  recomputeBoundary(): void
  edgePathCoords(vStart: int32, vEnd: int32, out: float[]): void
  generateUVFromSeams(marginMilli: int32): int32
  new (): Mesh
}
