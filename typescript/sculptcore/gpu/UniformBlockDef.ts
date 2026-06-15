/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {float2} from '../../litestl/math/float2'
import type {UniformBindType} from './UniformBindType'
import type {UniformDef} from './UniformDef'
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

export interface UniformBlockDef {
  [Symbol.dispose](): void
  name: string
  fields: (
    | UniformDef<float, UniformBindType.FLOAT>
    | UniformDef<float2, UniformBindType.FLOAT2>
    | UniformDef<float3, UniformBindType.FLOAT3>
  )[]
  set: uint32
  binding: uint32
  packedBytes: uint32
  preferPushConstant: boolean
}
