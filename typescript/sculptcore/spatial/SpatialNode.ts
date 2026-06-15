/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {float3} from '../../litestl/math/float3'
import type {AABB} from '../../litestl/math/AABB'
import type {NodeFlags} from './NodeFlags'

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

export interface SpatialNode {
  [Symbol.dispose](): void
  aabb: AABB<float3>
  flag: NodeFlags
  id: int32
  debugIdOffset: int32
}
