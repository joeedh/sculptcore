#pragma once

// Native engine factory free-functions (extern "C"; defined in the linked
// mesh/spatial libs — source/mesh/mesh_shapes.cc, source/spatial/c-api/
// spatial_c_api.cc). void* stands in for the opaque Mesh*/SpatialTree* — ABI
// identical for an extern "C" pointer. Shared across the napi_*.cc
// translation units that call into these seams; each such file includes this
// header rather than repeating the declarations.

#include <cstddef>
#include <cstdint>

#include "litestl/util/vector.h"

extern "C" {

size_t LSTL_GetMemSize(bool includePermanent);
void LSTL_PrintAllocBlocks(bool includePermanent);
void LSTL_FreeFormatBlocks(char *s);
char *LSTL_FormatBlock(void *mem);
char *LSTL_FormatBlocks(bool printPermanent);

void IntVector_assign(litestl::util::Vector<int> *vec, const int *data, int count);
void FloatVector_assign(litestl::util::Vector<float> *vec, const float *data, int count);

void *Mesh_createCube(int dimen, float size, float sphereFac);
void *Mesh_makeGrid(int nx, int ny, float size);
void *Mesh_makeUVSphere(int rings, int segs, float radius);
void *Mesh_buildSpatialTree(void *mesh, int leafLimit, int depthLimit, int gpuTriTarget);
void SpatialTree_free(void *tree);
void Mesh_free(void *mesh);
void Mesh_triangulate(void *mesh);
// Feature-aligned quad remesh (source/remesh/c-api/remesh_c_api.cc). Returns a
// new Mesh* (input untouched); null on clean failure.
void *Mesh_quadRemesh(void *mesh, void *params);
// Versioned, lz4hc-compressed mesh blob (source/mesh/c-api/mesh_c_api.cc).
uint8_t *serializeMesh(void *mesh, int *out_size);
// Uncompressed column payload only (autosave worker compresses off-thread).
uint8_t *serializeMeshRaw(void *mesh, int *out_size);
void *deserializeMesh(const uint8_t *data, int size);
void freeMeshBuffer(uint8_t *buf);
// M5 requested-attribute bridge (source/spatial/c-api/spatial_c_api.cc).
void setTreeRequestedAttrs(void *tree,
                           int count,
                           const char *namesJoined,
                           const int *srcTypes,
                           const int *elemSizes,
                           const int *slots,
                           const int *domains,
                           const int *defaultKinds);
void setTreeDrawShader(void *tree, const char *wgsl);
int getTreeMissingAttrSlots(void *tree, int *out, int maxOut);
void refreshTreeRequestedAttrs(void *tree);
// GPU brush-stroke seam (source/brush/c-api/gpu_brush_c_api.cc).
void *GpuBrush_beginStroke(void *mesh, void *tree, void *brush, void *meshLog, int tool);
void GpuBrush_free(void *session);
const char *GpuBrush_kernelName(void *session);
int GpuBrush_info(void *session, int which);
int GpuBrush_marshalDab(void *session,
                        float cx,
                        float cy,
                        float cz,
                        float nx,
                        float ny,
                        float nz,
                        float radius,
                        float filterRadius,
                        int mirrorIdx,
                        int nonaccum);
int GpuBrush_dataSize(void *session, int which);
const void *GpuBrush_dataPtr(void *session, int which);
void GpuBrush_applyCo(void *session, const float *co, int elemCount);
void GpuBrush_endStroke(void *session, const float *co, const float *no, int elemCount);
// VDM engine seam (source/vdm/c-api/vdm_c_api.cc).
void *VdmStore_new(int resolution, int tileSize);
void VdmStore_free(void *store);
int Mesh_vdmSplatDab(void *mesh,
                     void *tree,
                     void *store,
                     float cx,
                     float cy,
                     float cz,
                     float nx,
                     float ny,
                     float nz,
                     float radius,
                     float strength,
                     float alpha,
                     int invert);
int Mesh_vdmSplatDabLogged(void *mesh,
                           void *tree,
                           void *store,
                           void *log,
                           float cx,
                           float cy,
                           float cz,
                           float nx,
                           float ny,
                           float nz,
                           float radius,
                           float strength,
                           float alpha,
                           int invert);
uint8_t *VdmStore_serialize(void *store, int *out_size);
int Mesh_vdmApplyToVerts(void *mesh, void *store, int clearStore);
int VdmStore_restoreBlob(void *store, const uint8_t *data, int size);
void *VdmStore_deserialize(const uint8_t *data, int size);
int Vdm_lastSplatClamped();
void SpatialTree_fillDetailCarrier(void *tree, int carrier);
void Mesh_updateFrames(void *mesh);
// Sculpt-layer settings mutators (source/displace/c-api/displace_c_api.cc).
void Mesh_layerSetWeight(void *mesh, int li, float weight);
void Mesh_layerSetEnabled(void *mesh, int li, int enabled);
void Mesh_layerSetFrozen(void *mesh, int li, int frozen);
void Mesh_layerRemove(void *mesh, int li);
int Mesh_setActiveEditLayer(void *mesh, int li);
void Mesh_layerFold(void *mesh);
// Multires seam (source/subdiv/c-api/subdiv_c_api.cc).
void *
Multires_new(void *cage, int levels, int leafLimit, int depthLimit, int gpuTriTarget);
void Multires_free(void *mr);
int Multires_setActiveLevel(void *mr, int level);
void *Multires_activeMesh(void *mr);
void *Multires_activeTree(void *mr);
int Multires_writeback(void *mr, int level);
int Multires_downRefit(void *mr, int level);
uint8_t *Multires_serializeStore(void *mr, int *out_size);
int Multires_captureToVdm(void *mr, void *vstore, int level);
int Multires_restoreStore(void *mr, const uint8_t *data, int size);
}
