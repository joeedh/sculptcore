/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {AttrType} from './AttrType'
import type {AttrData} from './AttrData'
import type {AttrUse} from './AttrUse'
import type {AttrFlag} from './AttrFlag'

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

export interface AttrRef {
  [Symbol.dispose](): void
  name: string
  type: AttrType
  flag: AttrFlag
  use: AttrUse
  data: AttrData<float> | AttrData<int32> | AttrData<uint8> | AttrData<int16>
}
