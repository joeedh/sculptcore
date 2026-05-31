/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {float2} from '../../litestl/math/float2'
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

export interface CastRayIsect {
  [Symbol.dispose](): void
  p: float3
  normal: float3
  t: float
  uv: float2
  triIndex: int32
  nodeIndex: int32
  faceIndex: int32
  new (): CastRayIsect
}
