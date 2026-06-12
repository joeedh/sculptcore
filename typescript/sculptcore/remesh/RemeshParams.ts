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
  target_quad_count: int32
  target_edge_length: float
  solve_edge_length: float
  use_curvature: boolean
  use_sharp_features: boolean
  sharp_angle: float
  feature_hysteresis: float
  feature_min_chain: int32
  use_density: boolean
  quantize_direct_rounding: boolean
  reproject: boolean
  cap_odd_holes: boolean
  smooth_iterations: int32
  smooth_strength: float
  seed: uint32
  triage: boolean
  triage_weld_rel: float
  triage_min_component_frac: float
  input_hole_fill_max_frac: float
  per_component: boolean
  curvature_smooth_iters: int32
  curvature_smooth_lambda: float
  field_smoothness: float
  curvature_weight: float
  singularity_cancel: boolean
  singularity_cancel_max_sep: float
  auto_density: boolean
  density_min: float
  density_max: float
  density_gradation: float
  density_gradation_iters: int32
  pre_remesh: boolean
  pre_remesh_target: float
  pre_remesh_iters: int32
  pre_remesh_density: boolean
  pre_remesh_gradation: float
  pre_remesh_gradation_iters: int32
  pre_remesh_align: float
  pre_remesh_field_cadence: int32
  pre_remesh_bootstrap_iters: int32
  pre_remesh_smooth_iters: int32
  pre_remesh_smooth_lambda: float
  pre_remesh_converge_eps: float
  pre_remesh_preserve_features: boolean
  pre_remesh_sharp_angle: float
  pre_remesh_trace: boolean
  new (): RemeshParams
  new (b: RemeshParams): RemeshParams
}
