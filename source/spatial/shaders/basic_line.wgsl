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
  out.clipPos = spatial.drawMatrix * vec4f(in.position, 1.0);
  out.vColor = in.color;
  return out;
}

@fragment
fn fs_main(in : VsOut) -> @location(0) vec4f {
  return spatial.uColor * in.vColor;
}
