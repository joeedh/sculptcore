#pragma once

#include <cstdint>

namespace sculptcore::brush {

/* Host mirrors of the WGSL uniform/storage structs emit_wgsl.cc produces.
 * Field offsets MUST match the shader's std140 (uniform) / std430 (storage)
 * layout — see source/brush/compiler/emit_wgsl.cc. Explicit padding makes the
 * layout independent of the C++ ABI. Shared by every GPU compute backend
 * (vk_compute, wgpu_compute) so there is one source of truth. */

/* binding 5 — std140, size 112. */
struct ComputeBrushUniforms {
  float strength = 0.0f;
  float radius = 1.0f;
  float spacing = 0.25f;
  uint32_t invert = 0;
  uint32_t falloff_kind = 0;
  uint32_t falloff_shape = 0;
  uint32_t nonaccum = 0;               // offset 24 — non-accumulate stroke (accumulable kernels only)
  uint32_t grab_dab_gen = 0;           // offset 28 — grab-class per-dab generation (@grabmode
                                       // first-touch stamps); pad for every other kernel
  float falloff_dir[3] = {0, 0, 1};    // offset 32
  /* offset 44 — @unbounded cutoff radius as a multiple of `radius`; fills what
   * was falloff_dir's std140 pad, so falloff_extent still lands at 48.
   * 0 disables the window (every non-unbounded kernel). */
  float unbounded_extent = 0.0f;
  float falloff_extent[3] = {1, 1, 1}; // offset 48 — FalloffShape::Box extents
  uint32_t coord_space = 0;            // offset 60
  float tex_repeat = 1.0f;             // offset 64
  uint32_t stroke_path_count = 0;      // offset 68
  /* Offsets 72/76 are the appended DSL-uniform slots; every kernel's dynamic
   * uniforms start there, so per-kernel names share the slot via unions. */
  union {
    float mu = 1.0f;       // offset 72 — kelvinlet
    float planeoff;        //           — plane (Clay/Scrape/Fill)
    float pinch;           //           — pinch / sharp (toward-center/axis pull)
  };
  union {
    float nu = 0.4f;       // offset 76 — kelvinlet
    float planeSide;       //           — plane: +1 build-up, -1 cut
  };
  /* offset 80 — color kernel's `brushColor` vec4 (16-aligned past the scalar
   * slots, so 72/76 are pad in that kernel's WGSL view). */
  float brushColor[4] = {1, 1, 1, 1};
  /* offset 96 — color kernel's `mixMode` i32, appended after brushColor in its
   * WGSL view. std140 rounds the struct to its vec4 member's 16-byte
   * alignment, hence the tail pad and size 112. */
  int32_t mixMode = 0;
  uint32_t _pad2[3] = {0, 0, 0};
};

/* binding 6 — std140. Base block (surfacePos/surfaceNo/render_matrix + the
 * view-normal automask params) is 128 bytes; the global-brush tail starts at
 * offset 128. Kelvinlet's grab vectors and pose's cage arrays both begin there
 * in their respective kernels' CtxUniforms, so they alias in a union — only one
 * kernel's view is live per dispatch (size = 128 + 128 = 256). */
struct ComputeCtxUniforms {
  float surfacePos[3] = {0, 0, 0};
  uint32_t _pad0 = 0;
  float surfaceNo[3] = {0, 0, 1};
  uint32_t _pad1 = 0;
  float render_matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  /* View-normal automask params (offset 96): brush_view_normal in the WGSL
   * kernel evaluates viewNormalFactor's twin dynamically from these + the
   * no_buf vertex normal, per dab — so each symmetry image's dispatch carries
   * its own reflected ray. vn_enabled=0 short-circuits to 1.0. */
  float view_dir[3] = {0, 0, -1}; // offset 96
  uint32_t vn_enabled = 0;        // offset 108
  float vn_limit = 1.5707964f;    // offset 112 — kViewNormalLimitDefault
  float vn_falloff = 0.4363323f;  // offset 116 — kViewNormalFalloffDefault
  uint32_t vn_cull = 0;           // offset 120
  uint32_t _pad2 = 0;             // offset 124; base rounds to 128
  union {
    struct {                              // kelvinlet.wgsl CtxUniforms tail
      float grabFrom[3]; uint32_t _kpad0;  // offset 128
      float grabTo[3];   uint32_t _kpad1;  // offset 144
    } kelvinlet;
    struct {                    // pose.wgsl tail — std140 array<vec3> stride 16
      float poseCageRest[4][4];  // offset 128 ([i][0..2]=xyz, [i][3]=pad)
      float poseCageNow[4][4];   // offset 192
    } pose;
    struct {                              // grab.wgsl tail
      float grabTo[3]; uint32_t _gpad0;   // offset 128 (no grabFrom — the
                                          // kernel drags by grabTo alone)
    } grab;
  } global = {};
};

/* binding 10 element — std430, matching WGSL `struct StrokeSample`. A
 * vec3<f32> has 16-byte alignment but only 12-byte size, so `arclen` packs
 * into the tail of `normal`'s 16-byte slot at offset 28, giving stride 32 (NOT
 * 48 — over-padding here makes the shader read arclen out of the wrong slot). */
struct ComputeStrokeSample {
  float pos[3] = {0, 0, 0};     // offset 0
  uint32_t _pad0 = 0;           // pad to normal's 16-byte alignment
  float normal[3] = {0, 0, 1};  // offset 16
  float arclen = 0.0f;          // offset 28
};

/* One spatial-node chunk: a (offset,count) window into the flattened element
 * array. count must be <= 64 (the kernel's workgroup size); the host splits
 * larger nodes into multiple chunks.
 *
 * NOTE: the fields are named `vert_*` but the face-stage dispatch reuses this
 * same struct generically — for a face kernel they index the flattened
 * `unique_faces` array, not verts. Read them as elem_offset/elem_count when the
 * dispatch is in face mode (see gpu_stroke.cc faceMode_). */
struct ComputeNodeMeta {
  uint32_t vert_offset = 0;
  uint32_t vert_count = 0;
};

/* Binding 22 is retired: it held a read-only stroke-start position copy until
 * grab moved onto kDispBinding. Left unassigned so the later slots keep their
 * numbers. The attr superset (kAttrBase=14 + kMaxAttrBindings=8) ends there. */

/* Fixed binding of the grab-class per-vertex dab stamp (@grabmode kernels
 * only): one u32 per vertex, zero-filled at beginStroke, compared against
 * ComputeBrushUniforms::grab_dab_gen for first-touch arbitration (the GPU
 * twin of grabClaimFirstTouch / the `.brush.dab.gen` attr). */
inline constexpr uint32_t kDabStampBinding = 23;

/* Fixed binding of the read-only per-vertex CAVITY automask factor (vertex
 * kernels only): one f32 per vertex, filled host-side at beginStroke via
 * packAutomask. brush_strength multiplies it in — the GPU twin of the CPU
 * CommandCtx::strength cavity multiply. The host uploads identity 1.0 for
 * every vertex when cavity masking is off, so `strength * 1.0` keeps the GPU
 * path bit-for-bit equal to the (multiply-skipping) CPU path. The view-normal
 * contributor is dynamic (brush_view_normal from the ctx uniforms), not
 * packed. See automask.h. */
inline constexpr uint32_t kAutomaskBinding = 24;

/* Fixed binding of the per-vertex accumulated brush displacement — the base of
 * every from-base kernel, grab-class included
 * (plans/2026-07-26-0909-brush-displacement-base-attribute.md). read_write: the
 * kernel adds each dab's delta and the base is derived as `co - disp`. GPU
 * stroke topology is static, so the CPU's generational stamp collapses to the
 * beginStroke zero-fill. */
inline constexpr uint32_t kDispBinding = 25;

/* Fixed binding of the runtime texture-program param slab (spliced kernels
 * only, and only when the program has params): TextureProgram::paramSlabSize
 * f32s uploaded from Brush::texture_params at stroke begin. The spliced WGSL
 * declares it as `sb_tex_params` (emitWgslTextureDefs paramsFromBinding). */
inline constexpr uint32_t kTexParamsBinding = 26;

/* binding 12 element — std430 vec2<u32>, stride 8. CSR neighbor index: for
 * global vertex i, its neighbors are nbr_verts[offset .. offset+count). */
struct ComputeVertNbr {
  uint32_t offset = 0;
  uint32_t count = 0;
};

} // namespace sculptcore::brush
