/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {float3} from "./float3";

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
export interface AABB<T> {
  [Symbol.dispose](): void;
  min: float3
  max: float3
  new(): AABB<float3>
}
