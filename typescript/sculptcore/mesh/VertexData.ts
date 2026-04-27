/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {AttrGroup} from "./AttrGroup";
import type {float3} from "../../litestl/math/float3";
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
export interface VertexData {
  [Symbol.dispose](): void;
  attrs: AttrGroup
  capacity_: int
  co: BuiltinAttr<float3,"positions">
  no: BuiltinAttr<float3,"normals">
  e: BuiltinAttr<int,".vert.e">
  alloc(arg0: int, arg1: boolean): void
  alloc(): int
}
