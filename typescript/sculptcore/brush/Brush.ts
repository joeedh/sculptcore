/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {StructProp} from '../props/StructProp'

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

export interface Brush {
  [Symbol.dispose](): void
  strength: float
  radius: float
  invert: boolean
  props: StructProp
  loadProps(): void
  new (): Brush
}
