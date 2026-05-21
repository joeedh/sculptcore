/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {Buffer} from './Buffer'
import type {UniformBlockInstance} from './UniformBlockInstance'
import type {GPUCmdType} from './GPUCmdType'
import type {ShaderDef} from './ShaderDef'

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

export interface DrawCommand {
  [Symbol.dispose](): void
  type: GPUCmdType
  shader: ShaderDef | undefined
  attrs: Buffer[]
  start: int32
  end: int32
  primCount: int32
  blocks: UniformBlockInstance[]
  new (): DrawCommand
}
