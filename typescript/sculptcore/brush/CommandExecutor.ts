/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {Brush} from './Brush'
import type {SpatialTree} from '../spatial/SpatialTree'
import type {SpatialNode} from '../spatial/SpatialNode'
import type {SculptBrushes} from './SculptBrushes'

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
  brush: Brush
  tree: SpatialTree | undefined
  execBrush(
    brushType: SculptBrushes,
    nodes: SpatialNode | undefined,
    count: int32
  ): void
  new (arg0: SpatialTree, arg1: Brush): CommandExecutor
}
