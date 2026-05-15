#include "bindings.h"
#include "node.h"
#include "spatial.h"

#include "gpu/batch.h"
#include "gpu/command.h"
#include "gpu/manager.h"
#include "gpu/vbo.h"
#include "litestl/binding/binding.h"
#include "litestl/math/math_bindings.h"
#include "shaders/spatial_shaders.h"

using namespace litestl::binding;

namespace sculptcore::spatial {

const types::Struct<SpatialNode> *SpatialNode::defineBindings()
{
  using namespace litestl::binding;
  types::Struct<SpatialNode> *st = new types::Struct<SpatialNode>(
      "sculptcore::spatial::SpatialNode", sizeof(SpatialNode));

  BIND_STRUCT_MEMBER(st, aabb);
  BIND_STRUCT_MEMBER(st, flag);
  BIND_STRUCT_MEMBER(st, id);
  BIND_STRUCT_MEMBER(st, debugIdOffset);

  return st;
}

types::Struct<SpatialTree> *SpatialTree::defineBindings()
{
  using namespace litestl::binding;
  types::Struct<SpatialTree> *st = new types::Struct<SpatialTree>(
      "sculptcore::spatial::SpatialTree", sizeof(SpatialTree));

  BIND_STRUCT_CONSTRUCTOR(st, "main", mesh::Mesh *);

  BIND_STRUCT_MEMBER(st, leaf_limit);
  BIND_STRUCT_MEMBER(st, depth_limit);

  BIND_STRUCT_METHOD(st, setup, MARGS());
  BIND_STRUCT_METHOD(st, add_face, MARGS("face"));
  BIND_STRUCT_METHOD(st, split_node, MARGS("node"));
  BIND_STRUCT_METHOD(st, node_from_id, MARGS("id"));
  BIND_STRUCT_METHOD(st, leaves, MARGS());
  BIND_STRUCT_METHOD(st, ensure_node_tris, MARGS("node"));
  BIND_STRUCT_METHOD(st, buildAll, MARGS());
  BIND_STRUCT_METHOD(st, buildLeafBoundsBatch, MARGS("batch"));
  BIND_STRUCT_METHOD(st, update, MARGS("gpu"));
  BIND_STRUCT_METHOD(st, getDrawBatch, MARGS());
  BIND_STRUCT_METHOD(st, castRay, MARGS("orig", "dir", "out"));
  BIND_STRUCT_METHOD(st, filterNodes, MARGS("co", "radius", "out"));
  return st;
}

void registerBindings(BindingManager &manager)
{
  using namespace litestl::binding;

  manager.add(Bind<Vector<SpatialNode *>>());
  manager.add(Bind<SpatialNode>());
  manager.add(Bind<SpatialTree>());
  manager.add(Bind<NodeFlags>());
  manager.add(Bind<SpatialShaders>());
}
} // namespace sculptcore::spatial
