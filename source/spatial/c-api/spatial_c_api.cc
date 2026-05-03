#include "litestl/util/alloc.h"
#include "mesh/mesh.h"
#include "spatial/spatial.h"

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
}
