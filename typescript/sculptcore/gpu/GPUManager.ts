/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {DrawBatch} from "./DrawBatch";
import type {DrawCommand} from "./DrawCommand";
import type {Buffer} from "./Buffer";
import type {GPUType} from "./GPUType";
import type {GPUCmdType} from "./GPUCmdType";
import type {ShaderDef} from "./ShaderDef";

/** Auto-generated file */
/* eslint-disable @typescript-eslint/no-misused-new */
/* eslint-disable @typescript-eslint/no-unused-vars */

type float = number;
type pointer<T=any> = number;
type int = number;
type uint = number;
type double = number;
type short = number;
type ushort = number;
type char = number;
type uchar = number;
export interface GPUManager {
  [Symbol.dispose](): void;
  buffers: Buffer[]
  batches: DrawBatch[]
  commands: DrawCommand[]
  createBuffer(name: string, type: GPUType, elemsize: int, elemCount: int): Buffer
  createBatch(): DrawBatch
  createCommand(batch: DrawBatch, type: GPUCmdType, shader: ShaderDef | undefined, start: int, end: int, primCount: int): DrawCommand
  new(): GPUManager
}
