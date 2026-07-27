#pragma once

#include "brush/gpu_marshal.h"

#include "litestl/util/set.h"
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

/** One GPU brush stroke's host-side state, behind the app-facing GpuBrush_*
 * C-API (documentation/plans/gpuGlobalBrushes.md §3). The TS dispatcher owns
 * the GPU objects; this session owns every byte layout: begin blobs, per-dab
 * uniform/index packing (via gpu_marshal), undo snapshots, and the stroke-end
 * write-back. Created by GpuBrush_beginStroke, freed by GpuBrush_endStroke
 * (or GpuBrush_free on the abort path). The caller opens/closes the MeshLog
 * step around the stroke exactly as on the CPU path — this session only
 * appends snapshots into the already-open step. */
struct GpuBrushSession {
  mesh::Mesh *mesh = nullptr;
  spatial::SpatialTree *tree = nullptr;
  Brush *brush = nullptr;
  meshlog::MeshLog *log = nullptr;
  SculptBrushes tool = SculptBrushes::DRAW;
  const GpuKernelInfo *info = nullptr;
  int elemCount = 0;

  // Stroke-static begin blobs (packed xyz co/no + f32 mask).
  litestl::util::Vector<float> co, no, mask;
  // Per-vertex cavity automask factor (binding 24), one f32 per vertex. Filled
  // identity 1.0 when cavity masking is off. Stroke-static like co/no/mask.
  litestl::util::Vector<float> automask;
  // Scratch for GPUBRUSH_DATA_LIVE_CO (repacked per query).
  litestl::util::Vector<float> liveCo;

  // Neighbor CSR (needsNeighbors kernels only). nbrVerts borrows the mesh
  // topo cache's flat array — valid while topology is static (the stroke).
  litestl::util::Vector<ComputeVertNbr> nbrMeta;
  const uint32_t *nbrVerts = nullptr;
  int nbrCount = 0;

  // Normal-pass topology, built lazily on first request (M3).
  GpuNormalTopology topo;
  bool topoBuilt = false;

  // GPU-node scatter tables (M3): meta = 6 u32 per node (pos/nor buffer
  // identity keys lo,hi + corner offset,count), map = corner->global-vert in
  // fill order. metaBuilt is per stroke; the corner map fills lazily only
  // when SCATTER_MAP is queried (TS caches it across strokes keyed on
  // GPUBRUSH_INFO_GPU_LAYOUT_GEN).
  litestl::util::Vector<uint32_t> scatterMeta;
  litestl::util::Vector<uint32_t> scatterMap;
  litestl::util::Vector<spatial::SpatialNode *> scatterOwners;
  bool scatterMetaBuilt = false;
  bool scatterMapBuilt = false;
  // Meta indices of the GPU owner nodes touched by the last marshalDab.
  litestl::util::Vector<uint32_t> touchedOwnerIdx;

  // Last-marshaled-dab state (GpuBrush_marshalDab).
  litestl::util::Vector<spatial::SpatialNode *> nodes;
  litestl::util::Vector<uint32_t> uverts;
  litestl::util::Vector<ComputeNodeMeta> chunks;
  ComputeBrushUniforms brushU;
  ComputeCtxUniforms ctxU;
  litestl::util::Vector<ComputeStrokeSample> strokePath;
  // True when uverts/chunks differ from the previous dab's — TS skips the
  // re-upload otherwise. Derived from the node-set compare in marshalDab
  // (chunk order is a pure function of the filtered set).
  bool uvertsChanged = true;

  // Union of nodes touched this stroke (endStroke dirty-flags these). The set
  // twin makes per-dab membership O(1) — a whole-mesh brush filters thousands
  // of leaves every dab, and Vector::contains scans made marshal O(n²).
  litestl::util::Vector<spatial::SpatialNode *> touched;
  litestl::util::Set<spatial::SpatialNode *> touchedSet;
  // Nodes marshaled since the last GpuBrush_applyCo — the per-dab readback
  // path dirty-flags exactly these (drained on apply).
  litestl::util::Vector<spatial::SpatialNode *> pendingDirty;
  litestl::util::Set<spatial::SpatialNode *> pendingDirtySet;
  // Previous dab's filtered node set: when unchanged (the steady state of an
  // anchored whole-mesh brush), the snapshot walk, chunk rebuild, uverts
  // compare, and owner mapping are all skipped — per-dab marshal collapses to
  // the node filter + uniform packing.
  litestl::util::Vector<spatial::SpatialNode *> prevNodes;

  // Grab-class per-dab generation (mirrors CommandExecutor::dabGen): bumped on
  // the primary image; the kernel's first-touch stamp arbitration keys on it.
  uint32_t dabGen = 0;
};

/** GpuBrush_info(session, which) selectors. Mirrored by hand in
 * typescript/api/wasm.ts (GpuBrushInfo) — keep the two in sync. */
