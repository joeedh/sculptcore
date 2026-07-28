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

export interface BrushDefFlags {
  [Symbol.dispose](): void
  needsCoPrev: boolean
  accumulable: boolean
  relaxesBase: boolean
  grabModeCapable: boolean
  unbounded: boolean
  incremental: boolean
  writesMask: boolean
  writesColor: boolean
  faceMode: boolean
  readsVclass: boolean
  new (): BrushDefFlags
}
