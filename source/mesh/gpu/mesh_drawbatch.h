#include "../mesh.h"
#include "gpu/batch.h"
#include "gpu/command.h"
#include "gpu/manager.h"
#include "gpu/types.h"
#include "gpu/vbo.h"

namespace sculptcore::mesh::gpu {
struct MeshBatchManager {
  Mesh *m;

  MeshBatchManager(Mesh *mesh) : m(mesh) {};
  MeshBatchManager(const MeshBatchManager &) = delete;
  MeshBatchManager(MeshBatchManager &&) = delete;

  sculptcore::gpu::DrawBatch *createMeshBatch(sculptcore::gpu::GPUManager *gpuManager);

  static binding::types::Struct<MeshBatchManager> *defineBindings()
  {
    using binding::types::Struct;

    Struct<MeshBatchManager> *st = new Struct<MeshBatchManager>(
        "sculptcore::mesh::gpu::MeshBatchManager", sizeof(MeshBatchManager));
    BIND_STRUCT_MEMBER(st, m);
    BIND_STRUCT_CONSTRUCTOR(st, "main", Mesh *);
    BIND_STRUCT_METHOD(st, createMeshBatch, MARGS("gpuManager"));
    
    st->setNonNull("m");
    return st;
  }
};

FORWARD_CLS_BINDING(MeshBatchManager)

} // namespace sculptcore::mesh::gpu
