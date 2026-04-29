/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */

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
  add_face(face: int): void
  split_node(node: SpatialNode): void
  node_from_id(id: int): SpatialNode | undefined
  leaves(): SpatialNode | undefined[]
  ensure_node_tris(node: SpatialNode): boolean
  buildAll(): void
  buildLeafBoundsBatch(batch: reference): DrawBatch | undefined
  new(arg0: Mesh | undefined): SpatialTree
}
