/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */
import type {FalloffKind} from '../gpu/FalloffKind'
import type {FalloffShape} from '../gpu/FalloffShape'
import type {float3} from '../../litestl/math/float3'
import type {StructProp} from '../props/StructProp'
import type {float4} from '../../litestl/math/float4'

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

export interface Brush {
  [Symbol.dispose](): void
  falloffCurveSize: float
  falloff_shape: FalloffShape
  falloff_kind: FalloffKind
  strength: float
  radius: float
  spacing: float
  planeoff: float
  autosmooth: float
  invert: boolean
  mu: float
  nu: float
  pinch: float
  projection: float
  rake: float
  automask_cavity: boolean
  cavity_factor: float
  cavity_blur_steps: int32
  cavity_inverted: boolean
  cavity_use_curve: boolean
  cavityCurveSize: float
  enhance_rings: int32
  enhance_inner: int32
  grabFrom: float3
  grabTo: float3
  falloff_dir: float3
  falloff_extent: float3
  planeSide: float
  strokeDir: float3
  wingAngle: float
  wingNormalA: float3
  wingNormalB: float3
  activeGroup: int32
  brushColor: float4
  mixMode: int32
  props: StructProp
  setFalloffCurveEntry(i: int32, f: float): void
  setCavityCurveEntry(i: int32, f: float): void
  loadProps(): void
  writeProps(): void
  pushDeviceInput(type: int32, value: float): void
  clearDeviceInputs(): void
  clearPropDynamics(propId: int32): void
  addPropDynamic(
    propId: int32,
    deviceType: int32,
    mixMode: int32,
    mixFactor: float
  ): void
  setPropDynamicSample(
    propId: int32,
    deviceType: int32,
    i: int32,
    n: int32,
    value: float
  ): void
  clearPropDynamicsByName(name: string): void
  addPropDynamicByName(
    name: string,
    deviceType: int32,
    mixMode: int32,
    mixFactor: float
  ): void
  setPropDynamicSampleByName(
    name: string,
    deviceType: int32,
    i: int32,
    n: int32,
    value: float
  ): void
  setPropsParent(parentProps: StructProp): void
  clearPropsParent(): void
  new (): Brush
}
