// Billboard point-sprite overlay for box-modeling vertex display. Each vertex is
// drawn as two triangles (6 verts) sharing the vertex position; the per-vertex
// `corner` in [-1,1]^2 expands them into a screen-facing, pixel-constant-size
// quad, and the fragment stage clips to a disk for round points.
struct PointUniforms {
  drawMatrix   : mat4x4f,
  uColor       : vec4f,
  viewportSize : vec4f, // xy = framebuffer size in pixels
};
@group(0) @binding(0) var<uniform> u : PointUniforms;

struct VsIn {
  @location(0) position : vec3f,
  @location(1) corner   : vec2f,
  @location(2) color    : vec4f,
};

struct VsOut {
  @builtin(position) clipPos : vec4f,
  @location(0) vColor  : vec4f,
  @location(1) vCorner : vec2f,
};

const PT_HALF_PX : f32 = 4.0;

@vertex
fn vs_main(in : VsIn) -> VsOut {
  var out : VsOut;
  var clip = u.drawMatrix * vec4f(in.position, 1.0);
  // Offset the clip-space position by the quad corner scaled to PT_HALF_PX
  // pixels. NDC spans 2 units across the viewport, so 2/size is NDC-per-pixel;
  // multiplying by clip.w cancels the later perspective divide â†’ constant pixels.
  let ndcPerPx = vec2f(2.0, 2.0) / max(u.viewportSize.xy, vec2f(1.0, 1.0));
  clip = vec4f(clip.xy + in.corner * PT_HALF_PX * ndcPerPx * clip.w, clip.zw);
  // Overlay depth bias — keep in sync with litemesh_wgsl.ts OVERLAY_DEPTH_BIAS.
  clip.z = clip.z - 5e-3 * (clip.w - clip.z);
  out.clipPos = clip;
  out.vColor = in.color;
  out.vCorner = in.corner;
  return out;
}

@fragment
fn fs_main(in : VsOut) -> @location(0) vec4f {
  if (dot(in.vCorner, in.vCorner) > 1.0) {
    discard;
  }
  return u.uColor * in.vColor;
}
