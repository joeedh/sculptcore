#include "spatial.h"

#include "node.h"

#include "litestl/math/vector.h"
#include "litestl/util/map.h"
#include "litestl/util/task.h"
#include "litestl/util/vector.h"

#include "mesh/mesh.h"
#include "mesh/mesh_proxy.h"

#include "gpu/standard_attrs.h"
#include "gpu/vbo.h"

#include <cmath>

using namespace litestl::util;
using namespace litestl::math;
using namespace sculptcore::mesh;
using namespace sculptcore::gpu;
using namespace litestl;

namespace sculptcore::spatial {
SpatialNode::NodeData::GPUData::~GPUData()
{
  if (vbo) {
    alloc::Delete<VBO>(vbo);
  }
}

void SpatialTree::regen_node_gpu_buffers(SpatialNode *node)
{
  using GPUData = SpatialNode::NodeData::GPUData;

  node->data->gpu.~GPUData();

  ensure_node_tris(node);

  int tottri = node->data->tris.size();
  node->data->gpu.vbo = alloc::New<VBO>("VBO", tottri * 3);

  update_node_gpu_buffers(node);
}

void SpatialTree::update_node_gpu_buffers(SpatialNode *node)
{
  node->flag &= ~Spatial_UpdateGPU;

  int tottri = node->data->tris.size();

  VBO *vbo = node->data->gpu.vbo;
  float3 *pos =
      vbo->ensure<float3>(GPU_STD_ATTR_NAME(pos), tottri * 3)->get_data<float3>(true);
  float3 *nor =
      vbo->ensure<float3>(GPU_STD_ATTR_NAME(nor), tottri * 3)->get_data<float3>(true);

  auto &tris = node->data->tris;
  Mesh *m = this->m;

  task::parallel_for(
      util::IndexRange(tris.size()),
      [pos, nor, m, &tris](util::IndexRange range) {
        for (int i : range) {
          int vert_i = i * 3;
          auto &tri = tris[i];

          int c = tri.c[i];
          int v = m->c.v[c];

          pos[vert_i] = m->v.co[v];
          nor[vert_i] = m->v.no[v];
        }
      },
      1024);
}

} // namespace sculptcore::spatial