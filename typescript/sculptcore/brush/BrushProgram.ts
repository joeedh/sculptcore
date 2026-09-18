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

export interface BrushProgram {
  [Symbol.dispose](): void
  clear(): void
  addCommand(type: int32): int32
  setCommandFloat(idx: int32, propId: int32, v: float): void
  setCommandFloatByName(idx: int32, name: string, v: float): void
  replaceCommandResponseDynamicsChecked(
    idx: int32,
    name: string,
    scalarType: int32,
    devices: int32[],
    modes: int32[],
    factors: float[],
    enabled: int32[],
    offsets: int32[],
    samples: float[],
    kinds: int32[],
    parameters: double[]
  ): int32
  removeCommandDynamicsChecked(
    idx: int32,
    name: string,
    scalarType: int32
  ): int32
  replaceCommandCavityCurveChecked(idx: int32, samples: float[]): int32
  removeCommandCavityCurveChecked(idx: int32): int32
  setCommandInvert(idx: int32, inv: boolean): void
  setCommandAttrLayer(idx: int32, attrIdx: int32, layerIndex: int32): void
  new (): BrushProgram
}
