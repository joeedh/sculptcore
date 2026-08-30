#include "brush/gpu_brush_session.h"

#include "brush/brush.h"
#include "litestl/util/alloc.h"
#include "mesh/mesh.h"
#include "meshlog/meshlog.h"
#include "spatial/node.h"
#include "spatial/spatial.h"
#include "spatial/spatial_enums.h"

#include <cstring>

using namespace sculptcore;
using namespace sculptcore::brush;
using litestl::math::float3;

namespace {

GpuBrushSession *cast(void *session)
{
  return static_cast<GpuBrushSession *>(session);
}

// Lazily build the normal-pass topology (vertex kernels only). Returns false
// in face mode, where the tri topology has no meaning for the dispatch.
bool ensureTopo(GpuBrushSession *s)
{
  if (s->info->faceMode) {
    return false;
  }
  if (!s->topoBuilt) {
    s->topo.build(*s->mesh);
    s->topoBuilt = true;
  }
  return true;
}

// Build the scatter meta (+ owners) once per stroke; the corner map only when
// asked (fillMap) — TS caches it across strokes keyed on the layout gen.
void ensureScatter(GpuBrushSession *s, bool fillMap)
{
  if (fillMap ? s->scatterMapBuilt : s->scatterMetaBuilt) {
    return;
  }
  s->tree->buildGpuScatterTables(
      s->scatterMeta, s->scatterMap, &s->scatterOwners, fillMap);
  s->scatterMetaBuilt = true;
  s->scatterMapBuilt = fillMap;
}

} // namespace

