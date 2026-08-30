/** Grids-native stroke c-api (grids-native brush path, G3): the session
 * surface a host (the Blender addon, the debug app) drives instead of the
 * materialized-mesh executor when the active tool is grids-capable. A session
 * wraps one GridBrushExecutor + GridStrokeLog over one (Multires, level);
 * GridStroke_sync re-binds after any Multires fold point (mesh-path
 * writeback, level ops) dropped the domain — begin() calls it implicitly. */

#include "brush/brush.h"
#include "brush/grid_executor.h"
#include "subdiv/grid_domain.h"
#include "subdiv/grid_draw_source.h"
#include "subdiv/grid_stroke_log.h"
#include "subdiv/grid_tree.h"
#include "subdiv/multires.h"

#include "litestl/util/alloc.h"

#include <cstdint>

using namespace sculptcore;

struct GridStrokeSession {
  subdiv::Multires *mr = nullptr;
  int level = 0;
  /** Ride-along mirror into the resident slot mesh: per-dab moved verts, and
   * a full-level sync after undo/redo seeks. The Blender addon turns this on
   * so extdraw and mesh-path queries stay current; the debug app mirrors
   * itself. */
  bool mirror = false;
  /** Multires::domainGeneration() at bind time. Sync compares THIS, not the
   * domain pointer — a drop + rebuild routinely reuses the same allocation,
   * so pointer equality misses the rebuild and the executor's cached tree
   * pointer dangles into freed memory. */
  uint64_t boundGen = 0;
  subdiv::GridStrokeLog log;
  brush::GridBrushExecutor exec;

