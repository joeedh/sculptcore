/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {StrokeSpaceMode} from './StrokeSpaceMode'
import type {SpatialTree} from '../spatial/SpatialTree'
import type {DabSample} from './DabSample'
import type {AnchoredLiveMode} from './AnchoredLiveMode'
import type {StrokeMethod} from './StrokeMethod'

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

export interface BrushStrokeDriver {
  [Symbol.dispose](): void
  spaceMode: StrokeSpaceMode
  strokeMethod: StrokeMethod
  anchoredLiveMode: AnchoredLiveMode
  radiusIsWorld: boolean
  setViewRow(
    matId: int32,
    row: int32,
    x: float,
    y: float,
    z: float,
    w: float
  ): void
  setViewParams(
    camX: float,
    camY: float,
    camZ: float,
    viewW: float,
    viewH: float,
    glW: float,
    glH: float,
    camNear: float,
    hasObjectMatrix: boolean
  ): void
  push(
    x: float,
    y: float,
    pressure: float,
    tiltX: float,
    tiltY: float,
    twist: float,
    invert: boolean,
    useAltBrush: boolean,
    radius: float,
    strength: float,
    spacing: float
  ): void
  pushColor(r: float, g: float, b: float, a: float): void
  end(): void
  reset(): void
  poll(): int32
  sampleAt(i: int32): DabSample | undefined
  finished(): boolean
  hasAnchorScreen(): boolean
  anchorScreenX(): float
  anchorScreenY(): float
  hasPreviewScreen(): boolean
  previewScreenX(): float
  previewScreenY(): float
  getMatrixElem(matId: int32, row: int32, col: int32): float
  viewSizeX(): float
  viewSizeY(): float
  new (): BrushStrokeDriver
  new (arg0: SpatialTree): BrushStrokeDriver
}
