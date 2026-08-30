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

  auto result = triangulate(*m, IndexRange(0, m->f.count), tris);

  if (!result) {
    result.printError();
    return nullptr;
  }

// #define TRI_DRAW_LINES
#ifdef TRI_DRAW_LINES
  int totalVerts = tris.size() * 6;
#else
  int totalVerts = tris.size() * 3;
#endif

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

  // printf("\n\n");

#ifdef TRI_DRAW_LINES
  for (auto &tri : tris) {
    float3 &co1 = co[tri.v[0]];
    float3 &co2 = co[tri.v[1]];
    float3 &co3 = co[tri.v[2]];
    float3 no = triNormal(co1, co2, co3);

    for (int i = 0; i < 3; i++) {
      aPos[index] = co[tri.v[i]];
      aNo[index] = no;
      index++;
      aPos[index] = co[tri.v[(i + 1) % 3]];
      aNo[index] = no;
      index++;
    }
  }

  DrawCommand *cmd = mgr->createCommand(
      batch, GPUCmdType::DRAW_LINES, nullptr, 0, totalVerts, totalVerts / 2);

#else
  for (auto &tri : tris) {
    float3 &co1 = co[tri.v[0]];
    float3 &co2 = co[tri.v[1]];
    float3 &co3 = co[tri.v[2]];
    float3 triNo = triNormal(co1, co2, co3);

    float3 no1 = no[tri.v[0]];
    float3 no2 = no[tri.v[1]];
    float3 no3 = no[tri.v[2]];

    no1 = no2 = no3 = triNo; // m->f.no[tri.f];
    // printf("%d %d %d\n", tri.v[0], tri.v[1], tri.v[2]);

    aPos[index] = co1;
    aPos[index + 1] = co2;
    aPos[index + 2] = co3;
    aNo[index] = no1;
    aNo[index + 1] = no2;
    aNo[index + 2] = no3;

    index += 3;
  }

  DrawCommand *cmd = mgr->createCommand(
      batch, GPUCmdType::DRAW_TRIS, nullptr, 0, totalVerts, totalVerts / 3);

#endif

  cmd->attrs.append(posBuf);
  cmd->attrs.append(noBuf);

  return batch;
}
} // namespace sculptcore::mesh::gpu