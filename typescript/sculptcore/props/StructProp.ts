/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {Prop} from "./Prop";

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
export interface StructProp {
  [Symbol.dispose](): void;
  type: Prop
  name: string
  ui_name: string
  lookupFloat(name: string, default_value: float): float
}
