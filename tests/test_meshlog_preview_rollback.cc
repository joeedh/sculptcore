/* Regression for the Anchored/Drag Dot live-preview rollback primitive
 * (MeshLog::beginPreviewDab / rollbackPreviewDab, CommandExecutor wrappers).
 * A preview dab is a real applyDab() call issued mid-step; rollback must undo
 * it exactly — topology AND positions — while leaving the step open, and a
 * subsequent real dab + endStep()/undo()/redo() must still work normally
 * (rollback must not corrupt the step it stays inside of). See
 * documentation/plans/anchored-drag-dot-stroke-2026-07-16.md step 2a/6. */
#include "test_util.h"

#include "brush/brush_executor.h"
#include "debug/scene.h"
#include "debug/script.h"
#include "mesh/mesh.h"
#include "spatial/spatial.h"

#include "litestl/math/vector.h"

#include <cmath>
#include <cstdio>
#include <string>

test_init;

using namespace sculptcore;
using namespace sculptcore::debug_app;
using litestl::math::float3;

namespace {

struct MeshCounts {
  int vc, ec, cc, lc, fc;
};

MeshCounts counts(mesh::Mesh &m)
{
  return {m.v.count, m.e.count, m.c.count, m.l.count, m.f.count};
}

bool counts_equal(const MeshCounts &a, const MeshCounts &b)
{
  return a.vc == b.vc && a.ec == b.ec && a.cc == b.cc && a.lc == b.lc && a.fc == b.fc;
}

/* Snapshot of every vertex slot's liveness + position at capture time (mirrors
 * the debug-app save_pos/assert_pos undo-fidelity pattern). Indices that were
 * never live are recorded as such (not just left as a zero-vector placeholder)
 * so diff_pos can tell "never live" apart from "moved to the origin". */
struct PosSnapshot {
  litestl::util::Vector<bool> live;
  litestl::util::Vector<float3> pos;
};

PosSnapshot save_pos(mesh::Mesh &m)
{
  PosSnapshot out;
  int cap = int(m.v.capacity());
  out.live.resize(cap);
  out.pos.resize(cap);
  for (int v = 0; v < cap; v++) {
    out.live[v] = !m.v.freemap[v];
  }
  for (int v : m.v) {
    out.pos[v] = m.v.co[v];
  }
  return out;
}

/* Returns the count of vertex slots whose liveness or (for live-in-both slots)
 * position deviates from `ref` by more than eps. A slot dead in `ref` and
 * still dead now is fine even if some other slot took its place meanwhile —
 * the exact live SET (not just a same-sized set) must match `ref`'s. */
int diff_pos(mesh::Mesh &m, const PosSnapshot &ref, float eps, bool verbose = false)
{
  int bad = 0;
  int cap = int(ref.live.size());
  for (int v = 0; v < cap; v++) {
    bool wasLive = ref.live[v];
    bool isLive = v < int(m.v.capacity()) && !m.v.freemap[v];
    if (wasLive != isLive) {
      bad++;
      if (verbose && bad <= 10) {
        fprintf(stderr,
                "    v=%d liveness mismatch: wasLive=%d isLive=%d\n",
                v,
                wasLive,
                isLive);
      }
      continue;
    }
    if (wasLive && (m.v.co[v] - ref.pos[v]).length() > eps) {
      bad++;
      if (verbose && bad <= 10) {
        float3 d = m.v.co[v] - ref.pos[v];
        fprintf(stderr,
                "    v=%d pos mismatch: cur=(%f,%f,%f) ref=(%f,%f,%f) delta=%f\n",
                v,
                m.v.co[v][0],
                m.v.co[v][1],
                m.v.co[v][2],
                ref.pos[v][0],
                ref.pos[v][1],
                ref.pos[v][2],
                d.length());
      }
    }
  }
  return bad;
}

/* Local equivalent of debug_app's script.cc firstTreeDivergence (anonymous
 * namespace there, not linkable from a test binary). Checks the incrementally
 * maintained spatial tree's face/vert ownership stays consistent with the
 * live mesh: no stale-owned dead elements, no dangling owner ids, leaf-set
 * <-> owner-array agreement in both directions, and full coverage. This is
 * exactly the check that caught the rollbackPreviewDab NOCOPY-ownership-wipe
 * bug (leaf claims a vert the owner array no longer agrees with). */
bool treeOwnershipConsistent(spatial::SpatialTree &tree, mesh::Mesh &m, std::string &msg)
{
  auto &fnode = tree.treeMesh.f.node;
  auto &vnode = tree.treeMesh.v.node;
  char buf[256];

  for (int f = 0; f < int(m.f.capacity()); f++) {
    if (m.f.freemap[f] && fnode[f] != 0) {
      std::snprintf(buf, sizeof(buf), "dead face %d still owned by leaf %d", f, fnode[f]);
      msg = buf;
      return false;
    }
  }
  for (int v = 0; v < int(m.v.capacity()); v++) {
    if (m.v.freemap[v] && vnode[v] != 0) {
      std::snprintf(buf, sizeof(buf), "dead vert %d still owned by leaf %d", v, vnode[v]);
      msg = buf;
      return false;
    }
  }

  for (int f : m.f) {
    int id = fnode[f];
    if (id == 0) {
      continue;
    }
    spatial::SpatialNode *n = tree.node_from_id(id);
    if (!n || !n->data) {
      std::snprintf(
          buf, sizeof(buf), "live face %d owner id %d resolves to no live leaf", f, id);
      msg = buf;
      return false;
    }
  }
  for (int v : m.v) {
    int id = vnode[v];
    if (id == 0) {
      continue;
    }
    spatial::SpatialNode *n = tree.node_from_id(id);
    if (!n || !n->data) {
      std::snprintf(
          buf, sizeof(buf), "live vert %d owner id %d resolves to no live leaf", v, id);
      msg = buf;
      return false;
    }
  }

  auto leaves = tree.leaves();
  for (auto *leaf : leaves) {
    if (!leaf->data) {
      continue;
    }
    for (int f : leaf->data->unique_faces) {
      if (m.f.freemap[f]) {
        std::snprintf(buf, sizeof(buf), "leaf %d claims dead face %d", leaf->id, f);
        msg = buf;
        return false;
      }
      if (fnode[f] != leaf->id) {
        std::snprintf(buf,
                      sizeof(buf),
                      "leaf %d claims face %d but owner array says %d",
                      leaf->id,
                      f,
                      fnode[f]);
        msg = buf;
        return false;
      }
    }
    for (int v : leaf->data->unique_verts) {
      if (m.v.freemap[v]) {
        std::snprintf(buf, sizeof(buf), "leaf %d claims dead vert %d", leaf->id, v);
        msg = buf;
        return false;
      }
      if (vnode[v] != leaf->id) {
        std::snprintf(buf,
                      sizeof(buf),
                      "leaf %d claims vert %d but owner array says %d",
                      leaf->id,
                      v,
                      vnode[v]);
        msg = buf;
        return false;
      }
    }
  }

  for (int f : m.f) {
    if (fnode[f] == 0) {
      std::snprintf(buf, sizeof(buf), "live face %d unowned (dropped)", f);
      msg = buf;
      return false;
    }
  }
  for (int v : m.v) {
    if (vnode[v] == 0) {
      std::snprintf(buf, sizeof(buf), "live vert %d unowned (dropped)", v);
      msg = buf;
      return false;
    }
  }
  int ownedF = 0, ownedV = 0;
  for (auto *leaf : leaves) {
    if (!leaf->data) {
      continue;
    }
    ownedF += int(leaf->data->unique_faces.size());
    ownedV += int(leaf->data->unique_verts.size());
  }
  if (ownedF != m.f.count) {
    std::snprintf(
        buf, sizeof(buf), "leaf faces sum to %d but mesh has %d live", ownedF, m.f.count);
    msg = buf;
    return false;
  }
  if (ownedV != m.v.count) {
    std::snprintf(
        buf, sizeof(buf), "leaf verts sum to %d but mesh has %d live", ownedV, m.v.count);
    msg = buf;
    return false;
  }
  return true;
}

} // namespace

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  Scene scene(256, 256, /*headless=*/true);
  const char *src = "make_cube subdivs=8 size=0.5\n"
                    "build_spatial leaf_limit=128 depth_limit=10\n"
                    "set_brush_tool tool=draw\n"
                    "set_brush radius=0.22 strength=0.5\n";
  auto r = script::run(scene, src, ".");
  test_assert(r.ok);
  if (!r.ok) {
    fprintf(stderr, "  script line %d: %s\n", r.line_no, r.error.c_str());
    return 1;
  }

  mesh::Mesh *m = scene.mesh;

  scene.dyntopoEnabled = true;
  scene.dyntopoParams.l_max = 0.035f;
  scene.dyntopoParams.l_min = 0.012f;
  scene.dyntopoParams.mode = dyntopo::DynTopoMode::Both;

  const float3 normal(0, 0, 1);
  const float radius = scene.brush.radius;
  const float3 origin(0.0f, 0.0f, 0.25f);
  /* Generous snapshot radius: brush radius plus the dyntopo cascade margin. */
  const float previewRadius = radius * 3.0f;

  brush::CommandExecutor exec(scene.tree, &scene.brush);
  exec.meshLog = &scene.meshLog;
  exec.ctx.renderMatrix = scene.renderMatrix;

  /* Pre-stroke baseline. */
  MeshCounts before = counts(*m);
  PosSnapshot posBefore = save_pos(*m);

  exec.beginStep(true);

  /* Dab 1: real, committed (mirrors Anchored's first, non-preview dab). */
  exec.applyDab(
      scene.currentTool, origin, normal, radius, &scene.dyntopoParams, scene.dyntopoSeed);

  MeshCounts afterDab1 = counts(*m);
  PosSnapshot posAfterDab1 = save_pos(*m);
  test_assert(!counts_equal(afterDab1, before)); /* dyntopo actually did something */

  /* Dab 2: a PREVIEW dab at the same origin with a larger radius/strength, so
   * it both deforms differently AND drives more dyntopo splits — exercising
   * both halves of the rollback (topology + position). */
  float previewDabRadius = radius * 1.4f;
  exec.beginPreviewDab(origin, previewRadius);
  test_assert(exec.previewActive());
  scene.brush.strength = 0.9f;
  exec.applyDab(scene.currentTool,
                origin,
                normal,
                previewDabRadius,
                &scene.dyntopoParams,
                scene.dyntopoSeed + 1);

  MeshCounts afterPreview = counts(*m);
  int movedDuringPreview = diff_pos(*m, posAfterDab1, 1e-6f);
  printf("  preview dab: counts %s, %d verts moved\n",
         counts_equal(afterPreview, afterDab1) ? "unchanged" : "changed",
         movedDuringPreview);
  /* The preview dab must have actually done SOMETHING (topology or position),
   * or this test isn't exercising the rollback at all. */
  test_assert(!counts_equal(afterPreview, afterDab1) || movedDuringPreview > 0);

  /* Roll it back — must reproduce dab-1's exact post-state, step still open. */
  exec.rollbackPreviewDab();
  test_assert(!exec.previewActive());

  MeshCounts afterRollback = counts(*m);
  printf("  after rollback: v=%d (want %d) f=%d (want %d)\n",
         afterRollback.vc,
         afterDab1.vc,
         afterRollback.fc,
         afterDab1.fc);
  test_assert(counts_equal(afterRollback, afterDab1));
  int badPos = diff_pos(*m, posAfterDab1, 1e-5f);
  if (badPos) {
    fprintf(
        stderr, "  %d verts NOT restored to pre-preview-dab liveness/position\n", badPos);
  }
  test_assert(badPos == 0);

  /* Spatial-tree ownership must stay consistent after the rollback -- this is
   * the regression check for the NOCOPY-attribute (.spatial.v.node) ownership
   * wipe that rollbackPreviewDab used to reintroduce via ChunkElemRow::writeTo. */
  std::string treeMsg;
  bool treeOk1 = treeOwnershipConsistent(*scene.tree, *m, treeMsg);
  if (!treeOk1) {
    fprintf(
        stderr, "  tree ownership diverged after first rollback: %s\n", treeMsg.c_str());
  }
  test_assert(treeOk1);

  /* A second preview at a different radius, rolled back again — the rollback
   * primitive must be reusable within the same step. */
  scene.brush.strength = 0.7f;
  exec.beginPreviewDab(origin, previewRadius);
  exec.applyDab(scene.currentTool,
                origin,
                normal,
                radius * 1.1f,
                &scene.dyntopoParams,
                scene.dyntopoSeed + 2);
  exec.rollbackPreviewDab();
  test_assert(counts_equal(counts(*m), afterDab1));
  test_assert(diff_pos(*m, posAfterDab1, 1e-5f) == 0);
  std::string treeMsg2;
  bool treeOk2 = treeOwnershipConsistent(*scene.tree, *m, treeMsg2);
  if (!treeOk2) {
    fprintf(stderr,
            "  tree ownership diverged after second rollback: %s\n",
            treeMsg2.c_str());
  }
  test_assert(treeOk2);

  /* Now apply a real (non-preview) dab elsewhere and close the step normally —
   * rollback must not have corrupted the step's own undo/redo. */
  scene.brush.strength = 0.5f;
  const float3 origin2(0.15f, 0.1f, 0.25f);
  exec.applyDab(scene.currentTool,
                origin2,
                normal,
                radius,
                &scene.dyntopoParams,
                scene.dyntopoSeed + 3);
  exec.endDynTopoStroke();
  exec.endStep();

  MeshCounts afterStroke = counts(*m);
  PosSnapshot posAfterStroke = save_pos(*m);
  test_assert(!counts_equal(afterStroke, before));

  /* Whole-step undo must reproduce the exact pre-stroke mesh (the preview
   * dabs must have left no trace at all in the step's chunks). */
  scene.meshLog.undo(m, scene.tree);
  MeshCounts afterUndo = counts(*m);
  printf("  after undo: v=%d (want %d) f=%d (want %d)\n",
         afterUndo.vc,
         before.vc,
         afterUndo.fc,
         before.fc);
  test_assert(counts_equal(afterUndo, before));
  int badUndoPos = diff_pos(*m, posBefore, 1e-4f, true);
  if (badUndoPos) {
    fprintf(stderr, "  %d verts NOT undone to pre-stroke position\n", badUndoPos);
  }
  std::string treeMsgUndo;
  bool treeOkUndo = treeOwnershipConsistent(*scene.tree, *m, treeMsgUndo);
  if (!treeOkUndo) {
    fprintf(stderr,
            "  tree ownership diverged after whole-step undo: %s\n",
            treeMsgUndo.c_str());
  }
  test_assert(treeOkUndo);

  /* Redo must reproduce the post-stroke (dab1 + dab-at-origin2) mesh exactly —
   * NOT the preview dabs' state. */
  scene.meshLog.redo(m, scene.tree);
  MeshCounts afterRedo = counts(*m);
  printf("  after redo: v=%d (want %d) f=%d (want %d)\n",
         afterRedo.vc,
         afterStroke.vc,
         afterRedo.fc,
         afterStroke.fc);
  test_assert(counts_equal(afterRedo, afterStroke));
  int badRedoPos = diff_pos(*m, posAfterStroke, 1e-4f, true);
  if (badRedoPos) {
    fprintf(stderr, "  %d verts NOT redone to post-stroke position\n", badRedoPos);
  }
  test_assert(badRedoPos == 0);
  std::string treeMsgRedo;
  bool treeOkRedo = treeOwnershipConsistent(*scene.tree, *m, treeMsgRedo);
  if (!treeOkRedo) {
    fprintf(stderr,
            "  tree ownership diverged after whole-step redo: %s\n",
            treeMsgRedo.c_str());
  }
  test_assert(treeOkRedo);

  /* --- Cross-stroke regression -----------------------------------------
   * The interactive JS driver (sculptcore_ops.ts applyDabOne) wraps EVERY
   * preview-method dab in beginPreviewDab/rollbackPreviewDab, including the
   * stroke's last sample -- applyDabOne has no "this is the final dab"
   * signal. Without a matching commitPreviewDab() at the true end of the
   * stroke, previewActive() stays true after endStep() and leaks into the
   * NEXT stroke's beginStep(): that stroke's first preview dab sees a stale
   * previewActive()==true and calls rollbackPreviewDab() against a snapshot
   * captured at the end of the PREVIOUS stroke, restoring stale vertex rows
   * onto the current mesh and popping the new step's own freshly-pushed
   * chunk. Reproduce the exact two-stroke shape and confirm commitPreviewDab()
   * prevents it. */
  scene.brush.strength = 0.6f;
  const float3 origin3(-0.1f, 0.05f, 0.25f);

  exec.beginStep(true);
  exec.beginPreviewDab(origin3, previewRadius);
  exec.applyDab(scene.currentTool,
                origin3,
                normal,
                radius,
                &scene.dyntopoParams,
                scene.dyntopoSeed + 4);
  /* An intermediate preview sample, rolled back like any other. */
  exec.rollbackPreviewDab();
  /* Stroke A's "final" dab -- still wrapped as a preview, exactly like
   * applyDabOne always does (it never knows in advance which dab is last). */
  exec.beginPreviewDab(origin3, previewRadius);
  exec.applyDab(scene.currentTool,
                origin3,
                normal,
                radius * 1.05f,
                &scene.dyntopoParams,
                scene.dyntopoSeed + 5);
  test_assert(exec.previewActive());
  /* The fix under test: commit instead of leaving previewActive() dangling. */
  exec.commitPreviewDab();
  test_assert(!exec.previewActive());
  exec.endDynTopoStroke();
  exec.endStep();

  MeshCounts afterStrokeA = counts(*m);
  PosSnapshot posAfterStrokeA = save_pos(*m);

  /* Stroke B begins -- a fresh LogEntry via beginStep(). */
  const float3 origin4(0.2f, -0.1f, 0.25f);
  exec.beginStep(true);
  test_assert(!exec.previewActive());

  /* Mirror applyDabOne's exact first-dab gate: only if previewActive() is
   * (wrongly) still true does it roll back before doing anything else. With
   * the fix this branch must never fire, so the mesh must be byte-identical
   * to stroke A's committed result at this checkpoint -- before stroke B's
   * own first dab has touched anything. */
  if (exec.previewActive()) {
    exec.rollbackPreviewDab();
  }
  test_assert(counts_equal(counts(*m), afterStrokeA));
  int badLeak = diff_pos(*m, posAfterStrokeA, 1e-6f, true);
  if (badLeak) {
    fprintf(stderr,
            "  %d verts corrupted by a stale cross-stroke preview rollback\n",
            badLeak);
  }
  test_assert(badLeak == 0);

  exec.beginPreviewDab(origin4, previewRadius);
  exec.applyDab(scene.currentTool,
                origin4,
                normal,
                radius,
                &scene.dyntopoParams,
                scene.dyntopoSeed + 6);
  test_assert(exec.previewActive());
  exec.commitPreviewDab();
  exec.endDynTopoStroke();
  exec.endStep();

  MeshCounts afterStrokeB = counts(*m);
  test_assert(!counts_equal(afterStrokeB, afterStrokeA));

  std::string treeMsgB;
  bool treeOkB = treeOwnershipConsistent(*scene.tree, *m, treeMsgB);
  if (!treeOkB) {
    fprintf(stderr, "  tree ownership diverged after stroke B: %s\n", treeMsgB.c_str());
  }
  test_assert(treeOkB);

  /* Each step must still undo independently and in order. */
  scene.meshLog.undo(m, scene.tree);
  test_assert(counts_equal(counts(*m), afterStrokeA));
  test_assert(diff_pos(*m, posAfterStrokeA, 1e-4f) == 0);

  scene.meshLog.undo(m, scene.tree);
  test_assert(counts_equal(counts(*m), afterStroke));

  scene.meshLog.redo(m, scene.tree);
  scene.meshLog.redo(m, scene.tree);
  test_assert(counts_equal(counts(*m), afterStrokeB));

  /* --- Symmetry group regression -----------------------------------------
   * Under symmetry, one driver tick's preview session must cover the
   * primary dab AND every mirror-image dab as a single atomic group:
   * beginPreviewDab starts the group at the primary dab's region,
   * extendPreviewDab adds each mirror image's region without resetting the
   * snapshot, and one rollbackPreviewDab() must undo the WHOLE group — not
   * just the primary dab's half — or a mirror image's edits would leak
   * across ticks. This is exactly "anchor/drag-dot rollback isn't working"
   * with symmetry on: the driver used to disable preview entirely whenever
   * symmetry was active (a single (center,radius) snapshot slot couldn't
   * safely cover several independently-positioned mirror dabs), so every
   * dab — primary and mirror alike — was committed for real instead of
   * being a live, rollback-able preview. */
  scene.brush.strength = 0.6f;
  const float3 origin5(0.15f, 0.05f, 0.25f);
  const float3 origin5Mirror(-0.15f, 0.05f, 0.25f);

  MeshCounts beforeSymStroke = counts(*m);
  PosSnapshot posBeforeSymStroke = save_pos(*m);

  exec.beginStep(true);
  test_assert(!exec.previewActive());

  exec.beginPreviewDab(origin5, previewRadius);
  exec.applyDab(scene.currentTool,
                origin5,
                normal,
                radius,
                &scene.dyntopoParams,
                scene.dyntopoSeed + 7);
  exec.extendPreviewDab(origin5Mirror, previewRadius);
  exec.applyDab(scene.currentTool,
                origin5Mirror,
                normal,
                radius,
                &scene.dyntopoParams,
                scene.dyntopoSeed + 8);
  test_assert(exec.previewActive());

  MeshCounts afterSymGroup1 = counts(*m);
  int movedSymGroup1 = diff_pos(*m, posBeforeSymStroke, 1e-6f);
  printf("  symmetry preview group: counts %s, %d verts moved\n",
         counts_equal(afterSymGroup1, beforeSymStroke) ? "unchanged" : "changed",
         movedSymGroup1);
  test_assert(!counts_equal(afterSymGroup1, beforeSymStroke) || movedSymGroup1 > 0);

  /* Roll the WHOLE two-region group back with a single call. */
  exec.rollbackPreviewDab();
  test_assert(!exec.previewActive());
  test_assert(counts_equal(counts(*m), beforeSymStroke));
  int badSymRollback = diff_pos(*m, posBeforeSymStroke, 1e-5f, true);
  if (badSymRollback) {
    fprintf(
        stderr,
        "  %d verts NOT restored after symmetry-group rollback (mirror image leaked)\n",
        badSymRollback);
  }
  test_assert(badSymRollback == 0);
  std::string treeMsgSym;
  bool treeOkSym = treeOwnershipConsistent(*scene.tree, *m, treeMsgSym);
  if (!treeOkSym) {
    fprintf(stderr,
            "  tree ownership diverged after symmetry-group rollback: %s\n",
            treeMsgSym.c_str());
  }
  test_assert(treeOkSym);

  /* Final preview group of the stroke: same primary+mirror shape, committed
   * instead of rolled back — both sides' edits must survive together. */
  exec.beginPreviewDab(origin5, previewRadius);
  exec.applyDab(scene.currentTool,
                origin5,
                normal,
                radius,
                &scene.dyntopoParams,
                scene.dyntopoSeed + 9);
  exec.extendPreviewDab(origin5Mirror, previewRadius);
  exec.applyDab(scene.currentTool,
                origin5Mirror,
                normal,
                radius,
                &scene.dyntopoParams,
                scene.dyntopoSeed + 10);
  test_assert(exec.previewActive());
  exec.commitPreviewDab();
  test_assert(!exec.previewActive());
  exec.endDynTopoStroke();
  exec.endStep();

  MeshCounts afterSymStroke = counts(*m);
  PosSnapshot posAfterSymStroke = save_pos(*m);
  test_assert(!counts_equal(afterSymStroke, beforeSymStroke));

  /* Whole-step undo/redo must reproduce exactly — proving the group's
   * topology chunks (both dabs' worth) and step bookkeeping are intact. */
  scene.meshLog.undo(m, scene.tree);
  MeshCounts afterSymUndo = counts(*m);
  printf("  after symmetry-stroke undo: v=%d (want %d) f=%d (want %d)\n",
         afterSymUndo.vc,
         beforeSymStroke.vc,
         afterSymUndo.fc,
         beforeSymStroke.fc);
  test_assert(counts_equal(afterSymUndo, beforeSymStroke));
  int badSymUndoPos = diff_pos(*m, posBeforeSymStroke, 1e-4f, true);
  if (badSymUndoPos) {
    fprintf(
        stderr, "  %d verts NOT undone to pre-symmetry-stroke position\n", badSymUndoPos);
  }
  test_assert(badSymUndoPos == 0);

  scene.meshLog.redo(m, scene.tree);
  test_assert(counts_equal(counts(*m), afterSymStroke));
  int badSymRedoPos = diff_pos(*m, posAfterSymStroke, 1e-4f, true);
  if (badSymRedoPos) {
    fprintf(stderr,
            "  %d verts NOT redone to post-symmetry-stroke position\n",
            badSymRedoPos);
  }
  test_assert(badSymRedoPos == 0);
  std::string treeMsgSym2;
  bool treeOkSym2 = treeOwnershipConsistent(*scene.tree, *m, treeMsgSym2);
  if (!treeOkSym2) {
    fprintf(stderr,
            "  tree ownership diverged after symmetry-stroke redo: %s\n",
            treeMsgSym2.c_str());
  }
  test_assert(treeOkSym2);

  printf("meshlog_preview_rollback: ok\n");
  /* Return retval directly (not test_end()): the debug Scene/GPU
   * infrastructure leaves allocations live at exit — see test_dyntopo_stroke_undo. */
  return retval;
}
