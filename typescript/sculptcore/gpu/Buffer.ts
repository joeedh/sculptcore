/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {GPUType} from "./GPUType";
import type {GPUBufferType} from "./GPUBufferType";
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
  name: string
  type: GPUType
  size: int
  elemsize: int
  mode: GPUFetchMode
  target: GPUBufferType
  data: pointer
  update_buffer: boolean
  resize(size: int): void
  dirty(): void
}
