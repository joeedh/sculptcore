struct SpatialUniforms {
  drawMatrix : mat4x4f,
  uColor     : vec4f,
};
@group(0) @binding(0) var<uniform> spatial : SpatialUniforms;

struct VsIn {
  @location(0) position : vec3f,
  @location(1) color    : vec4f,
};

struct VsOut {
  @builtin(position) clipPos : vec4f,
  @location(0) vColor : vec4f,
};

@vertex
fn vs_main(in : VsIn) -> VsOut {
  var out : VsOut;
  var clip = spatial.drawMatrix * vec4f(in.position, 1.0);
  // Overlay depth bias (polygonOffset-style): pull toward the viewer in NDC.
  // Keep in sync with litemesh_wgsl.ts OVERLAY_DEPTH_BIAS.
  clip.z = clip.z - 5e-3 * (clip.w - clip.z);
  out.clipPos = clip;
  out.vColor = in.color;
  return out;
}

@fragment
fn fs_main(in : VsOut) -> @location(0) vec4f {
  return spatial.uColor * in.vColor;
}
