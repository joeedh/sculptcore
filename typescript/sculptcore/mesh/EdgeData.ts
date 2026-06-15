/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {int4} from '../../litestl/math/int4'
import type {BuiltinAttr} from './BuiltinAttr'
import type {int2} from '../../litestl/math/int2'

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

export interface EdgeData {
  [Symbol.dispose](): void
  capacity_: int32
  c: BuiltinAttr<int32, '.edge.c'>
  vs: BuiltinAttr<int2, '.edge.vs'>
  select: BuiltinAttr<boolean, 'select'>
  disk: BuiltinAttr<int4, '.edge.vs.disk'>
}
