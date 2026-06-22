/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {Mesh} from '../mesh/Mesh'
import type {SpatialTree} from '../spatial/SpatialTree'

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
  compactIfFragmented(tree: SpatialTree, vertRatioThreshold: double): boolean
  new (): MeshLog
}
