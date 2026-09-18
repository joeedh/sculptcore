/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {BrushUniformManifestEntry} from './BrushUniformManifestEntry'
import type {BrushScalarResult} from './BrushScalarResult'
import type {Mesh} from '../mesh/Mesh'
import type {Brush} from './Brush'
import type {SpatialTree} from '../spatial/SpatialTree'
import type {MeshLog} from '../meshlog/MeshLog'
import type {SculptBrushes} from './SculptBrushes'
import type {float3} from '../../litestl/math/float3'
import type {DynTopoParams} from '../dyntopo/DynTopoParams'
import type {SpatialNode} from '../spatial/SpatialNode'
import type {BrushProgram} from './BrushProgram'
import type {DynTopoStats} from '../dyntopo/DynTopoStats'

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
  beginStep(hasDyntopo: boolean): void
  endStep(): void
  execBrush(
    mesh: Mesh,
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
  applyDab(
    prog: BrushProgram,
    center: float3,
    normal: float3,
    radius: float,
    params: DynTopoParams | undefined,
    seed: uint32
  ): int32
  endDynTopoStroke(): void
  clearIsFirstOfStep(): void
  beginPreviewDab(center: float3, radius: float): void
  extendPreviewDab(center: float3, radius: float): void
  rollbackPreviewDab(): void
  previewActive(): boolean
  commitPreviewDab(): void
  setNeighborMode(mode: int32): void
  setNonAccum(nonAccum: boolean): void
  setAnchoredGrab(anchored: boolean): void
  setGrabAccumAdd(add: boolean): void
  setStrokeGen(gen: int32): void
  lastUniformValidationOk(): boolean
  preflightRaw(type: SculptBrushes): boolean
  preflightRawProgram(program: BrushProgram): boolean
  supportsResolved(type: SculptBrushes): boolean
  supportsResolvedProgram(program: BrushProgram): boolean
  queryUniformManifest(brushType: int32): int32
  uniformQueryToken(): int32
  uniformSnapshotChecked(
    token: int32,
    uniformIndex: int32
  ): BrushUniformManifestEntry
  readUniformScalarChecked(
    token: int32,
    uniformIndex: int32,
    scalarType: int32,
    evaluate: boolean
  ): BrushScalarResult
  writeUniformScalarChecked(
    token: int32,
    uniformIndex: int32,
    scalarType: int32,
    value: double
  ): int32
  configureUniformDynamicChecked(
    token: int32,
    uniformIndex: int32,
    scalarType: int32,
    device: int32,
    mode: int32,
    factor: float
  ): int32
  enableUniformDynamicChecked(
    token: int32,
    uniformIndex: int32,
    scalarType: int32,
    device: int32,
    enabled: int32
  ): int32
  moveUniformDynamicChecked(
    token: int32,
    uniformIndex: int32,
    scalarType: int32,
    device: int32,
    index: int32
  ): int32
  clearUniformDynamicsChecked(
    token: int32,
    uniformIndex: int32,
    scalarType: int32
  ): int32
  replaceUniformDynamicTableChecked(
    token: int32,
    uniformIndex: int32,
    scalarType: int32,
    device: int32,
    samples: float[]
  ): int32
  setUniformDynamicSampleChecked(
    token: int32,
    uniformIndex: int32,
    scalarType: int32,
    device: int32,
    index: int32,
    count: int32,
    value: float
  ): int32
  replaceUniformDynamicsChecked(
    token: int32,
    uniformIndex: int32,
    scalarType: int32,
    devices: int32[],
    modes: int32[],
    factors: float[],
    enabled: int32[],
    offsets: int32[],
    samples: float[]
  ): int32
  replaceUniformResponseDynamicsChecked(
    token: int32,
    uniformIndex: int32,
    scalarType: int32,
    devices: int32[],
    modes: int32[],
    factors: float[],
    enabled: int32[],
    offsets: int32[],
    samples: float[],
    kinds: int32[],
    parameters: double[]
  ): int32
  queriedUniformEntry(idx: int32): BrushUniformManifestEntry | undefined
  filterRadiusFloor(brushType: SculptBrushes): float
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
  setRenderMatrix(m16: float[]): void
  new (arg0: SpatialTree, arg1: Brush): CommandExecutor
}
