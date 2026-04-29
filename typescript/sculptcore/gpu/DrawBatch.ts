/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {DrawCommand} from "./DrawCommand";
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
export interface DrawBatch {
  [Symbol.dispose](): void;
  commands: DrawCommand | undefined[]
  buffers: Buffer | undefined[]
  new(): DrawBatch
}
