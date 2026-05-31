struct SpatialUniforms {
  drawMatrix   : mat4x4f,
  normalMatrix : mat4x4f,
  uColor       : vec4f,
};
@group(0) @binding(0) var<uniform> spatial : SpatialUniforms;

struct VsIn {
  @location(0) position : vec3f,
  @location(1) normal   : vec3f,
  @location(2) color    : vec4f,
};

struct VsOut {
  @builtin(position) clipPos : vec4f,
  @location(0) vNormal : vec3f,
  @location(1) vColor  : vec4f,
};

@vertex
fn vs_main(in : VsIn) -> VsOut {
  var out : VsOut;
  out.clipPos = spatial.drawMatrix * vec4f(in.position, 1.0);
  let n = spatial.normalMatrix * vec4f(in.normal, 0.0);
  out.vNormal = normalize(n.xyz);
  out.vColor = in.color;
  return out;
}

@fragment
fn fs_main(in : VsOut) -> @location(0) vec4f {
  let no = in.vNormal;
  let f1 = dot(no, normalize(vec3f(0.1, 0.2, -1.0)));
  var f = f1;
  if (f1 < 0.0) { f = -f1 * 0.2; }
  f = f * 0.8 + 0.2;

  var vcolor = in.vColor;
  if (f1 < 0.0) {
    vcolor.z = vcolor.z * 0.8;
    vcolor.x = vcolor.x * 0.5;
  }
  // Surface is opaque — never let a per-vertex attr alpha make it transparent.
  return vec4f(vcolor.rgb * f, 1.0);
}
