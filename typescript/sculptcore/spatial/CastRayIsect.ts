/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {float2} from "../../litestl/math/float2";
import type {float3} from "../../litestl/math/float3";

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
export interface CastRayIsect {
  [Symbol.dispose](): void;
  p: float3
  normal: float3
  t: float
  uv: float2
  triIndex: int
  nodeIndex: int
  new(): CastRayIsect
}
