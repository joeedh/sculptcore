#include "mesh_drawbatch.h"
#include "../utils/triangulate.h"
#include "litestl/math/geom.h"

using namespace litestl::math;
using namespace litestl::util;

namespace sculptcore::mesh::gpu {

using namespace sculptcore::gpu;

DrawBatch *MeshBatchManager::createMeshBatch(sculptcore::gpu::GPUManager *mgr)
{
  Vector<Tri> tris;

  auto result = triangulate(*m, IndexRange(m->f.count), tris);

  if (!result) {
    result.printError();
    return nullptr;
  }

  int totalVerts = tris.size() * 3;

  DrawBatch *batch = mgr->createBatch();

  Buffer *posBuf = mgr->createBuffer(
      litestl::util::string("position"), GPUType::FLOAT32, 3, totalVerts);
  Buffer *noBuf =
      mgr->createBuffer(litestl::util::string("normal"), GPUType::FLOAT32, 3, totalVerts);
  batch->buffers.append(posBuf);
  batch->buffers.append(noBuf);

  auto &co = m->v.co;
  auto &no = m->v.no;

  float3 *aPos = posBuf->get_data<float3>();
  float3 *aNo = noBuf->get_data<float3>();
  int index = 0;

  for (auto &tri : tris) {
    float3 &co1 = co[tri.v[0]];
    float3 &co2 = co[tri.v[1]];
    float3 &co3 = co[tri.v[2]];
    float3 no = triNormal(co1, co2, co3);

    aPos[index] = co1;
    aPos[index + 1] = co2;
    aPos[index + 2] = co3;
    aNo[index] = no;
    aNo[index + 1] = no;
    aNo[index + 2] = no;

    index += 3;
  }

  DrawCommand *cmd = mgr->createCommand(
      batch, GPUCmdType::DRAW_TRIS, nullptr, 0, totalVerts, totalVerts / 3);

  cmd->attrs.append(posBuf);
  cmd->attrs.append(noBuf);

  return batch;
}
} // namespace sculptcore::mesh::gpu