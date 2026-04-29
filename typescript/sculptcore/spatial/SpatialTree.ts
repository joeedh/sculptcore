/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {GPUManager} from "../gpu/GPUManager";

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
export interface SpatialTree {
  [Symbol.dispose](): void;
  leaf_limit: int
  setup(): void
  add_face(arg0: int): void
  split_node(arg0: pointer): void
  node_from_id(arg0: int): pointer
  leaves(): pointer[]
  ensure_node_tris(arg0: pointer): boolean
  buildAll(): void
  buildLeafBoundsBatch(arg0: GPUManager): pointer
  new(arg0: pointer): SpatialTree
}
