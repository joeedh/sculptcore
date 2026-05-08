/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {float3} from '../../litestl/math/float3'
import type {DrawBatch} from '../gpu/DrawBatch'
import type {CastRayIsect} from './CastRayIsect'
import type {GPUManager} from '../gpu/GPUManager'
import type {Mesh} from '../mesh/Mesh'
import type {SpatialNode} from './SpatialNode'

/** Auto-generated file */
/* eslint-disable @typescript-eslint/no-misused-new */
/* eslint-disable @typescript-eslint/no-unused-vars */

type pointer<T = any> = number
type int8 = number
type uint8 = number
type int16 = number
type uint16 = number
type int32 = number
type uint32 = number
type int64 = number
type uint64 = number
type float = number
type double = number

export interface SpatialTree {
  [Symbol.dispose](): void
  leaf_limit: int32
  depth_limit: int32
  setup(): void
  add_face(face: int32): void
  split_node(node: SpatialNode): void
  node_from_id(id: int32): SpatialNode | undefined
  leaves(): SpatialNode[]
  ensure_node_tris(node: SpatialNode): boolean
  buildAll(): void
  buildLeafBoundsBatch(batch: GPUManager): DrawBatch | undefined
  update(gpu: GPUManager): boolean
  getDrawBatch(): DrawBatch | undefined
  castRay(orig: float3, dir: float3, out: CastRayIsect): boolean
  filterNodes(
    origin: float3,
    ray: float3,
    radius: float,
    out: SpatialNode[]
  ): boolean
  new (arg0: Mesh): SpatialTree
}
