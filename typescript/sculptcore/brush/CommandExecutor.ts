/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {Brush} from './Brush'
import type {BrushProgram} from './BrushProgram'
import type {SpatialTree} from '../spatial/SpatialTree'
import type {SpatialNode} from '../spatial/SpatialNode'
import type {float3} from '../../litestl/math/float3'
import type {SculptBrushes} from './SculptBrushes'
import type {MeshLog} from '../meshlog/MeshLog'

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

export interface CommandExecutor {
  [Symbol.dispose](): void
  brush: Brush | undefined
  tree: SpatialTree | undefined
  meshLog: MeshLog | undefined
  execBrush(
    brushType: SculptBrushes,
    nodes: SpatialNode[],
    origin: float3,
    normal: float3
  ): void
  execProgram(
    prog: BrushProgram,
    nodes: SpatialNode[],
    origin: float3,
    normal: float3
  ): void
  clearIsFirstOfStep(): void
  setNeighborMode(mode: int32): void
  new (arg0: SpatialTree, arg1: Brush): CommandExecutor
}
