/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {StructProp} from "../props/StructProp";

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
export interface Brush {
  [Symbol.dispose](): void;
  strength: float
  radius: float
  invert: boolean
  props: StructProp
  loadProps(): void
}
