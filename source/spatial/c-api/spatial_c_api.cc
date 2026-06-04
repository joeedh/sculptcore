#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"
#include "gpu/gpu_attr_request.h"
#include "gpu/types.h"
#include "spatial/shaders/spatial_shaders.h"
#include "spatial/spatial.h"

#include <string>

using namespace sculptcore;

extern "C" {

spatial::SpatialTree *Mesh_buildSpatialTree(mesh::Mesh *m, int leafLimit, int depthLimit)
{
  spatial::SpatialTree *t = litestl::alloc::New<spatial::SpatialTree>("SpatialTree", m);
  if (leafLimit > 0) {
    t->leaf_limit = leafLimit;
  }
  if (depthLimit > 0) {
    t->depth_limit = depthLimit;
  }
  t->buildAll();
  return t;
}

void SpatialTree_free(spatial::SpatialTree *t)
{
  litestl::alloc::Delete<spatial::SpatialTree>(t);
}
spatial::SpatialShaders *getSpatialShaders()
{
  return &spatial::spatialShaders;
}

/* Install the material's requested attribute set on the tree (M5 bridge). The
 * per-attribute fields cross as flat parallel arrays + a single '\n'-joined name
 * string so the seam stays dependency-free and identical across the WASM and
 * native backends (the C++ RequestedAttr struct never crosses). `count` is the
 * number of attributes; each int array (when non-null) has `count` entries in
 * slot order. Never throws — a bad/empty set just yields an empty request (the
 * legacy single-color path). gpuType is always FLOAT32 (every requested attr is
 * a float vector); defaultKinds[i] != 0 => White fill, else Zero. */
void setTreeRequestedAttrs(spatial::SpatialTree *t,
                           int count,
                           const char *namesJoined,
                           const int *srcTypes,
                           const int *elemSizes,
                           const int *slots,
                           const int *domains,
                           const int *defaultKinds)
{
  if (!t) {
    return;
  }

  litestl::util::Vector<gpu::RequestedAttr> reqs;
  const char *p = namesJoined ? namesJoined : "";
  for (int i = 0; i < count; i++) {
    /* Slice out the i-th name (up to the next '\n' or the terminating NUL). */
    const char *start = p;
    while (*p && *p != '\n') {
      p++;
    }
    std::string nm(start, static_cast<size_t>(p - start));
    if (*p == '\n') {
      p++;
    }

    gpu::RequestedAttr r;
    r.name = nm.c_str();
    r.srcType = srcTypes ? srcTypes[i] : 0;
    r.gpuType = gpu::GPUType::FLOAT32;
    r.elemSize = elemSizes ? elemSizes[i] : 0;
    r.slot = slots ? slots[i] : i;
    r.domain = domains ? domains[i] : 1;
    r.defaultKind = (defaultKinds && defaultKinds[i]) ? gpu::AttrDefaultKind::White
                                                      : gpu::AttrDefaultKind::Zero;
    reqs.append(r);
  }

  t->setRequestedAttrs(reqs);
}

/* Set the WGSL source for the requested-attr draw shader (M5 bridge). The string
 * is copied inbound by the caller's marshalling; C++ rebuilds + links the tree
 * ShaderDef and flags leaves for a GPU regen. No-op-safe before
 * setTreeRequestedAttrs. */
void setTreeDrawShader(spatial::SpatialTree *t, const char *wgsl)
{
  if (!t || !wgsl) {
    return;
  }
  t->setDrawShader(wgsl);
}

/* Copy the advisory "missing" requested slots (those with no matching mesh
 * layer) into `out` (capacity `maxOut`); returns the full count. Pass out=null /
 * maxOut=0 to query the count only. Advisory — never throws. */
int getTreeMissingAttrSlots(spatial::SpatialTree *t, int *out, int maxOut)
{
  if (!t) {
    return 0;
  }
  litestl::util::Vector<int> &miss = t->getMissingAttrSlots();
  const int n = static_cast<int>(miss.size());
  if (out && maxOut > 0) {
    const int lim = n < maxOut ? n : maxOut;
    for (int i = 0; i < lim; i++) {
      out[i] = miss[i];
    }
  }
  return n;
}
}
