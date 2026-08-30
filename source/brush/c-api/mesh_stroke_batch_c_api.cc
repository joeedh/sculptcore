/** Mesh-path spaced-dab batch c-api: the host's per-dab loop (raycast, prop
 * cycle, node filter, apply, symmetry) as flat batch calls over an existing
 * CommandExecutor + SpatialTree, mirroring what the Blender addon's Python
 * loop does per dab through the reflected bindings (stroke.apply_dab). The
 * grids-native equivalents live in grid_stroke_c_api.cc; grab-class,
 * anchored, and dyntopo strokes stay host-side. */

#include "brush/brush.h"
#include "brush/brush_executor.h"
#include "mesh/mesh.h"
#include "props/prop_dynamics.h"
#include "spatial/spatial.h"

#include <cstdint>

using namespace sculptcore;

extern "C" {

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
  if (!exec || !tree || !m || !b) {
    return -1;
  }
  litestl::util::Vector<spatial::SpatialNode *> nodes;
  auto oneImage = [&](float3 center, float3 normal, float radius) {
    nodes.clear();
    if (!tree->filterNodes(center, radius * filterMul, nodes)) {
      return 0;
    }
    exec->setGrabAccumAdd(false);
    exec->execBrush(m, brush::SculptBrushes(tool), &nodes, center, normal);
    exec->clearIsFirstOfStep();
    int count = int(nodes.size());
    // Node handles die here: updateQueries may split or merge leaves.
    tree->updateQueries();
    return count;
  };
  int total = 0;
  for (int i = 0; i < n; i++) {
    const float *d = dabs + i * 7;
    // Rewritten every dab: loadProps assigns post-dynamics values back into
    // the Brush fields (see mapping.apply_dab_state host-side).
    b->strength = strength;
    b->radius = d[6];
    b->invert = invert != 0;
    b->writeProps();
    if (usePressure) {
      b->clearDeviceInputs();
      b->pushDeviceInput(int(props::DeviceType::PRESSURE), pressure);
    }
    float3 center(d[0], d[1], d[2]);
    float3 normal(d[3], d[4], d[5]);
    total += oneImage(center, normal, d[6]);
    for (int mi = 0; mi < mirrorCount; mi++) {
      const float *sg = signs + mi * 3;
      total += oneImage(float3(d[0] * sg[0], d[1] * sg[1], d[2] * sg[2]),
                        float3(d[3] * sg[0], d[4] * sg[1], d[5] * sg[2]),
                        d[6]);
    }
  }
  return total;
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
  if (!exec || !tree || !m || !b || !prog) {
    return -1;
  }
  litestl::util::Vector<spatial::SpatialNode *> nodes;
  auto oneImage = [&](float3 center, float3 normal, float radius) {
    nodes.clear();
    if (!tree->filterNodes(center, radius * filterMul, nodes)) {
      return 0;
    }
    exec->setGrabAccumAdd(false);
    exec->execProgram(prog, &nodes, center, normal);
    exec->clearIsFirstOfStep();
    int count = int(nodes.size());
    tree->updateQueries();
    return count;
  };
  int total = 0;
  for (int i = 0; i < n; i++) {
    const float *d = dabs + i * 7;
    b->strength = strength;
    b->radius = d[6];
    b->invert = invert != 0;
    b->writeProps();
    if (usePressure) {
      b->clearDeviceInputs();
      b->pushDeviceInput(int(props::DeviceType::PRESSURE), pressure);
    }
    float3 center(d[0], d[1], d[2]);
    float3 normal(d[3], d[4], d[5]);
    total += oneImage(center, normal, d[6]);
    for (int mi = 0; mi < mirrorCount; mi++) {
      const float *sg = signs + mi * 3;
      total += oneImage(float3(d[0] * sg[0], d[1] * sg[1], d[2] * sg[2]),
                        float3(d[3] * sg[0], d[4] * sg[1], d[5] * sg[2]),
                        d[6]);
    }
  }
  return total;
}
}
