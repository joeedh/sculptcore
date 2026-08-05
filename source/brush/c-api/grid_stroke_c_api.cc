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
  /** Ride-along mirror into the resident slot mesh: per-dab moved verts, and
   * a full-level sync after undo/redo seeks. The Blender addon turns this on
   * so extdraw and mesh-path queries stay current; the debug app mirrors
   * itself. */
  bool mirror = false;
  subdiv::GridStrokeLog log;
  brush::GridBrushExecutor exec;

  GridStrokeSession(subdiv::Multires *mr, int level, brush::Brush *b)
      : mr(mr), level(level), exec(mr->gridDomain(level), b, &log)
  {
  }

  void mirrorAll()
  {
    subdiv::GridLevelDomain *d = exec.domain;
    litestl::util::Vector<int> all;
    all.resize(d->vertCount());
    for (int v = 0; v < d->vertCount(); v++) {
      all[v] = v;
    }
    brush::gridsMirrorToSlot(mr, level, std::span<const int>(all.data(), all.size()));
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

/** Ride-along mirror toggle (see GridStrokeSession::mirror). */
void GridStroke_setMirror(GridStrokeSession *s, int enable)
{
  if (s) {
    s->mirror = enable != 0;
  }
}

/** Stroke policy mirrors of CommandExecutor's setNonAccum/setAnchoredGrab —
 * set before begin(), like the mesh path. */
void GridStroke_setNonAccum(GridStrokeSession *s, int nonAccum)
{
  if (s) {
    s->exec.nonAccum = nonAccum != 0;
  }
}

void GridStroke_setAnchoredGrab(GridStrokeSession *s, int anchored)
{
  if (s) {
    s->exec.anchoredGrab = anchored != 0;
  }
}

/** Defer the touched-set normal refresh to GridStroke_flushNormals (host
 * frame cadence — closely-spaced dabs overlap ~90%, so per-dab refresh
 * recomputes the same fans many times). The mesh path's per-frame cadence. */
void GridStroke_setDeferNormals(GridStrokeSession *s, int defer)
{
  if (s) {
    s->exec.deferNormals = defer != 0;
  }
}

/** Refresh every deferred normal (and mirror the refreshed verts into the
 * slot mesh when mirroring is on). Call at the host's frame cadence;
 * GridStroke_end flushes implicitly. */
void GridStroke_flushNormals(GridStrokeSession *s)
{
  if (!s) {
    return;
  }
  auto &flushed = s->exec.flushNormals();
  if (s->mirror && flushed.size() > 0) {
    brush::gridsMirrorToSlot(s->mr, s->level,
                             std::span<const int>(flushed.data(), flushed.size()));
  }
}

/** Re-bind to the current domain after a fold point. Returns 0 when the
 * session's level no longer exists, 1 when the binding was already current,
 * 2 when it re-attached — the undo history was cleared, so the host must
 * reset its step bookkeeping (the blob fallback covers older steps). */
int GridStroke_sync(GridStrokeSession *s)
{
  if (!s || s->level < 1 || s->level > s->mr->maxLevel()) {
    return 0;
  }
  subdiv::GridLevelDomain *d = s->mr->gridDomain(s->level);
  if (d != s->exec.domain) {
    s->exec.attach(d);
    return 2;
  }
  return 1;
}

/** Pull the host-side mask (the slot mesh's mask column — flood fills,
 * imported grid masks) into the domain mirror grids kernels read. O(level);
 * the host calls it only when its mask state changed (a dirty flag), not per
 * stroke. */
void GridStroke_syncMask(GridStrokeSession *s)
{
  if (s && GridStroke_sync(s)) {
    brush::gridsSyncMaskFromSlot(s->mr, s->level);
  }
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
  int moved = s->exec.applyDab(brush::SculptBrushes(tool), float3(ox, oy, oz),
                               float3(nx, ny, nz));
  if (s->mirror && moved > 0) {
    auto &mv = s->exec.lastDabMoved();
    brush::gridsMirrorToSlot(s->mr, s->level,
                             std::span<const int>(mv.data(), mv.size()));
  }
  return moved;
}

void GridStroke_end(GridStrokeSession *s)
{
  if (!s) {
    return;
  }
  // endStep flushes deferred normals internally; mirror the final normals
  // for the whole touched set so the slot mesh releases consistent.
  s->exec.endStep();
  if (s->mirror) {
    // Non-const cast: litestl Vector exposes no const data().
    auto &tv = const_cast<litestl::util::Vector<int> &>(s->exec.strokeTouchedVerts());
    brush::gridsMirrorToSlot(s->mr, s->level,
                             std::span<const int>(tv.data(), tv.size()));
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
  if (!s || !s->log.undo()) {
    return 0;
  }
  if (s->mirror) {
    // Seek granularity is leaf blocks; the full sync keeps the mirror exact.
    s->mirrorAll();
  }
  return 1;
}

int GridStroke_redo(GridStrokeSession *s)
{
  if (!s || !s->log.redo()) {
    return 0;
  }
  if (s->mirror) {
    s->mirrorAll();
  }
  return 1;
}

/** Captured undo bytes across the session's history. */
double GridStroke_undoBytes(GridStrokeSession *s)
{
  return s ? double(s->log.bytes()) : 0.0;
}

/** Whether (mr, level)'s grid domain is currently alive — hosts gate
 * GridTree_castRay on this so a dropped domain (mesh-path fold) doesn't pay
 * a full rebuild inside a hover raycast. */
int Multires_hasGridDomain(subdiv::Multires *mr, int level)
{
  return mr && mr->hasGridDomain(level) ? 1 : 0;
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
