#pragma once
#include <cstdint>
namespace sculptcore {
namespace dyntopo {
struct DynTopoParams;
}
namespace brush {
struct Brush;
struct BrushProgram;
struct CommandExecutor;
} // namespace brush
namespace spatial {
struct SpatialTree;
}
namespace mesh {
struct Mesh;
}
} // namespace sculptcore

struct GridStrokeSession;
extern "C" {
int MeshStroke_dabProgramResolvedDyntopo(sculptcore::brush::CommandExecutor *,
                                         sculptcore::brush::BrushProgram *,
                                         float,
                                         float,
                                         float,
                                         float,
                                         float,
                                         float,
                                         float,
                                         sculptcore::dyntopo::DynTopoParams *,
                                         uint32_t);
int MeshStroke_dabResolved(
    sculptcore::brush::CommandExecutor *, int, float, float, float, float, float, float);
int MeshStroke_dabResolvedImage(sculptcore::brush::CommandExecutor *,
                                int,
                                float,
                                float,
                                float,
                                float,
                                float,
                                float,
                                int);
int MeshStroke_dabProgramResolvedImage(sculptcore::brush::CommandExecutor *,
                                       sculptcore::brush::BrushProgram *,
                                       float,
                                       float,
                                       float,
                                       float,
                                       float,
                                       float,
                                       int);
int MeshStroke_dabProgramResolved(sculptcore::brush::CommandExecutor *,
                                  sculptcore::brush::BrushProgram *,
                                  float,
                                  float,
                                  float,
                                  float,
                                  float,
                                  float);
int MeshStroke_dabBatchInputs(sculptcore::brush::CommandExecutor *,
                              sculptcore::spatial::SpatialTree *,
                              sculptcore::mesh::Mesh *,
                              sculptcore::brush::Brush *,
                              int,
                              int,
                              const float *,
                              float,
                              const float *,
                              float,
                              const float *,
                              int,
                              int);
int MeshStroke_dabBatchProgramInputs(sculptcore::brush::CommandExecutor *,
                                     sculptcore::spatial::SpatialTree *,
                                     sculptcore::mesh::Mesh *,
                                     sculptcore::brush::Brush *,
                                     sculptcore::brush::BrushProgram *,
                                     int,
                                     const float *,
                                     float,
                                     const float *,
                                     float,
                                     const float *,
                                     int,
                                     int);
int GridStroke_supportsResolved(GridStrokeSession *,
                                int,
                                sculptcore::brush::BrushProgram *);
int GridStroke_dabResolved(
    GridStrokeSession *, int, float, float, float, float, float, float);
int GridStroke_dabResolvedImage(
    GridStrokeSession *, int, float, float, float, float, float, float, int);
int GridStroke_dabProgramResolved(GridStrokeSession *,
                                  sculptcore::brush::BrushProgram *,
                                  float,
                                  float,
                                  float,
                                  float,
                                  float,
                                  float);
int GridStroke_dabBatchInputs(GridStrokeSession *,
                              int,
                              int,
                              const float *,
                              float,
                              const float *,
                              const float *,
                              int,
                              int);
int GridStroke_dabBatchProgramInputs(GridStrokeSession *,
                                     sculptcore::brush::BrushProgram *,
                                     int,
                                     const float *,
                                     float,
                                     const float *,
                                     const float *,
                                     int,
                                     int);
}
