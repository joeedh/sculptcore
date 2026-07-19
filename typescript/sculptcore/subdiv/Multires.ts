/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */

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

export interface Multires {
  [Symbol.dispose](): void
  maxLevel(): int32
  activeLevel(): int32
  addLevel(): int32
  removeTopLevel(): int32
  setStoreBudget(bytes: int32): void
  layerAdd(): int32
  layerRemove(li: int32): void
  layerSetWeight(li: int32, weight: float): void
  layerSetEnabled(li: int32, enabled: int32): void
  layerSetFrozen(li: int32, frozen: int32): void
  setEditTarget(li: int32): int32
  editTarget(): int32
  layerCount(): int32
  layerWeight(li: int32): float
  layerEnabled(li: int32): int32
  layerFrozen(li: int32): int32
  layerTableOut(out: float[]): void
  layerTableRestore(table: float[]): void
  vdmAdjacencyOut(out: int32[]): void
  stencilMetaOut(level: int32, out: int32[]): void
  stencilOffsetsOut(level: int32, out: int32[]): void
  stencilIndicesOut(level: int32, out: int32[]): void
  stencilWeightsOut(level: int32, out: float[]): void
  levelTriIndicesOut(level: int32, out: int32[]): void
  levelVertGridCoordsOut(level: int32, out: int32[]): void
  levelGridVertsOut(level: int32, out: int32[]): void
}
