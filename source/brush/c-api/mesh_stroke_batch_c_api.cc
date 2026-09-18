/** Mesh-path spaced-dab batch c-api: the host's per-dab loop (raycast, prop
 * cycle, node filter, apply, symmetry) as flat batch calls over an existing
 * CommandExecutor + SpatialTree, mirroring what the Blender addon's Python
 * loop does per dab through the reflected bindings (stroke.apply_dab). The
 * grids-native equivalents live in grid_stroke_c_api.cc; grab-class,
 * anchored, and dyntopo strokes stay host-side. */

#include "brush/brush.h"
#include "brush/brush_executor.h"
#include "dab_inputs.h"
#include "mesh/mesh.h"
#include "props/prop_dynamics.h"
#include "spatial/spatial.h"
#include "stroke_inputs.h"

#include <cstdint>
#include <optional>

using namespace sculptcore;

extern "C" {

int MeshStroke_dabProgramResolvedDyntopo(brush::CommandExecutor *exec,
                                         brush::BrushProgram *program,
                                         float x,
                                         float y,
                                         float z,
                                         float nx,
                                         float ny,
                                         float nz,
                                         float radius,
                                         dyntopo::DynTopoParams *params,
                                         uint32_t seed)
{
  if (!exec || !std::isfinite(radius) || radius < 0)
    return -1;
  const auto result = exec->applyResolvedProgram(
      program, float3(x, y, z), float3(nx, ny, nz), false, false, params, radius, seed);
  if (result.error != props::PropError::ERROR_NONE)
    return brush::reportStrokeInputFailure(result);
  return params ? exec->lastDynTopoStats.splits + exec->lastDynTopoStats.collapses : 0;
}

int BrushProgram_replaceCavityCurveChecked(brush::BrushProgram *program,
                                           int command,
                                           util::Vector<float> *samples)
{
  if (!program || !samples)
    return int(props::PropError::ERROR_INVALID_OWNER);
  return program->replaceCommandCavityCurveChecked(command, *samples);
}

int BrushProgram_removeCavityCurveChecked(brush::BrushProgram *program, int command)
{
  if (!program)
    return int(props::PropError::ERROR_INVALID_OWNER);
  return program->removeCommandCavityCurveChecked(command);
}

int BrushProgram_replaceResponseDynamicsChecked(brush::BrushProgram *program,
                                                int command,
                                                const char *name,
                                                int type,
                                                util::Vector<int> *devices,
                                                util::Vector<int> *modes,
                                                util::Vector<float> *factors,
                                                util::Vector<int> *enabled,
                                                util::Vector<int> *offsets,
                                                util::Vector<float> *samples,
                                                util::Vector<int> *kinds,
                                                util::Vector<double> *parameters)
{
  if (!program || !name || !devices || !modes || !factors || !enabled || !offsets ||
      !samples || !kinds || !parameters)
    return int(props::PropError::ERROR_INVALID_OWNER);
  return program->replaceCommandResponseDynamicsChecked(command,
                                                        util::string(name),
                                                        type,
                                                        *devices,
                                                        *modes,
                                                        *factors,
                                                        *enabled,
                                                        *offsets,
                                                        *samples,
                                                        *kinds,
                                                        *parameters);
}

int BrushProgram_removeDynamicsChecked(brush::BrushProgram *program,
                                       int command,
                                       const char *name,
                                       int type)
{
  if (!program || !name)
    return int(props::PropError::ERROR_INVALID_OWNER);
  return program->removeCommandDynamicsChecked(command, util::string(name), type);
}

int BrushProgram_setScalarChecked(
    brush::BrushProgram *program, int command, const char *name, int type, double value)
{
  if (!program || !name)
    return int(props::PropError::ERROR_INVALID_OWNER);
  return int(program->setCommandScalarChecked(
      command, util::string(name), props::Prop(type), value));
}

int MeshStroke_dabResolved(brush::CommandExecutor *exec,
                           int tool,
                           float x,
                           float y,
                           float z,
                           float nx,
                           float ny,
                           float nz)
{
  if (!exec || exec->applyResolvedDab(
                       brush::SculptBrushes(tool), float3(x, y, z), float3(nx, ny, nz))
                       .error != props::PropError::ERROR_NONE)
    return exec ? brush::reportStrokeInputFailure(exec->lastRegistration) : -1;
  return exec->lastDabNodeCount;
}

int MeshStroke_dabResolvedImage(brush::CommandExecutor *exec,
                                int tool,
                                float x,
                                float y,
                                float z,
                                float nx,
                                float ny,
                                float nz,
                                int grabAdd)
{
  if (!exec || exec->applyResolvedDab(brush::SculptBrushes(tool),
                                      float3(x, y, z),
                                      float3(nx, ny, nz),
                                      false,
                                      grabAdd != 0)
                       .error != props::PropError::ERROR_NONE)
    return exec ? brush::reportStrokeInputFailure(exec->lastRegistration) : -1;
  return exec->lastDabNodeCount;
}

int MeshStroke_dabProgramResolved(brush::CommandExecutor *exec,
                                  brush::BrushProgram *program,
                                  float x,
                                  float y,
                                  float z,
                                  float nx,
                                  float ny,
                                  float nz)
{
  if (!exec ||
      exec->applyResolvedProgram(program, float3(x, y, z), float3(nx, ny, nz)).error !=
          props::PropError::ERROR_NONE)
    return exec ? brush::reportStrokeInputFailure(exec->lastRegistration) : -1;
  return exec->lastDabNodeCount;
}

int MeshStroke_dabProgramResolvedImage(brush::CommandExecutor *exec,
                                       brush::BrushProgram *program,
                                       float x,
                                       float y,
                                       float z,
                                       float nx,
                                       float ny,
                                       float nz,
                                       int grabAdd)
{
  if (!exec || exec->applyResolvedProgram(
                       program, float3(x, y, z), float3(nx, ny, nz), false, grabAdd != 0)
                       .error != props::PropError::ERROR_NONE)
    return exec ? brush::reportStrokeInputFailure(exec->lastRegistration) : -1;
  return exec->lastDabNodeCount;
}

/** Policy 0 preserves raw sources, 1 requires resolved execution, 2 selects by
 * capability. A rejected dab stops the batch; earlier successful dabs remain in its undo
 * transaction. */
static int meshDabInputs(brush::CommandExecutor *exec,
                         spatial::SpatialTree *tree,
                         mesh::Mesh *mesh,
                         brush::Brush *brush,
                         brush::BrushProgram *program,
                         int n,
                         const float *dabs,
                         float strength,
                         const float *inputs,
                         float filterMul,
                         const float *signs,
                         int mirrors,
                         int policy,
                         int tool)
{
  if (!exec || !tree || !mesh || !brush || exec->tree != tree || exec->brush != brush ||
      tree->m != mesh || policy < 0 || policy > 2 || !std::isfinite(filterMul) ||
      filterMul <= 0 || !brush::validDabInputs(n, dabs, strength, inputs, signs, mirrors))
    return -1;
  const auto type = brush::SculptBrushes(tool);
  bool resolved =
      policy == 1 || (policy == 2 && (program ? exec->supportsResolvedProgram(program)
                                              : exec->supportsResolved(type)));
  if (n == 0) {
    if (!resolved)
      return (program ? exec->preflightRawProgram(program) : exec->preflightRaw(type))
                 ? 0
                 : -1;
    auto result =
        program ? exec->applyResolvedProgram(program, float3(0.0f), float3(0.0f), true)
                : exec->applyResolvedDab(type, float3(0.0f), float3(0.0f), true);
    return result.error == props::PropError::ERROR_NONE ? 0 : -1;
  }
  litestl::util::Vector<spatial::SpatialNode *> nodes;
  auto image =
      [&](float3 center, float3 normal, float radius, const float *sign = nullptr) {
        std::optional<brush::DabFrameOverlay> frame;
        if (resolved && sign)
          frame.emplace(*brush, sign);
        if (resolved) {
          auto result =
              program
                  ? exec->applyResolvedProgram(
                        program, center, normal, false, sign != nullptr)
                  : exec->applyResolvedDab(type, center, normal, false, sign != nullptr);
          return result.error == props::PropError::ERROR_NONE
                     ? exec->lastDabNodeCount
                     : brush::reportStrokeInputFailure(result);
        }
        if (!(program ? exec->preflightRawProgram(program) : exec->preflightRaw(type)))
          return -1;
        nodes.clear();
        tree->filterNodes(center, radius * filterMul, nodes);
        exec->setGrabAccumAdd(false);
        if (program)
          exec->execProgram(program, &nodes, center, normal);
        else
          exec->execBrush(mesh, type, &nodes, center, normal);
        if (!exec->lastUniformValidationOk())
          return -1;
        const int count = int(nodes.size());
        exec->clearIsFirstOfStep();
        tree->updateQueries();
        return count;
      };
  int total = 0;
  for (int i = 0; i < n; i++) {
    const float *d = dabs + i * 7;
    brush::DabInputOverlay overlay(*brush);
    if (!overlay.valid())
      return -1;
    brush::setDabInputs(*brush, d[6], strength, inputs + i * 6);
    int result = image(float3(d[0], d[1], d[2]), float3(d[3], d[4], d[5]), d[6]);
    if (result < 0)
      return -1;
    total += result;
    for (int j = 0; j < mirrors; j++) {
      const float *s = signs + j * 3;
      result = image(float3(d[0] * s[0], d[1] * s[1], d[2] * s[2]),
                     float3(d[3] * s[0], d[4] * s[1], d[5] * s[2]),
                     d[6],
                     s);
      if (result < 0)
        return -1;
      total += result;
    }
    overlay.commit();
  }
  return total;
}

int MeshStroke_dabBatchInputs(brush::CommandExecutor *exec,
                              spatial::SpatialTree *tree,
                              mesh::Mesh *mesh,
                              brush::Brush *brush,
                              int tool,
                              int n,
                              const float *dabs,
                              float strength,
                              const float *inputs,
                              float filterMul,
                              const float *signs,
                              int mirrors,
                              int policy)
{
  return meshDabInputs(exec,
                       tree,
                       mesh,
                       brush,
                       nullptr,
                       n,
                       dabs,
                       strength,
                       inputs,
                       filterMul,
                       signs,
                       mirrors,
                       policy,
                       tool);
}

int MeshStroke_dabBatchProgramInputs(brush::CommandExecutor *exec,
                                     spatial::SpatialTree *tree,
                                     mesh::Mesh *mesh,
                                     brush::Brush *brush,
                                     brush::BrushProgram *program,
                                     int n,
                                     const float *dabs,
                                     float strength,
                                     const float *inputs,
                                     float filterMul,
                                     const float *signs,
                                     int mirrors,
                                     int policy)
{
  if (!program)
    return -1;
  return meshDabInputs(exec,
                       tree,
                       mesh,
                       brush,
                       program,
                       n,
                       dabs,
                       strength,
                       inputs,
                       filterMul,
                       signs,
                       mirrors,
                       policy,
                       0);
}

/** Batch raycast: `rays` = n x 6 {origin.xyz, dir.xyz}; on hit `out6` row i =
 * {p.xyz, normal.xyz} and `hit[i]` = 1. Returns the hit count. */
int MeshStroke_castBatch(
    spatial::SpatialTree *tree, int n, const float *rays, float *out6, uint8_t *hit)
{
  if (!tree) {
    return -1;
  }
  int count = 0;
  for (int i = 0; i < n; i++) {
    const float *r = rays + i * 6;
    spatial::CastRayIsect isect;
    if (tree->castRay(float3(r[0], r[1], r[2]), float3(r[3], r[4], r[5]), isect)) {
      float *o = out6 + i * 6;
      o[0] = isect.p[0];
      o[1] = isect.p[1];
      o[2] = isect.p[2];
      o[3] = isect.normal[0];
      o[4] = isect.normal[1];
      o[5] = isect.normal[2];
      hit[i] = 1;
      count++;
    } else {
      hit[i] = 0;
    }
  }
  return count;
}

/** Batch of plain spaced dabs on the materialized-mesh path. `dabs` = n x 7
 * {center.xyz, normal.xyz, radius}; `strength`/`invert` are constant for the
 * event (overlap attenuation and brush direction folded in by the host),
 * `pressure` refills the device sample once per logical dab when
 * `usePressure`, `filterMul` scales the node-filter radius (the kernel's
 * field radius: unboundedExtent for @unbounded kernels, else 1 — grab
 * latching never applies to this non-grab path), and `signs` = mirrorCount x
 * 3 reflection sign vectors. Per image: filterNodes at the reflected center,
 * execBrush, clearIsFirstOfStep, then updateQueries — the same cycle and
 * order as the host's per-dab loop. Returns total filtered-node count. */
int MeshStroke_dabBatch(brush::CommandExecutor *exec,
                        spatial::SpatialTree *tree,
                        mesh::Mesh *m,
                        brush::Brush *b,
                        int tool,
                        int n,
                        const float *dabs,
                        float strength,
                        int invert,
                        float pressure,
                        int usePressure,
                        float filterMul,
                        const float *signs,
                        int mirrorCount)
{
  if (n < 0 || n > INT_MAX / 7)
    return -1;
  litestl::util::Vector<float> samples;
  samples.resize(size_t(n) * 6);
  for (int i = 0; i < n; i++) {
    float *row = samples.data() + i * 6;
    row[0] = usePressure ? pressure : 0;
    row[1] = row[2] = row[3] = 0;
    row[4] = usePressure ? 1 : 0;
    row[5] = invert != 0;
  }
  return meshDabInputs(exec,
                       tree,
                       m,
                       b,
                       nullptr,
                       n,
                       dabs,
                       strength,
                       samples.data(),
                       filterMul,
                       signs,
                       mirrorCount,
                       0,
                       tool);
}

/** Batch of composite-program dabs on the materialized-mesh path:
 * MeshStroke_dabBatch's per-dab prop cycle with `execProgram` as the dab unit
 * (the host per-dab loop is stroke.py's `apply_dab_program`). Entry-level
 * overrides (autosmooth's pinned smooth strength/invert) supersede the
 * per-event `strength`/`invert` for their entry; `filterMul` is the main
 * kernel's field-radius multiplier (a chained BSMOOTH is never unbounded).
 * Same `dabs`/`signs` layout and return as MeshStroke_dabBatch. */
int MeshStroke_dabBatchProgram(brush::CommandExecutor *exec,
                               spatial::SpatialTree *tree,
                               mesh::Mesh *m,
                               brush::Brush *b,
                               brush::BrushProgram *prog,
                               int n,
                               const float *dabs,
                               float strength,
                               int invert,
                               float pressure,
                               int usePressure,
                               float filterMul,
                               const float *signs,
                               int mirrorCount)
{
  if (n < 0 || n > INT_MAX / 7)
    return -1;
  litestl::util::Vector<float> samples;
  samples.resize(size_t(n) * 6);
  for (int i = 0; i < n; i++) {
    float *row = samples.data() + i * 6;
    row[0] = usePressure ? pressure : 0;
    row[1] = row[2] = row[3] = 0;
    row[4] = usePressure ? 1 : 0;
    row[5] = invert != 0;
  }
  return meshDabInputs(exec,
                       tree,
                       m,
                       b,
                       prog,
                       n,
                       dabs,
                       strength,
                       samples.data(),
                       filterMul,
                       signs,
                       mirrorCount,
                       0,
                       0);
}
}
