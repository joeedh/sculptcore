/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {DrawBatch} from "./DrawBatch";
import type {DrawCommand} from "./DrawCommand";
import type {Buffer} from "./Buffer";
import type {GPUType} from "./GPUType";
import type {GPUCmdType} from "./GPUCmdType";

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
  buffers: pointer[]
  batches: pointer[]
  commands: pointer[]
  createBuffer(arg0: string, arg1: GPUType, arg2: int, arg3: int): pointer
  createBatch(): pointer
  createCommand(arg0: pointer, arg1: GPUCmdType, arg2: pointer, arg3: int, arg4: int, arg5: int): pointer
  new(): GPUManager
}
