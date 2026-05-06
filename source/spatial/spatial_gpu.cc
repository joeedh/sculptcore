
#include "gpu/types.h"
#include "spatial.h"

#include "node.h"

#include "litestl/math/vector.h"
#include "litestl/util/task.h"
#include "litestl/util/vector.h"

#include "gpu/manager.h"
#include "gpu/vbo.h"

using namespace litestl::util;
using namespace litestl::math;
using namespace sculptcore::mesh;
using namespace sculptcore::gpu;
using namespace litestl;

namespace sculptcore::spatial {
void SpatialTree::regen_node_gpu_buffers(SpatialNode *node, gpu::GPUManager *gpu)
{
  using GPUData = SpatialNode::NodeData::GPUData;
  node->flag &= ~Spatial_RegenGPU;

  node->data->gpu.dispose();
  update_node_gpu_buffers(node, gpu);
}

void SpatialTree::update_node_gpu_buffers(SpatialNode *node, gpu::GPUManager *gpu)
{
  node->flag &= ~Spatial_UpdateGPU;
  bool smooth_shading = false;
  
  ensure_node_tris(node);
  int tottri = node->data->tris.size();
  int totvert = tottri * 3;

  if (!node->data->gpu.pos) {
    node->data->gpu.pos = gpu->createBuffer(
        litestl::util::string("position"), GPUType::FLOAT32, 3, totvert);
  }
  if (!node->data->gpu.nor) {
    node->data->gpu.nor =
        gpu->createBuffer(litestl::util::string("normal"), GPUType::FLOAT32, 3, totvert);
  }

  node->data->gpu.pos->resize(totvert);
  node->data->gpu.nor->resize(totvert);
  node->data->gpu.pos->update_buffer = true;
  node->data->gpu.nor->update_buffer = true;

  gpu::Buffer *posBuf = node->data->gpu.pos;
  gpu::Buffer *norBuf = node->data->gpu.nor;

  float3 *pos = posBuf->get_data<float3>();
  float3 *nor = norBuf->get_data<float3>();

  auto &tris = node->data->tris;
  Mesh *m = this->m;

#if 0
  task::parallel_for(
      util::IndexRange(tris.size()),
      [pos, nor, m, &tris](util::IndexRange range) {
        for (int i : range) {
          int vert_i = i * 3;
          auto &tri = tris[i];

          for (int j=0; j<3; j++, vert_i++) {
            int c = tri.c[j];
            int v = m->c.v[c];

            pos[vert_i] = m->v.co[v];
            if (smooth_shading) {
              nor[vert_i] = m->v.no[v];
            } else {
              nor[vert_i] = tri.nor;
            }
          }
        }
      },
      1024);
#else
  float3 no;

  for (int i : util::IndexRange(tris.size())) {
    int vert_i = i * 3;
    auto &tri = tris[i];

    if (!smooth_shading) {
      no = m->f.no[tri.f];
    }

    for (int j = 0; j < 3; j++, vert_i++) {
      int c = tri.c[j];
      int v = m->c.v[c];

      pos[vert_i] = m->v.co[v];
      nor[vert_i] = smooth_shading ? m->v.no[v] : no;
    }
  }

#endif
}

} // namespace sculptcore::spatial
