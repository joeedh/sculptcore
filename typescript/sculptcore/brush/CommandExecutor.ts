/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {Brush} from './Brush'
import type {BrushProgram} from './BrushProgram'
import type {SpatialTree} from '../spatial/SpatialTree'
import type {SpatialNode} from '../spatial/SpatialNode'
import type {DynTopoStats} from '../dyntopo/DynTopoStats'
import type {float3} from '../../litestl/math/float3'
import type {BrushUniformManifestEntry} from './BrushUniformManifestEntry'
import type {SculptBrushes} from './SculptBrushes'
import type {MeshLog} from '../meshlog/MeshLog'
import type {DynTopoParams} from '../dyntopo/DynTopoParams'

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
  lastDynTopoStats: DynTopoStats
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
  applyDynTopoDab(
    center: float3,
    radius: float,
    params: DynTopoParams,
    seed: uint32
  ): int32
  endDynTopoStroke(): void
  clearIsFirstOfStep(): void
  setNeighborMode(mode: int32): void
  lastUniformValidationOk(): boolean
  queryUniformManifest(brushType: int32): int32
  queriedUniformEntry(idx: int32): BrushUniformManifestEntry | undefined
  clearUniformDynamics(idx: int32): void
  addUniformDynamic(
    idx: int32,
    deviceType: int32,
    mixMode: int32,
    mixFactor: float
  ): void
  setUniformDynamicSample(
    idx: int32,
    deviceType: int32,
    i: int32,
    n: int32,
    value: float
  ): void
  new (arg0: SpatialTree, arg1: Brush): CommandExecutor
}
