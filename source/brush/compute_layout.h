#pragma once

#include <cstdint>

namespace sculptcore::brush {

/* Host mirrors of the WGSL uniform/storage structs emit_wgsl.cc produces.
 * Field offsets MUST match the shader's std140 (uniform) / std430 (storage)
 * layout — see source/brush/compiler/emit_wgsl.cc. Explicit padding makes the
 * layout independent of the C++ ABI. Shared by every GPU compute backend
 * (vk_compute, wgpu_compute) so there is one source of truth. */

/* binding 5 — std140, size 80. */
struct ComputeBrushUniforms {
  float strength = 0.0f;
  float radius = 1.0f;
  float spacing = 0.25f;
  uint32_t invert = 0;
  uint32_t falloff_kind = 0;
  uint32_t falloff_shape = 0;
  uint32_t _pad0[2] = {0, 0};          // pad so falloff_dir (vec3) lands at 32
  float falloff_dir[3] = {0, 0, 1};    // offset 32
  uint32_t _pad1 = 0;                  // pad so falloff_extent (vec3) lands at 48
  float falloff_extent[3] = {1, 1, 1}; // offset 48 — FalloffShape::Box extents
  uint32_t coord_space = 0;            // offset 60
  float tex_repeat = 1.0f;             // offset 64
  uint32_t stroke_path_count = 0;      // offset 68
  float mu = 1.0f;                     // offset 72 — kelvinlet (else unused)
  float nu = 0.4f;                     // offset 76 — kelvinlet; rounds struct to 80
};

/* binding 6 — std140. Base block (surfacePos/surfaceNo/render_matrix) is 96
 * bytes; the global-brush tail starts at offset 96. Kelvinlet's grab vectors
 * and pose's cage arrays both begin there in their respective kernels'
 * CtxUniforms, so they alias in a union — only one kernel's view is live per
 * dispatch (size = 96 + 128 = 224). */
struct ComputeCtxUniforms {
  float surfacePos[3] = {0, 0, 0};
  uint32_t _pad0 = 0;
  float surfaceNo[3] = {0, 0, 1};
  uint32_t _pad1 = 0;
  float render_matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  union {
    struct {                              // kelvinlet.wgsl CtxUniforms tail
      float grabFrom[3]; uint32_t _kpad0;  // offset 96
      float grabTo[3];   uint32_t _kpad1;  // offset 112
    } kelvinlet;
    struct {                    // pose.wgsl tail — std140 array<vec3> stride 16
      float poseCageRest[4][4];  // offset 96  ([i][0..2]=xyz, [i][3]=pad)
      float poseCageNow[4][4];   // offset 160
    } pose;
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

/* One spatial-node chunk: a (offset,count) window into the flattened
 * unique_verts array. count must be <= 64 (the kernel's workgroup size);
 * the host splits larger nodes into multiple chunks. */
struct ComputeNodeMeta {
  uint32_t vert_offset = 0;
  uint32_t vert_count = 0;
};

/* binding 12 element — std430 vec2<u32>, stride 8. CSR neighbor index: for
 * global vertex i, its neighbors are nbr_verts[offset .. offset+count). */
struct ComputeVertNbr {
  uint32_t offset = 0;
  uint32_t count = 0;
};

} // namespace sculptcore::brush
