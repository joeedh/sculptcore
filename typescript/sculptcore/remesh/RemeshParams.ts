/* Warning: auto-generated file! Regenerate with 'pnpm build' in 'tools/' folder. */

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

export interface RemeshParams {
  [Symbol.dispose](): void
  target_edge_length: float
  solve_edge_length: float
  use_curvature: boolean
  use_sharp_features: boolean
  sharp_angle: float
  use_density: boolean
  reproject: boolean
  smooth_iterations: int32
  smooth_strength: float
  seed: uint32
  triage: boolean
  triage_weld_rel: float
  triage_min_component_frac: float
  new (): RemeshParams
  new (b: RemeshParams): RemeshParams
}
