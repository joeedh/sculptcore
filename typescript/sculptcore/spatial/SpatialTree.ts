/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {SpatialNode} from './SpatialNode'
import type {GPUManager} from '../gpu/GPUManager'
import type {DrawBatch} from '../gpu/DrawBatch'
import type {CastRayIsect} from './CastRayIsect'
import type {Mesh} from '../mesh/Mesh'
import type {float3} from '../../litestl/math/float3'

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
  gpu_tri_target: int32
  setup(): void
  add_face(face: int32, searchNode: boolean): void
  split_node(node: SpatialNode): void
  node_from_id(id: int32): SpatialNode | undefined
  leaves(): SpatialNode[]
  ensure_node_tris(node: SpatialNode): boolean
  buildAll(): void
  buildLeafBoundsBatch(batch: GPUManager): DrawBatch | undefined
  buildSeamBatch(
    mgr: GPUManager,
    includePolyGroup: boolean
  ): DrawBatch | undefined
  buildSelectionBatch(
    mgr: GPUManager,
    activeVert: int32,
    activeEdge: int32,
    activeFace: int32
  ): DrawBatch | undefined
  buildWireframeBatch(mgr: GPUManager): DrawBatch | undefined
  markVertsMoved(verts: int32[]): void
  update(gpu: GPUManager): boolean
  getDrawBatch(): DrawBatch | undefined
  castRay(orig: float3, dir: float3, out: CastRayIsect): boolean
  filterNodes(co: float3, radius: float, out: SpatialNode[]): boolean
  setColorDisplayMode(mode: int32): void
  setDisplayColorAttr(index: int32): void
  setDisplayGroupAttr(index: int32): void
  setDisplayMask(on: boolean): void
  castScreenCircle(
    co: float3,
    ray: float3,
    r1: float,
    r2: float,
    faces: int32[],
    verts: int32[]
  ): boolean
  castScreenRect(
    near0: float3,
    near1: float3,
    near2: float3,
    near3: float3,
    far0: float3,
    far1: float3,
    far2: float3,
    far3: float3,
    faces: int32[],
    verts: int32[]
  ): boolean
  new (arg0: Mesh): SpatialTree
}