extern "C" {

/** Open a GPU stroke session: resolve the kernel, pack the stroke-static
 * geometry blobs (+ neighbor CSR when the kernel needs it), and reset the
 * brush stroke path. Returns null when the tool has no GPU kernel. The MeshLog
 * step must already be open (executor.beginStep), exactly as on the CPU path. */
void *GpuBrush_beginStroke(void *mesh, void *tree, void *brush, void *meshLog, int tool)
{
  auto *m = static_cast<mesh::Mesh *>(mesh);
  auto *t = static_cast<spatial::SpatialTree *>(tree);
  auto *b = static_cast<Brush *>(brush);
  auto *log = static_cast<meshlog::MeshLog *>(meshLog);
  if (!m || !t || !b || !log) {
    return nullptr;
  }

  const GpuKernelInfo *info = gpuKernelForTool(SculptBrushes(tool));
  if (!info) {
    return nullptr;
  }
  // This session only marshals — the host loads the kernel WGSL itself and
  // cannot consume the T5 splice (spliceTextureProgramWgsl) or upload the
  // binding-26 slab; a null return routes the host onto the CPU stroke path.
  if (b->texture_program) {
    return nullptr;
  }

  // A prior CPU stroke can leave the mesh topology-frozen; the CSR/topology
  // builds below walk live links. The mesh is static for the GPU stroke, so a
  // one-time thaw stays valid (a later CPU stroke re-freezes as needed).
  if (m->topo_frozen) {
    m->thawTopo();
  }

  auto *s = litestl::alloc::New<GpuBrushSession>("GpuBrushSession");
  s->mesh = m;
  s->tree = t;
  s->brush = b;
  s->log = log;
  s->tool = SculptBrushes(tool);
  s->info = info;

  s->elemCount = packGeometry(*m, t, info->faceMode, s->co, s->no, s->mask);
  if (info->needsNeighbors) {
    packNeighborCSR(*m, s->elemCount, s->nbrMeta, &s->nbrVerts, &s->nbrCount);
  }
  // Cavity automask (vertex kernels only) — identity 1.0 when off, so the GPU
  // strength stays bit-identical to the CPU path.
  if (!info->faceMode) {
    packAutomask(*m, *b, s->automask);
  }
  b->resetStrokePath();
  return s;
}

/** Free a session without touching the mesh (the abort path — e.g. GPU init
 * failed before the first dab). endStroke frees internally; don't call both. */
void GpuBrush_free(void *session)
{
  if (session) {
    litestl::alloc::Delete(cast(session));
  }
}

const char *GpuBrush_kernelName(void *session)
{
  GpuBrushSession *s = cast(session);
  return s ? s->info->kernel : "";
}

int GpuBrush_info(void *session, int which)
{
  GpuBrushSession *s = cast(session);
  if (!s) {
    return 0;
  }
  switch (which) {
  case GPUBRUSH_INFO_ELEM_COUNT:
    return s->elemCount;
  case GPUBRUSH_INFO_NEEDS_NEIGHBORS:
    return s->info->needsNeighbors ? 1 : 0;
  case GPUBRUSH_INFO_WRITES_MASK:
    return s->info->writesMask ? 1 : 0;
  case GPUBRUSH_INFO_WRITES_COLOR:
    return s->info->writesColor ? 1 : 0;
  case GPUBRUSH_INFO_ACCUMULABLE:
    return s->info->accumulable ? 1 : 0;
  case GPUBRUSH_INFO_READS_VCLASS:
    return s->info->readsVclass ? 1 : 0;
  case GPUBRUSH_INFO_FACE_MODE:
    return s->info->faceMode ? 1 : 0;
  case GPUBRUSH_INFO_TRI_COUNT:
    return ensureTopo(s) ? s->topo.triCount : 0;
  case GPUBRUSH_INFO_UVERTS_CHANGED:
    return s->uvertsChanged ? 1 : 0;
  case GPUBRUSH_INFO_NODE_COUNT:
    return int(s->chunks.size());
  case GPUBRUSH_INFO_UNIQUE_COUNT:
    return int(s->uverts.size());
  case GPUBRUSH_INFO_STROKE_SAMPLE_COUNT:
    return int(s->strokePath.size());
  case GPUBRUSH_INFO_DAB_GEN:
    return int(s->dabGen);
  case GPUBRUSH_INFO_GPU_LAYOUT_GEN:
    return int(s->tree->gpuLayoutGen & 0x7fffffffu);
  case GPUBRUSH_INFO_SCATTER_NODE_COUNT:
    ensureScatter(s, false);
    return int(s->scatterMeta.size() / 6);
  }
  return 0;
}

/** Marshal one dab (one symmetry image): run the spatial node filter at
 * `filterRadius` (the caller applies the same widened-radius policy as the CPU
 * path), snapshot the filtered nodes into the open MeshLog step, and pack the
 * per-dab upload blobs (uniforms/LUT/stroke path/indices). `mirrorIdx` 0 is
 * the primary image (bumps the grab dab generation + pushes the stroke
 * sample); >0 are mirror images of the same logical dab. Returns the workgroup
 * (chunk) count — 0 means the dab touched nothing and there is nothing to
 * dispatch. */
int GpuBrush_marshalDab(void *session,
                        float cx,
                        float cy,
                        float cz,
                        float nx,
                        float ny,
                        float nz,
                        float radius,
                        float filterRadius,
                        int mirrorIdx,
                        int nonaccum)
{
  GpuBrushSession *s = cast(session);
  if (!s) {
    return 0;
  }
  const float3 origin{cx, cy, cz};
  const float3 normal{nx, ny, nz};
  (void)radius; // falloff radius rides in on brush.radius (packBrushUniforms)

  if (mirrorIdx == 0) {
    s->dabGen++;
  }

  s->nodes.clear();
  s->tree->filterNodes(origin, filterRadius, s->nodes);
  if (s->nodes.size() == 0) {
    s->chunks.clear();
    s->uverts.clear();
    return 0;
  }

  // Steady-state fast path: the filtered node set of an anchored whole-mesh
  // brush saturates after the first dabs — when it is pointer-identical to
  // the previous dab's, every node is already snapshotted, the chunks/uverts/
  // touched-owner tables are all still valid, and marshal collapses to the
  // uniform packing below (the 5M-tri M5 requirement).
  const bool sameNodes =
      s->nodes.size() == s->prevNodes.size() &&
      (s->nodes.size() == 0 || std::memcmp(s->nodes.data(),
                                           s->prevNodes.data(),
                                           s->nodes.size() * sizeof(void *)) == 0);

  if (!sameNodes) {
    // Undo: capture each node's pre-write state now, while the CPU mesh still
    // holds it (the per-dab readback apply overwrites v.co afterwards). The
    // AttrSaver dedups per element, so once per node per stroke suffices.
    ensureScatter(s, false);
    s->touchedOwnerIdx.clear();
    for (spatial::SpatialNode *node : s->nodes) {
      if (s->touchedSet.add(node)) {
        snapshotNodeForUndo(*s->log, node);
        s->touched.append(node);
      }
      if (s->pendingDirtySet.add(node)) {
        s->pendingDirty.append(node);
      }
    }
    // Owner set for the M3 scatter pass (meta indices, deduped per dab).
    for (spatial::SpatialNode *node : s->nodes) {
      spatial::SpatialNode *owner = s->tree->find_gpu_owner(node);
      if (!owner) {
        continue;
      }
      for (int oi = 0; oi < int(s->scatterOwners.size()); oi++) {
        if (s->scatterOwners[oi] == owner) {
          uint32_t idx = uint32_t(oi);
          if (!s->touchedOwnerIdx.contains(idx)) {
            s->touchedOwnerIdx.append(idx);
          }
          break;
        }
      }
    }
    chunkNodes(s->nodes, s->info->faceMode, s->uverts, s->chunks);
    s->prevNodes = s->nodes;
  }

  s->brush->pushStrokeSample(origin, normal);

  packBrushUniforms(*s->brush, s->tool, nonaccum != 0, s->brushU);
  s->brushU.grab_dab_gen = s->info->grabMode ? s->dabGen : 0;
  packCtxUniforms(*s->brush, s->tool, origin, normal, nullptr, s->ctxU);
  packStrokePath(*s->brush, s->strokePath);

  // Index arrays changed exactly when the node set did (chunk order is a pure
  // function of the set) — no byte compare needed.
  s->uvertsChanged = !sameNodes;
  return int(s->chunks.size());
}

int GpuBrush_dataSize(void *session, int which)
{
  GpuBrushSession *s = cast(session);
  if (!s) {
    return 0;
  }
  switch (which) {
  case GPUBRUSH_DATA_CO:
    return int(s->co.size() * sizeof(float));
  case GPUBRUSH_DATA_NO:
    return int(s->no.size() * sizeof(float));
  case GPUBRUSH_DATA_MASK:
    return int(s->mask.size() * sizeof(float));
  case GPUBRUSH_DATA_AUTOMASK:
    return int(s->automask.size() * sizeof(float));
  case GPUBRUSH_DATA_NBR_META:
    return int(s->nbrMeta.size() * sizeof(ComputeVertNbr));
  case GPUBRUSH_DATA_NBR_VERTS:
    return int(s->nbrCount * sizeof(uint32_t));
  case GPUBRUSH_DATA_TRI_VERTS:
    return ensureTopo(s) ? int(s->topo.triVerts.size() * sizeof(uint32_t)) : 0;
  case GPUBRUSH_DATA_VERT_TRI_META:
    return ensureTopo(s) ? int(s->topo.meta.size() * sizeof(uint32_t)) : 0;
  case GPUBRUSH_DATA_VERT_TRI_LIST:
    return ensureTopo(s) ? int(s->topo.list.size() * sizeof(uint32_t)) : 0;
  case GPUBRUSH_DATA_UVERTS:
    return int(s->uverts.size() * sizeof(uint32_t));
  case GPUBRUSH_DATA_NODE_META:
    return int(s->chunks.size() * sizeof(ComputeNodeMeta));
  case GPUBRUSH_DATA_BRUSH_UNIFORMS:
    return int(sizeof(ComputeBrushUniforms));
  case GPUBRUSH_DATA_CTX_UNIFORMS:
    return int(sizeof(ComputeCtxUniforms));
  case GPUBRUSH_DATA_FALLOFF_LUT:
    return int(s->brush->falloff_curve.size() * sizeof(float));
  case GPUBRUSH_DATA_STROKE_PATH:
    return int(s->strokePath.size() * sizeof(ComputeStrokeSample));
  case GPUBRUSH_DATA_LIVE_CO:
    return s->info->faceMode ? 0 : int(s->elemCount * 3 * sizeof(float));
  case GPUBRUSH_DATA_SCATTER_META:
    ensureScatter(s, false);
    return int(s->scatterMeta.size() * sizeof(uint32_t));
  case GPUBRUSH_DATA_SCATTER_MAP:
    ensureScatter(s, true);
    return int(s->scatterMap.size() * sizeof(uint32_t));
  case GPUBRUSH_DATA_TOUCHED_OWNERS:
    return int(s->touchedOwnerIdx.size() * sizeof(uint32_t));
  }
  return 0;
}

const void *GpuBrush_dataPtr(void *session, int which)
{
  GpuBrushSession *s = cast(session);
  if (!s) {
    return nullptr;
  }
  switch (which) {
  case GPUBRUSH_DATA_CO:
    return s->co.data();
  case GPUBRUSH_DATA_NO:
    return s->no.data();
  case GPUBRUSH_DATA_MASK:
    return s->mask.data();
  case GPUBRUSH_DATA_AUTOMASK:
    return s->automask.data();
  case GPUBRUSH_DATA_NBR_META:
    return s->nbrMeta.data();
  case GPUBRUSH_DATA_NBR_VERTS:
    return s->nbrVerts;
  case GPUBRUSH_DATA_TRI_VERTS:
    return ensureTopo(s) ? s->topo.triVerts.data() : nullptr;
  case GPUBRUSH_DATA_VERT_TRI_META:
    return ensureTopo(s) ? s->topo.meta.data() : nullptr;
  case GPUBRUSH_DATA_VERT_TRI_LIST:
    return ensureTopo(s) ? s->topo.list.data() : nullptr;
  case GPUBRUSH_DATA_UVERTS:
    return s->uverts.data();
  case GPUBRUSH_DATA_NODE_META:
    return s->chunks.data();
  case GPUBRUSH_DATA_BRUSH_UNIFORMS:
    return &s->brushU;
  case GPUBRUSH_DATA_CTX_UNIFORMS:
    return &s->ctxU;
  case GPUBRUSH_DATA_FALLOFF_LUT:
    return s->brush->falloff_curve.data();
  case GPUBRUSH_DATA_STROKE_PATH:
    return s->strokePath.data();
  case GPUBRUSH_DATA_SCATTER_META:
    ensureScatter(s, false);
    return s->scatterMeta.data();
  case GPUBRUSH_DATA_SCATTER_MAP:
    ensureScatter(s, true);
    return s->scatterMap.data();
  case GPUBRUSH_DATA_TOUCHED_OWNERS:
    return s->touchedOwnerIdx.data();
  case GPUBRUSH_DATA_LIVE_CO: {
    if (s->info->faceMode) {
      return nullptr;
    }
    // Re-pack the live mesh positions (shadow-verify reads this per dab).
    s->liveCo.resize(size_t(s->elemCount) * 3);
    for (int i = 0; i < s->elemCount; i++) {
      litestl::math::float3 c = s->mesh->v.co[i];
      s->liveCo[i * 3 + 0] = c[0];
      s->liveCo[i * 3 + 1] = c[1];
      s->liveCo[i * 3 + 2] = c[2];
    }
    return s->liveCo.data();
  }
  }
  return nullptr;
}

/** Per-dab readback apply (the M2 interactive-readback shape): write the read
 * -back positions into the mesh and dirty-flag the nodes marshaled since the
 * last apply, so the caller's spatial.update regenerates exactly those. Undo
 * snapshots were already taken at marshalDab time. Vertex kernels only. */
void GpuBrush_applyCo(void *session, const float *co, int elemCount)
{
  GpuBrushSession *s = cast(session);
  if (!s || !co || s->info->faceMode || elemCount != s->elemCount) {
    return;
  }
  mesh::Mesh *m = s->mesh;
  for (int i = 0; i < elemCount; i++) {
    m->v.co[i] = float3(co[i * 3 + 0], co[i * 3 + 1], co[i * 3 + 2]);
  }
  for (spatial::SpatialNode *node : s->pendingDirty) {
    node->update(spatial::Spatial_UpdateNormals | spatial::Spatial_UpdateGPU |
                 spatial::Spatial_RegenBounds);
  }
  s->pendingDirty.clear();
  s->pendingDirtySet.clear();
}

/** Close the stroke: snapshot any not-yet-snapshotted touched node (the mesh
 * still holds its pre-stroke data when no per-dab apply ran), write the final
 * co (and optionally no) into the mesh, dirty-flag every touched node, and
 * free the session. Pass co=null when per-dab applies already landed the final
 * state. The caller then runs spatial.update + executor.endStep as on the CPU
 * path. Vertex kernels only. */
void GpuBrush_endStroke(void *session, const float *co, const float *no, int elemCount)
{
  GpuBrushSession *s = cast(session);
  if (!s) {
    return;
  }
  if (co && !s->info->faceMode && elemCount == s->elemCount) {
    mesh::Mesh *m = s->mesh;
    for (spatial::SpatialNode *node : s->touched) {
      snapshotNodeForUndo(*s->log, node);
    }
    for (int i = 0; i < elemCount; i++) {
      m->v.co[i] = float3(co[i * 3 + 0], co[i * 3 + 1], co[i * 3 + 2]);
    }
    if (no) {
      for (int i = 0; i < elemCount; i++) {
        m->v.no[i] = float3(no[i * 3 + 0], no[i * 3 + 1], no[i * 3 + 2]);
      }
    }
    for (spatial::SpatialNode *node : s->touched) {
      node->update(spatial::Spatial_UpdateNormals | spatial::Spatial_UpdateGPU |
                   spatial::Spatial_RegenBounds);
    }
  }
  litestl::alloc::Delete(s);
}

} // extern "C"
