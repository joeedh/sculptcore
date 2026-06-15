/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {AttrGroup} from './AttrGroup'
import type {BuiltinAttr} from './BuiltinAttr'
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

export interface FaceData {
  [Symbol.dispose](): void
  attrs: AttrGroup
  capacity_: int32
  count: int32
  list_count: BuiltinAttr<int16, '.face.list_count'>
  l: BuiltinAttr<int32, '.face.list'>
  no: BuiltinAttr<float3, '.face.normal'>
  alloc(count: int32, clear: boolean): void
  alloc(): int32
}
