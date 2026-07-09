/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {DrawCommand} from './DrawCommand'
import type {Buffer} from './Buffer'
import type {GPUCmdType} from './GPUCmdType'
import type {ShaderDef} from './ShaderDef'
import type {GPUType} from './GPUType'
import type {DrawBatch} from './DrawBatch'

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

export interface GPUManager {
  [Symbol.dispose](): void
  buffers: Buffer[]
  batches: DrawBatch[]
  commands: DrawCommand[]
  createBuffer(name: string, type: GPUType, elemsize: int32, elemCount: int32): Buffer
  createBatch(): DrawBatch
  createCommand(
    batch: DrawBatch,
    type: GPUCmdType,
    shader: ShaderDef | undefined,
    start: int32,
    end: int32,
    primCount: int32
  ): DrawCommand
  destroyBatch(batch: DrawBatch, destroy_commands: boolean, destroy_buffers: boolean): void
  destroyBuffer(buffer: Buffer): void
  destroyCommand(command: DrawCommand, destroy_buffers: boolean): void
  new (): GPUManager
}
