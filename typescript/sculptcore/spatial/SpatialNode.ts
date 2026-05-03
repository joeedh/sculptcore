/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {float3} from "../../litestl/math/float3";
import type {NodeFlags} from "./NodeFlags";
import type {AABB} from "../../litestl/math/AABB";

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
export interface SpatialNode {
  [Symbol.dispose](): void;
  aabb: AABB<float3>
  flag: NodeFlags
  id: int
}
