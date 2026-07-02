/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {VertexData} from './VertexData'
import type {FaceData} from './FaceData'
import type {CornerData} from './CornerData'
import type {EdgeData} from './EdgeData'
import type {float3} from '../../litestl/math/float3'

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
  ngonFaceCount(): int32
  setAttrUse(domain: int32, index: int32, use: int32): void
  addAttr(domain: int32, type: int32, use: int32): int32
  removeAttr(domain: int32, index: int32): void
  detachAttr(domain: int32, index: int32): int32
  reattachAttr(stashId: int32): int32
  markSeamPath(vStart: int32, vEnd: int32, state: int32): int32
  edgePathEdges(vStart: int32, vEnd: int32, out: int32[]): void
  edgeSeam(e: int32): int32
  setEdgeSeam(e: int32, state: int32): void
  markEdgePath(vStart: int32, vEnd: int32, kind: int32, state: int32): int32
  edgeFlagKind(e: int32, kind: int32): int32
  setEdgeFlagKind(e: int32, kind: int32, state: int32): void
  markSharpByAngle(angle: float, state: int32): int32
  repairLogCount(): int32
  clearRepairLog(): void
  repairMesh(): int32
  featureVerts(kind: int32, outIdx: int32[], outCo: float[]): void
  recomputeBoundary(): void
  boundaryGraphStats(out: int32[]): void
  edgePathCoords(vStart: int32, vEnd: int32, out: float[]): void
  generateUVFromSeams(marginMilli: int32): int32
  markAllSeams(): void
  fillVertexColorFromPosition(): void
  vertexColor(vert: int32, out: float[]): void
  dumpVertCo(out: float[]): void
  setVertCo(idx: int32, x: float, y: float, z: float): void
  symmetrize(axis: int32, sign: int32, threshold: float): void
  selectedCount(domain: int32): int32
  selectedElems(domain: int32, out: int32[]): void
  gatherVertCos(idx: int32[], out: float[]): void
  selectionBoundaryEdges(out: int32[]): void
  movableVerts(out: int32[]): void
  edgeRing(e: int32, out: int32[]): void
  faceLoop(e: int32, out: int32[]): void
  edgeLoop(e: int32, out: int32[]): void
  faceEdgeNearest(f: int32, p: float3): int32
  loopCutPreviewCoords(seedEdge: int32, out: float[]): void
  calcAABB(minOut: float3, maxOut: float3): void
  new (): Mesh
}
