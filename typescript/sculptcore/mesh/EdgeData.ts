/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {int2} from "../../litestl/math/int2";
import type {int4} from "../../litestl/math/int4";
import type {BuiltinAttr} from "./BuiltinAttr";

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
export interface EdgeData {
  [Symbol.dispose](): void;
  capacity_: int
  c: BuiltinAttr<int,".edge.c">
  vs: BuiltinAttr<int2,".edge.vs">
  select: BuiltinAttr<boolean,"select">
  disk: BuiltinAttr<int4,".edge.vs.disk">
}
