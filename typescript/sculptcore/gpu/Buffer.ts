/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {GPUType} from "./GPUType";
import type {GPUManager} from "./GPUManager";
import type {GPUFetchMode} from "./GPUFetchMode";

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
export interface Buffer {
  [Symbol.dispose](): void;
  type: GPUType
  size: int
  elemsize: int
  mode: GPUFetchMode
  data: pointer
  update_buffer: boolean
  new(arg0: GPUManager, arg1: string, arg2: GPUType, arg3: int, arg4: GPUFetchMode, arg5: int): Buffer
}
