#pragma once

#include "brush/brushes/types.h"
#include "brush/compute_layout.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <cstdint>

namespace sculptcore::mesh {
struct Mesh;
}
namespace sculptcore::meshlog {
struct MeshLog;
}
namespace sculptcore::spatial {
struct SpatialNode;
struct SpatialTree;
}

namespace sculptcore::brush {

struct Brush;

/** Host-side marshaling for GPU brush-compute dispatch, shared by the debug
 * app's GpuStrokeSession and the app-facing GpuBrush_* seam. This is the ONE
 * implementation of the std140/std430 packing contract in compute_layout.h —
 * a dispatcher (Vulkan, wgpu-native, or the TS WebGPU path) uploads these
 * blobs verbatim and never re-derives the layout. */

/** Per-tool GPU kernel capabilities. `kernel` is the .wgsl / .spv stem — the one
 * hand-written field. Every bool below is derived from the kernel's own
 * BrushCommandDef (brushDefFlagsFor), so it cannot drift from the CPU
 * executor's view of the same kernel. */
struct GpuKernelInfo {
  SculptBrushes tool = SculptBrushes::DRAW;
  const char *kernel = nullptr;
  bool needsNeighbors = false; // for_neighbor kernel: CSR at bindings 12/13
  bool writesMask = false;     // output is the mask layer, not geometry
  bool writesColor = false;    // float4 "color" vertex attr at slot 14
  bool accumulable = false;    // local deformation kernel; honors nonaccum
  bool readsVclass = false;    // int boundary-class vertex attr at slot 14
  bool faceMode = false;       // threads over faces (centroids/normals)
  bool grabMode = false;       // @grabmode: from-orig + first-touch stamps
};

/** Kernel-map lookup; null when the tool has no GPU kernel. */
const GpuKernelInfo *gpuKernelForTool(SculptBrushes tool);

/** Pack the per-dab brush uniforms (binding 5). `nonaccum` is the raw
 * per-stroke request; it is gated on GpuKernelInfo::accumulable here. The
 * appended DSL uniforms (offset 72+) are written by the kernel's generated
 * pack fn (builtinBrushGpuPack), which also applies the DSL @range clamps
 * that mirror CPU-only host-stage clamps -- hence the mutable brush. */
void packBrushUniforms(Brush &brush, SculptBrushes tool, bool nonaccum,
                       ComputeBrushUniforms &out);

/** Pack the per-dab ctx uniforms (binding 6). `renderMatrixRowMajor` is 16
 * floats in litestl row-major order, transposed here into the shader's
 * column-major layout; null leaves identity. */
void packCtxUniforms(const Brush &brush, SculptBrushes tool,
                     const litestl::math::float3 &origin,
                     const litestl::math::float3 &normal,
                     const float *renderMatrixRowMajor, ComputeCtxUniforms &out);

/** Flatten the brush's stroke path into the binding-10 storage layout. */
void packStrokePath(const Brush &brush,
                    litestl::util::Vector<ComputeStrokeSample> &out);

/** Pack full-mesh geometry for beginStroke: tightly packed xyz co/no plus one
 * mask float per element. Vertex mode reads m.v.* and the tree mesh's mask
 * (zeros when `tree` is null); face mode packs face centroids/normals with a
 * zero dummy mask (face kernels never read binding 2). Returns the element
 * count (verts or faces). Face mode walks live topology — thaw first. */
int packGeometry(mesh::Mesh &m, spatial::SpatialTree *tree, bool faceMode,
                 litestl::util::Vector<float> &co,
                 litestl::util::Vector<float> &no,
                 litestl::util::Vector<float> &mask);

/** Pack the per-vertex CAVITY automask factor for beginStroke (binding 24), one
 * f32 per vertex indexed the same as packGeometry's co/mask. Fills identity 1.0
 * when cavity masking is off (so GPU strength == CPU strength bit-for-bit);
 * otherwise computes cavityFactor through the same automask.h path the CPU
 * executor uses. The view-normal contributor is NOT packed — the kernel
 * evaluates it dynamically per dab from the ctx uniforms + no_buf
 * (brush_view_normal), so symmetry images each carry their own reflected ray.
 * Vertex kernels only — face kernels never bind it. */
void packAutomask(mesh::Mesh &m, const Brush &brush,
                  litestl::util::Vector<float> &out);

/** Derive the per-vertex {offset,count} CSR meta (binding 12) from the mesh's
 * shared ring-1 topo cache, building the cache if needed. `flatVerts`/
 * `flatCount` return a view of the cache's flat neighbor array (binding 13) —
 * valid while the cache is (static across a stroke). */
void packNeighborCSR(mesh::Mesh &m, int vcount,
                     litestl::util::Vector<ComputeVertNbr> &meta,
                     const uint32_t **flatVerts, int *flatCount);

/** Capture a node's pre-write co/no/f.no into the open MeshLog step, once per
 * node per stroke (element-keyed AttrSaver gate, so repeat calls are cheap
 * no-ops). Call before overwriting the node's verts with GPU results — the
 * mesh must still hold the correct "before" image. */
void snapshotNodeForUndo(meshlog::MeshLog &log, spatial::SpatialNode *node);

/** Flatten a dab's filtered nodes into the flat element-index array (binding
 * 3) plus <=64-wide per-workgroup chunks (binding 4). Face kernels chunk
 * unique_faces, vertex kernels unique_verts. */
void chunkNodes(const litestl::util::Vector<spatial::SpatialNode *> &nodes,
                bool faceMode, litestl::util::Vector<uint32_t> &uverts,
                litestl::util::Vector<ComputeNodeMeta> &chunks);

/** Global triangle topology + vertex->incident-tri CSR for a GPU normal pass,
 * plus the per-dab localized work-set derivation. Built once per stroke (the
 * mesh is topology-static); triangulation matches mesh::triangulate (the
 * render path's), so GPU normals are render/pick-grade, not bit-identical to
 * the CPU per-node normals. */
struct GpuNormalTopology {
  litestl::util::Vector<uint32_t> triVerts; // 3 global vert ids per tri
  litestl::util::Vector<uint32_t> meta;     // uvec2 per vert: offset,count
  litestl::util::Vector<uint32_t> list;     // incident-tri CSR body
  int triCount = 0;
  int vcount = 0;

  void build(mesh::Mesh &m);

  /** Build from an explicit triangle list (3 vert ids per tri) + dense vert
   * count — the grids domain's entry (Multires::levelTriIndicesOut produces
   * exactly this, in buildLevelTopo's fan order, so for a level mesh the two
   * builders emit identical tables). build(mesh) delegates here. */
  void buildFromArrays(const uint32_t *tris, int triCountIn, int vcountIn);

  /** Fill workTris/workVerts with the normal-pass work set for a dab that
   * moved `uverts`. workVerts = every vert of every tri incident to a moved
   * vert (their summed normal changes). workTris additionally expands to
   * every incident face of every work vert — a work vert on the region
   * boundary reads face normals from outside the moved set, which would
   * otherwise be stale (or garbage on the first dab); the extra faces exist
   * solely to give those verts a complete, fresh 1-ring. Deduped via
   * generation stamps; cost scales with the dab, not the mesh. */
  void dabWork(const litestl::util::Vector<uint32_t> &uverts,
               litestl::util::Vector<uint32_t> &workTris,
               litestl::util::Vector<uint32_t> &workVerts);

 private:
  litestl::util::Vector<uint32_t> triStamp_, vertStamp_;
  uint32_t stampGen_ = 0;
};

} // namespace sculptcore::brush
