/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {GPUCmdType} from "./GPUCmdType";
import type {Buffer} from "./Buffer";

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
export interface DrawCommand {
  [Symbol.dispose](): void;
  type: GPUCmdType
  shader: pointer
  attrs: pointer[]
  start: int
  end: int
  primCount: int
  new(): DrawCommand
}
