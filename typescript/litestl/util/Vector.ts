/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {UniformBlockInstance} from '../../sculptcore/gpu/UniformBlockInstance'

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

export interface Vector<T = any> {
  [Symbol.dispose](): void
  resize(newsize: int32): void
  resize_no_construct_destruct(newsize: int32): void
  new (): UniformBlockInstance[]
  new (arg0: T, arg1: int32): UniformBlockInstance[]
}
