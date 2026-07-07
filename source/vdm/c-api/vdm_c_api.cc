#include "displace/frames.h"
#include "mesh/mesh.h"
#include "meshlog/meshlog.h"
#include "spatial/spatial.h"
#include "vdm/vdm_splat.h"
#include "vdm/vdm_store.h"
#include "vdm/vdm_undo.h"

#include "litestl/util/alloc.h"

#include <cstring>
#include <sstream>

using namespace sculptcore;

extern "C" {

/* Tag every live face's `.detail.carrier` (DetailCarrier: 0 = GEOM, 1 = VDM).
 * The V3/V5 region partition replaces this whole-mesh fill; it exists so the
 * app harness can stand up a VDM-carried mesh. */
void SpatialTree_fillDetailCarrier(spatial::SpatialTree *t, int carrier)
{
  if (!t || !t->m) {
    return;
  }
  for (int f : t->m->f) {
    t->treeMesh.f.carrier.get_data()->materialize(f);
    t->treeMesh.f.carrier[f] = carrier;
  }
}

/* Recompute vertex normals + the F3 frames (smoothed normal + cross-field
 * tangent) over the whole mesh — the splatter's frame prerequisite. */
void Mesh_updateFrames(mesh::Mesh *m)
{
  if (!m) {
    return;
  }
  m->recalc_normals();
  displace::FrameProviderParams params;
  displace::updateFramesAll(*m, params);
}

vdm::VdmStore *VdmStore_new(int resolution, int tileSize)
{
  vdm::VdmStoreParams params;
  if (resolution > 0) {
    params.resolution = resolution;
  }
  if (tileSize > 0) {
    params.tile_size = tileSize;
  }
  return litestl::alloc::New<vdm::VdmStore>("VdmStore", params);
}

void VdmStore_free(vdm::VdmStore *store)
{
  if (store) {
    litestl::alloc::Delete(store);
  }
}

namespace {
/* texelsClamped of this thread's most recent Mesh_vdmSplatDab (X1 prompt
 * signal — clamp-at-ceiling on locked bases suggests adding a level). */
thread_local int g_lastSplatClamped = 0;
} // namespace

/* Splat one dab (V2's CPU reference path); returns texels touched. The caller
 * owns the undo bracket (store delta + MeshLog step) — this is the raw splat. */
int Mesh_vdmSplatDab(mesh::Mesh *m,
                     spatial::SpatialTree *tree,
                     vdm::VdmStore *store,
                     float cx,
                     float cy,
                     float cz,
                     float nx,
                     float ny,
                     float nz,
                     float radius,
                     float strength,
                     float alpha,
                     int invert)
{
  if (!m || !tree || !store) {
    g_lastSplatClamped = 0;
    return 0;
  }
  vdm::VdmSplatParams params;
  params.center = litestl::math::float3(cx, cy, cz);
  params.normal = litestl::math::float3(nx, ny, nz);
  params.radius = radius;
  params.strength = strength;
  params.alpha = alpha;
  params.invert = invert != 0;
  vdm::VdmSplatStats stats = vdm::splatDab(*m, *tree, *store, params);
  g_lastSplatClamped = stats.texelsClamped;
  return stats.texelsTouched;
}

/* texelsClamped of the most recent Mesh_vdmSplatDab on this thread. */
int Vdm_lastSplatClamped()
{
  return g_lastSplatClamped;
}

/* The interactive splat: brackets the dab in a store tile-delta and appends a
 * VdmLogChunk to `log`'s OPEN step, so the stroke's single undo press reverts
 * the dab's texels (self-inverse delta; GPU-dirty marks ride applyDelta). */
int Mesh_vdmSplatDabLogged(mesh::Mesh *m,
                           spatial::SpatialTree *tree,
                           vdm::VdmStore *store,
                           meshlog::MeshLog *log,
                           float cx,
                           float cy,
                           float cz,
                           float nx,
                           float ny,
                           float nz,
                           float radius,
                           float strength,
                           float alpha,
                           int invert)
{
  if (!m || !tree || !store) {
    g_lastSplatClamped = 0;
    return 0;
  }
  store->beginDelta();
  int n = Mesh_vdmSplatDab(m, tree, store, cx, cy, cz, nx, ny, nz, radius, strength,
                           alpha, invert);
  vdm::VdmDelta *delta = store->endDelta();
  if (delta) {
    if (log) {
      auto *chunk = litestl::alloc::New<vdm::VdmLogChunk>(
          "VdmLogChunk", store, std::move(*delta));
      log->appendChunk(chunk);
    }
    litestl::alloc::Delete(delta);
  }
  return n;
}

/* Serialize the store (v2 container) into a freshly-allocated buffer
 * (*out_size = byte count; free with freeMeshBuffer). Undo seam for the
 * app's store-delete op. */
uint8_t *VdmStore_serialize(vdm::VdmStore *store, int *out_size)
{
  *out_size = 0;
  if (!store) {
    return nullptr;
  }
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  if (!store->write(ss)) {
    return nullptr;
  }
  std::string s = ss.str();
  uint8_t *buf = static_cast<uint8_t *>(litestl::alloc::alloc("vdm store buffer", s.size()));
  std::memcpy(buf, s.data(), s.size());
  *out_size = int(s.size());
  return buf;
}

/* Rebuild a store from a VdmStore_serialize blob (params — backend, tile
 * size, resolution, Ptex grid table + adjacency — all ride the v2 payload).
 * Returns nullptr on parse failure. */
vdm::VdmStore *VdmStore_deserialize(const uint8_t *data, int size)
{
  if (!data || size <= 0) {
    return nullptr;
  }
  std::string s(reinterpret_cast<const char *>(data), size_t(size));
  std::stringstream ss(s, std::ios::in | std::ios::out | std::ios::binary);
  vdm::VdmStore *store = litestl::alloc::New<vdm::VdmStore>("VdmStore");
  if (!store->read(ss)) {
    litestl::alloc::Delete(store);
    return nullptr;
  }
  return store;
}
}