enum GpuBrushInfoWhich : int32_t {
  GPUBRUSH_INFO_ELEM_COUNT = 0,
  GPUBRUSH_INFO_NEEDS_NEIGHBORS = 1,
  GPUBRUSH_INFO_WRITES_MASK = 2,
  GPUBRUSH_INFO_WRITES_COLOR = 3,
  GPUBRUSH_INFO_ACCUMULABLE = 4,
  GPUBRUSH_INFO_READS_VCLASS = 5,
  GPUBRUSH_INFO_FACE_MODE = 6,
  // 7 was GPUBRUSH_INFO_IS_GLOBAL (@global); the tag no longer exists.
  GPUBRUSH_INFO_TRI_COUNT = 8, // builds the normal topology on first query
  GPUBRUSH_INFO_UVERTS_CHANGED = 9,
  GPUBRUSH_INFO_NODE_COUNT = 10,   // last dab's workgroup (chunk) count
  GPUBRUSH_INFO_UNIQUE_COUNT = 11, // last dab's flattened element count
  GPUBRUSH_INFO_STROKE_SAMPLE_COUNT = 12,
  GPUBRUSH_INFO_DAB_GEN = 13,
  // SpatialTree::gpuLayoutGen (truncated to 31 bits) — the scatter-table
  // cache key. Builds the scatter meta on first query.
  GPUBRUSH_INFO_GPU_LAYOUT_GEN = 14,
  GPUBRUSH_INFO_SCATTER_NODE_COUNT = 15,
};

/** GpuBrush_dataPtr/dataSize(session, which) selectors — the raw upload blobs,
 * already in GPU layout per compute_layout.h. Mirrored by hand in
 * typescript/api/wasm.ts (GpuBrushData) — keep the two in sync. */
enum GpuBrushDataWhich : int32_t {
  GPUBRUSH_DATA_CO = 0,   // f32 xyz per element (begin snapshot)
  GPUBRUSH_DATA_NO = 1,   // f32 xyz per element
  GPUBRUSH_DATA_MASK = 2, // f32 per element
  GPUBRUSH_DATA_NBR_META = 3,  // u32 pairs per vert (binding 12)
  GPUBRUSH_DATA_NBR_VERTS = 4, // u32 flat (binding 13)
  GPUBRUSH_DATA_TRI_VERTS = 5,     // u32, 3 per tri (builds topo lazily)
  GPUBRUSH_DATA_VERT_TRI_META = 6, // u32 pairs per vert
  GPUBRUSH_DATA_VERT_TRI_LIST = 7, // u32 flat incident-tri CSR
  GPUBRUSH_DATA_UVERTS = 8,         // u32 (binding 3), last dab
  GPUBRUSH_DATA_NODE_META = 9,      // u32 pairs (binding 4), last dab
  GPUBRUSH_DATA_BRUSH_UNIFORMS = 10, // 96 B (binding 5), last dab
  GPUBRUSH_DATA_CTX_UNIFORMS = 11,   // 224 B (binding 6), last dab
  GPUBRUSH_DATA_FALLOFF_LUT = 12,    // 256 f32 (binding 7)
  GPUBRUSH_DATA_STROKE_PATH = 13,    // 32 B samples (binding 10), last dab
  // Live mesh positions, re-packed on every query (packed xyz). Shadow-verify
  // diffs the GPU readback against this after each CPU-authoritative dab.
  GPUBRUSH_DATA_LIVE_CO = 14,
  // M3 scatter tables: meta = u32×6 per GPU node, map = u32 per render
  // corner (lazily built — querying it pays the corner walk), touched =
  // u32 meta indices of the owners hit by the last marshalDab.
  GPUBRUSH_DATA_SCATTER_META = 15,
  GPUBRUSH_DATA_SCATTER_MAP = 16,
  GPUBRUSH_DATA_TOUCHED_OWNERS = 17,
  // Per-vertex cavity automask factor (binding 24), one f32 per vertex.
  GPUBRUSH_DATA_AUTOMASK = 18,
};

} // namespace sculptcore::brush

/* App-facing seam (both backends: WASM EXPORTED_FUNCTIONS + N-API thunks).
 * Handles are opaque; sizes/pointers cross as bytes the caller uploads
 * verbatim. See gpu_brush_c_api.cc for per-function contracts. */
extern "C" {
void *GpuBrush_beginStroke(void *mesh, void *tree, void *brush, void *meshLog,
                           int tool);
void GpuBrush_free(void *session);
const char *GpuBrush_kernelName(void *session);
int GpuBrush_info(void *session, int which);
int GpuBrush_marshalDab(void *session, float cx, float cy, float cz, float nx,
                        float ny, float nz, float radius, float filterRadius,
                        int mirrorIdx, int nonaccum);
int GpuBrush_dataSize(void *session, int which);
const void *GpuBrush_dataPtr(void *session, int which);
void GpuBrush_applyCo(void *session, const float *co, int elemCount);
void GpuBrush_endStroke(void *session, const float *co, const float *no,
                        int elemCount);
}
