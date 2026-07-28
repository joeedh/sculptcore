/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {float2} from '../../litestl/math/float2'
import type {float3} from '../../litestl/math/float3'
import type {float4} from '../../litestl/math/float4'

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

export interface DabSample {
  [Symbol.dispose](): void
  p: float4
  dp: float4
  screenP: float2
  dScreenP: float2
  strokeS: float
  dstrokeS: float
  isInterp: boolean
  angle: float
  futureAngle: float
  vec: float3
  color: float4
  viewvec: float3
  vieworigin: float3
  viewPlane: float3
  strength: float
  radius: float
  w: float
  invert: boolean
  pressure: float
  hit: boolean
  useAltBrush: boolean
  anchorVec: float3
  liveAngle: float
  tiltX: float
  tiltY: float
  twist: float
  hasCurve: boolean
  curve0: float3
  curve1: float3
  curve2: float3
  curve3: float3
  new (): DabSample
}