  GridStrokeSession(subdiv::Multires *mr, int level, brush::Brush *b)
      : mr(mr), level(level), exec(mr->gridDomain(level), b, &log)
  {
    boundGen = mr->domainGeneration();
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

/** Whether `tool` (a SculptBrushes id) runs grids-native on `mr`; 0 means the
 * host must fall back to the materialized-mesh path for the stroke.
 *
 * `mr` may be null, but a host that has one must pass it: a kernel writing an
 * attribute the stack classes `Derived` is refused only when the storage
 * policy can be asked (grid_attr_bind.h), and the mesh path is that
 * attribute's route home. */
int GridStroke_supported(subdiv::Multires *mr, int tool)
{
  const subdiv::MultiresAttrs *attrs = mr ? &mr->gridAttrs() : nullptr;
  return brush::GridBrushExecutor::supportsBrush(brush::SculptBrushes(tool), attrs) ? 1
                                                                                    : 0;
}

/** Ride-along mirror toggle (see GridStrokeSession::mirror). */
void GridStroke_setMirror(GridStrokeSession *s, int enable)
{
  if (s) {
    s->mirror = enable != 0;
  }
}

/** Object -> clip matrix for the view-mapped texture UV modes, 16 flat floats
 * row-major (the mesh path's CommandExecutor::setRenderMatrix, which the host
 * reaches through the reflected object instead). Set per stroke, before the
 * first dab; a texture brush left without one maps through the identity. */
void GridStroke_setRenderMatrix(GridStrokeSession *s, const float *m16)
{
  if (s) {
    s->exec.setRenderMatrix(m16);
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
  if (flushed.size() > 0) {
    if (subdiv::GridDrawSource *ds = s->mr->drawSource()) {
      ds->markVerts(std::span<const int>(flushed.data(), flushed.size()));
    }
  }
  if (s->mirror && flushed.size() > 0) {
    brush::gridsMirrorToSlot(
        s->mr, s->level, std::span<const int>(flushed.data(), flushed.size()));
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
  // Fetching the domain may rebuild it (bumping the generation); compare
  // against the post-fetch generation so the fresh bind reads as current.
  subdiv::GridLevelDomain *d = s->mr->gridDomain(s->level);
  const uint64_t gen = s->mr->domainGeneration();
  if (d != s->exec.domain || gen != s->boundGen) {
    s->exec.attach(d);
    s->boundGen = gen;
    return 2;
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
                   float ox,
                   float oy,
                   float oz,
                   float nx,
                   float ny,
                   float nz,
                   int grabAdd)
{
  if (!s) {
    return 0;
  }
  s->exec.setGrabAccumAdd(grabAdd != 0);
  int moved = s->exec.applyDab(
      brush::SculptBrushes(tool), float3(ox, oy, oz), float3(nx, ny, nz));
  if (moved > 0) {
    if (subdiv::GridDrawSource *ds = s->mr->drawSource()) {
      auto &mv = s->exec.lastDabMoved();
      ds->markVerts(std::span<const int>(mv.data(), mv.size()));
    }
  }
  if (s->mirror && moved > 0) {
    auto &mv = s->exec.lastDabMoved();
    brush::gridsMirrorToSlot(s->mr, s->level, std::span<const int>(mv.data(), mv.size()));
  }
  return moved;
}

/** One logical dab of a composite brush program (autosmooth's [main,
 * BSMOOTH]) — GridBrushExecutor::applyProgram, with GridStroke_dab's
 * draw-mark/mirror contract. Call once per symmetry image, like
 * GridStroke_dab; there is no grabAdd because grab-class entries are
 * unsupported in grids programs. Returns the union moved-vert count. */
int GridStroke_dabProgram(GridStrokeSession *s,
                          brush::BrushProgram *prog,
                          float ox,
                          float oy,
                          float oz,
                          float nx,
                          float ny,
                          float nz)
{
  if (!s) {
    return 0;
  }
  int moved = s->exec.applyProgram(prog, float3(ox, oy, oz), float3(nx, ny, nz));
  if (moved > 0) {
    if (subdiv::GridDrawSource *ds = s->mr->drawSource()) {
      auto &mv = s->exec.lastDabMoved();
      ds->markVerts(std::span<const int>(mv.data(), mv.size()));
    }
  }
  if (s->mirror && moved > 0) {
    auto &mv = s->exec.lastDabMoved();
    brush::gridsMirrorToSlot(s->mr, s->level, std::span<const int>(mv.data(), mv.size()));
  }
  return moved;
}

/** Whether the session's binding is still current: the level's domain is
 * alive and has not been dropped + rebuilt since bind (generation compare —
 * see GridStrokeSession::boundGen). The batch calls check this instead of
 * sync()ing, because a lazy rebuild here would mask the host's stale stroke
 * as a fresh (empty) domain instead of failing it. */
static bool gridStrokeCurrent(GridStrokeSession *s)
{
  return s && s->mr->hasGridDomain(s->level) &&
         s->mr->domainGeneration() == s->boundGen && s->exec.domain != nullptr;
}

/** Batch raycast against the session's bound tree. `rays` = n x 6
 * {origin.xyz, dir.xyz}; on hit `out6` row i = {p.xyz, normal.xyz} and
 * `hit[i]` = 1. Returns the hit count, or -1 when the binding is no longer
 * current (the host must end the stroke, not rebind mid-stroke). */
int GridStroke_castBatch(
    GridStrokeSession *s, int n, const float *rays, float *out6, uint8_t *hit)
{
  if (!gridStrokeCurrent(s)) {
    return -1;
  }
  subdiv::GridTree *tree = s->exec.tree;
  int count = 0;
  for (int i = 0; i < n; i++) {
    const float *r = rays + i * 6;
    subdiv::GridRayHit h;
    if (tree->castRay(float3(r[0], r[1], r[2]), float3(r[3], r[4], r[5]), h)) {
      float *o = out6 + i * 6;
      o[0] = h.p[0];
      o[1] = h.p[1];
      o[2] = h.p[2];
      o[3] = h.normal[0];
      o[4] = h.normal[1];
      o[5] = h.normal[2];
      hit[i] = 1;
      count++;
    } else {
      hit[i] = 0;
    }
  }
  return count;
}

/** Batch of plain spaced dabs: the host's per-dab prop cycle + apply +
 * symmetry, engine-side. `dabs` = n x 7 {center.xyz, normal.xyz, radius};
 * `strength`/`invert` are constant for the event (the host folds overlap
 * attenuation and brush direction in), `pressure` refills the device sample
 * once per logical dab when `usePressure` (shared by every mirror image, as
 * the host loop does), and `signs` = mirrorCount x 3 reflection sign vectors
 * applied to center and normal. Grab-class/anchored strokes stay host-side.
 * Returns total moved verts, or -1 when the binding is no longer current. */
int GridStroke_dabBatch(GridStrokeSession *s,
                        int tool,
                        int n,
                        const float *dabs,
                        float strength,
                        int invert,
                        float pressure,
                        int usePressure,
                        const float *signs,
                        int mirrorCount)
{
  if (!gridStrokeCurrent(s)) {
    return -1;
  }
  brush::Brush *b = s->exec.brush;
  int moved = 0;
  for (int i = 0; i < n; i++) {
    const float *d = dabs + i * 7;
    // Strength/radius must be rewritten every dab: the executor's loadProps
    // assigns post-dynamics values back into the Brush fields, so a stale
    // field would persist the decayed value into the prop store.
    b->strength = strength;
    b->radius = d[6];
    b->invert = invert != 0;
    b->writeProps();
    if (usePressure) {
      b->clearDeviceInputs();
      b->pushDeviceInput(int(props::DeviceType::PRESSURE), pressure);
    }
    moved += GridStroke_dab(s, tool, d[0], d[1], d[2], d[3], d[4], d[5], 0);
    for (int m = 0; m < mirrorCount; m++) {
      const float *sg = signs + m * 3;
      moved += GridStroke_dab(s,
                              tool,
                              d[0] * sg[0],
                              d[1] * sg[1],
                              d[2] * sg[2],
                              d[3] * sg[0],
                              d[4] * sg[1],
                              d[5] * sg[2],
                              0);
    }
  }
  return moved;
}

/** Batch of composite-program dabs: GridStroke_dabBatch's per-dab prop cycle
 * with GridStroke_dabProgram as the dab unit (one program run per symmetry
 * image). Same `dabs`/`signs` layout and pressure semantics; entry-level
 * overrides (autosmooth's pinned smooth strength/invert) supersede the
 * per-event `strength`/`invert` for their entry. Returns total moved verts,
 * or -1 when the binding is no longer current. */
int GridStroke_dabBatchProgram(GridStrokeSession *s,
                               brush::BrushProgram *prog,
                               int n,
                               const float *dabs,
                               float strength,
                               int invert,
                               float pressure,
                               int usePressure,
                               const float *signs,
                               int mirrorCount)
{
  if (!gridStrokeCurrent(s)) {
    return -1;
  }
  brush::Brush *b = s->exec.brush;
  int moved = 0;
  for (int i = 0; i < n; i++) {
    const float *d = dabs + i * 7;
    // Strength/radius must be rewritten every dab: the executor's loadProps
    // assigns post-dynamics values back into the Brush fields, so a stale
    // field would persist the decayed value into the prop store.
    b->strength = strength;
    b->radius = d[6];
    b->invert = invert != 0;
    b->writeProps();
    if (usePressure) {
      b->clearDeviceInputs();
      b->pushDeviceInput(int(props::DeviceType::PRESSURE), pressure);
    }
    moved += GridStroke_dabProgram(s, prog, d[0], d[1], d[2], d[3], d[4], d[5]);
    for (int m = 0; m < mirrorCount; m++) {
      const float *sg = signs + m * 3;
      moved += GridStroke_dabProgram(s,
                                     prog,
                                     d[0] * sg[0],
                                     d[1] * sg[1],
                                     d[2] * sg[2],
                                     d[3] * sg[0],
                                     d[4] * sg[1],
                                     d[5] * sg[2]);
    }
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
  if (subdiv::GridDrawSource *ds = s->mr->drawSource()) {
    auto &tv = const_cast<litestl::util::Vector<int> &>(s->exec.strokeTouchedVerts());
    ds->markVerts(std::span<const int>(tv.data(), tv.size()));
  }
  if (s->mirror) {
    // Non-const cast: litestl Vector exposes no const data().
    auto &tv = const_cast<litestl::util::Vector<int> &>(s->exec.strokeTouchedVerts());
    brush::gridsMirrorToSlot(s->mr, s->level, std::span<const int>(tv.data(), tv.size()));
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

/** Evict the oldest applied step (host undo-limiter truncation). Returns 1
 * when a step was dropped. No slot mirror: eviction never changes the live
 * surface, only the reachable history depth. */
int GridStroke_dropOldest(GridStrokeSession *s)
{
  return s && s->log.dropOldest() ? 1 : 0;
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
                     float ox,
                     float oy,
                     float oz,
                     float dx,
                     float dy,
                     float dz,
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
