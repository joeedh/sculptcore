/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {AttrGroup} from './AttrGroup'
import type {float3} from '../../litestl/math/float3'
import type {BuiltinAttr} from './BuiltinAttr'

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

export interface VertexData {
  [Symbol.dispose](): void
  attrs: AttrGroup
  capacity_: int32
  co: BuiltinAttr<float3, 'positions'>
  no: BuiltinAttr<float3, 'normals'>
  e: BuiltinAttr<int32, '.vert.e'>
  alloc(count: int32, clear: boolean): void
  alloc(): int32
}
