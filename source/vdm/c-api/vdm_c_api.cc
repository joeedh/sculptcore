#include "mesh/mesh.h"
#include "spatial/spatial.h"
#include "vdm/vdm_splat.h"
#include "vdm/vdm_store.h"

#include "litestl/util/alloc.h"

using namespace sculptcore;

extern "C" {

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
    return 0;
  }
  vdm::VdmSplatParams params;
  params.center = litestl::math::float3(cx, cy, cz);
  params.normal = litestl::math::float3(nx, ny, nz);
  params.radius = radius;
  params.strength = strength;
  params.alpha = alpha;
  params.invert = invert != 0;
  return vdm::splatDab(*m, *tree, *store, params).texelsTouched;
}
}
