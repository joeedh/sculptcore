/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {Mesh} from '../mesh/Mesh'
import type {SpatialTree} from '../spatial/SpatialTree'
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

export interface MeshLog {
  [Symbol.dispose](): void
  undo(m: Mesh, tree: SpatialTree): void
  redo(m: Mesh, tree: SpatialTree): void
  curStrokeId(): int32
  lastStepId(): int32
  stepMemSize(id: int32): double
  totalMemSize(): double
  entryCount(): int32
  freeStep(id: int32): int32
  hasTopoChunk(): boolean
  reorderForLocality(tree: SpatialTree): void
  extrudeRegion(m: Mesh, outNormal: float[]): void
  extrudeIndividual(m: Mesh, outNormal: float[]): void
  extrudeWireVerts(m: Mesh, outNormal: float[]): void
  insetRegion(
    m: Mesh,
    insetVerts: int32[],
    baseCo: float[],
    tangent: float[]
  ): void
  selectionBeginStep(): void
  selectionEndStep(): void
  selectOne(m: Mesh, domain: int32, idx: int32, state: boolean): void
  selectIndices(m: Mesh, domain: int32, indices: int32[], state: int32): void
  selectAllElems(m: Mesh, domain: int32, state: int32): void
  selectShortestPath(m: Mesh, vEnd: int32, state: int32): int32
  selectScreenCircle(
    m: Mesh,
    tree: SpatialTree,
    co: float3,
    ray: float3,
    r1: float,
    r2: float,
    domain: int32,
    state: int32
  ): void
  selectScreenRect(
    m: Mesh,
    tree: SpatialTree,
    near0: float3,
    near1: float3,
    near2: float3,
    near3: float3,
    far0: float3,
    far1: float3,
    far2: float3,
    far3: float3,
    domain: int32,
    state: int32
  ): void
  setActiveElem(domain: int32, idx: int32): void
  activeVert(): int32
  activeEdge(): int32
  activeFace(): int32
  new (): MeshLog
}
