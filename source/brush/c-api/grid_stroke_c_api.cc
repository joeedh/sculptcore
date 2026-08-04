/** Grids-native stroke c-api (grids-native brush path, G3): the session
 * surface a host (the Blender addon, the debug app) drives instead of the
 * materialized-mesh executor when the active tool is grids-capable. A session
 * wraps one GridBrushExecutor + GridStrokeLog over one (Multires, level);
 * GridStroke_sync re-binds after any Multires fold point (mesh-path
 * writeback, level ops) dropped the domain — begin() calls it implicitly. */

#include "brush/brush.h"
#include "brush/grid_executor.h"
#include "subdiv/grid_domain.h"
#include "subdiv/grid_stroke_log.h"
#include "subdiv/grid_tree.h"
#include "subdiv/multires.h"

#include "litestl/util/alloc.h"

using namespace sculptcore;

struct GridStrokeSession {
  subdiv::Multires *mr = nullptr;
  int level = 0;
  subdiv::GridStrokeLog log;
  brush::GridBrushExecutor exec;

  GridStrokeSession(subdiv::Multires *mr, int level, brush::Brush *b)
      : mr(mr), level(level), exec(mr->gridDomain(level), b, &log)
  {
  }
};

extern "C" {

/** Open a grids stroke session for `level` of `mr` (both outlive the session;
 * `brush` is the host's engine brush). Builds the domain/tree on demand. */
GridStrokeSession *GridStroke_new(subdiv::Multires *mr, int level, brush::Brush *brush)
{
  if (!mr || !brush || level < 1 || level > mr->maxLevel()) {
    return nullptr;
  }
  return litestl::alloc::New<GridStrokeSession>("grid stroke session", mr, level, brush);
}

void GridStroke_free(GridStrokeSession *s)
{
  if (s) {
    litestl::alloc::Delete(s);
  }
}

/** Whether `tool` (a SculptBrushes id) runs grids-native; 0 means the host
 * must fall back to the materialized-mesh path for the stroke. */
int GridStroke_supported(int tool)
{
  return brush::GridBrushExecutor::supportsBrush(brush::SculptBrushes(tool)) ? 1 : 0;
}

/** Re-bind to the current domain after a fold point (drops undo history when
 * the domain was rebuilt — the host's blob fallback covers those seams).
 * Returns 1; 0 when the session's level no longer exists. */
int GridStroke_sync(GridStrokeSession *s)
{
  if (!s || s->level < 1 || s->level > s->mr->maxLevel()) {
    return 0;
  }
  subdiv::GridLevelDomain *d = s->mr->gridDomain(s->level);
  if (d != s->exec.domain) {
    s->exec.attach(d);
  }
  return 1;
}

int GridStroke_begin(GridStrokeSession *s)
{
  if (!s || !GridStroke_sync(s)) {
    return 0;
  }
  s->exec.beginStep();
  return 1;
}

/** One dab; returns the moved-vert count. `grabAdd` mirrors
 * CommandExecutor::setGrabAccumAdd (0 = primary symmetry image, 1 = mirror
 * image of the same dab); ignored by non-grab tools. */
int GridStroke_dab(GridStrokeSession *s,
                   int tool,
                   float ox, float oy, float oz,
                   float nx, float ny, float nz,
                   int grabAdd)
{
  if (!s) {
    return 0;
  }
  s->exec.setGrabAccumAdd(grabAdd != 0);
  return s->exec.applyDab(brush::SculptBrushes(tool), float3(ox, oy, oz),
                          float3(nx, ny, nz));
}

void GridStroke_end(GridStrokeSession *s)
{
  if (s) {
    s->exec.endStep();
  }
}

int GridStroke_canUndo(GridStrokeSession *s)
{
  return s && s->log.canUndo() ? 1 : 0;
}

int GridStroke_canRedo(GridStrokeSession *s)
{
  return s && s->log.canRedo() ? 1 : 0;
}

int GridStroke_undo(GridStrokeSession *s)
{
  return s && s->log.undo() ? 1 : 0;
}

int GridStroke_redo(GridStrokeSession *s)
{
  return s && s->log.redo() ? 1 : 0;
}

/** Captured undo bytes across the session's history. */
double GridStroke_undoBytes(GridStrokeSession *s)
{
  return s ? double(s->log.bytes()) : 0.0;
}

/** Raycast the grids domain of (mr, level): closest forward hit. Returns 1 on
 * hit with out10 = {p.xyz, normal.xyz, t, grid, cellU, cellV} and
 * *nearestVert = the dense level vert id; 0 on miss. */
int GridTree_castRay(subdiv::Multires *mr,
                     int level,
                     float ox, float oy, float oz,
                     float dx, float dy, float dz,
                     float *out10,
                     int *nearestVert)
{
  if (!mr || level < 1 || level > mr->maxLevel() || !out10) {
    return 0;
  }
  subdiv::GridLevelDomain *d = mr->gridDomain(level);
  subdiv::GridTree *tree = d->ensureTree();
  subdiv::GridRayHit hit;
  if (!tree->castRay(float3(ox, oy, oz), float3(dx, dy, dz), hit)) {
    return 0;
  }
  out10[0] = hit.p[0];
  out10[1] = hit.p[1];
  out10[2] = hit.p[2];
  out10[3] = hit.normal[0];
  out10[4] = hit.normal[1];
  out10[5] = hit.normal[2];
  out10[6] = hit.t;
  out10[7] = float(hit.grid);
  out10[8] = float(hit.cellU);
  out10[9] = float(hit.cellV);
  if (nearestVert) {
    *nearestVert = hit.nearestVert;
  }
  return 1;
}
}
