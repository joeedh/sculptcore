/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {AttrGroup} from './AttrGroup'
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

export interface CornerData {
  [Symbol.dispose](): void
  attrs: AttrGroup
  capacity_: int32
  capacity_: int32
  v: BuiltinAttr<int32, '.corner.v'>
  e: BuiltinAttr<int32, '.corner.e'>
  l: BuiltinAttr<int32, '.corner.l'>
  next: BuiltinAttr<int32, '.corner.next'>
  prev: BuiltinAttr<int32, '.corner.prev'>
  radial_next: BuiltinAttr<int32, '.corner.radial_next'>
  radial_prev: BuiltinAttr<int32, '.corner.radial_prev'>
  alloc(count: int32, clear: boolean): void
  alloc(): int32
}
