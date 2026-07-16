/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {DynTopoMode} from './DynTopoMode'

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

export interface DynTopoParams {
  [Symbol.dispose](): void
  l_max: float
  l_min: float
  mode: DynTopoMode
  grade: float
  max_rounds: int32
  do_flips: boolean
  max_splits: int32
  max_collapses: int32
  do_smooth: boolean
  smooth_lambda: float
  preserve_features: boolean
  new (): DynTopoParams
  new (b: DynTopoParams): DynTopoParams
}
