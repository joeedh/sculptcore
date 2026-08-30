/* Multires sculpt loop (displacementAndSubSurf plan, S4 gate). Drives the
 * debug-app multires verbs end-to-end on a cube cage:
 *   - ride-along invariance: sculpt fine detail at L3, then a coarse stroke
 *     at L2 — the L3 SURFACE survives the switch down (which restricts the
 *     fine detail into L2 and re-expresses L3's deltas against the new base,
 *     so the deltas themselves change) and then follows the coarse edit;
 *   - undo/redo fidelity across level switches (stroke at L2, bounce to L3
 *     and back, undo -> pre positions, redo -> post positions);
 *   - level-aware undo: undoing from L3 auto-switches to the stroke's level.
 * Strokes run through the standard executor + meshlog; each stroke verb ends
 * with a store writeback (frame-relative deltas, baseline-diff skip). */
#include "test_util.h"

#include "debug/scene.h"
#include "debug/script.h"

#include "mesh/mesh.h"
#include "subdiv/multires.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::debug_app;
using namespace sculptcore::mesh;
using namespace litestl::math;
using litestl::util::Vector;

static void snapshot(Mesh *m, Vector<float3> &out)
{
  out.resize(m->v.capacity());
  for (int v : m->v) {
    out[v] = m->v.co[v];
  }
}

static int countMoved(Mesh *m, const Vector<float3> &ref, float eps = 1e-6f)
{
  int moved = 0;
  for (int v : m->v) {
    float3 d = m->v.co[v] - ref[v];
    if (std::fabs(d[0]) > eps || std::fabs(d[1]) > eps || std::fabs(d[2]) > eps) {
      moved++;
    }
  }
  return moved;
}

static bool runOk(Scene &scene, const char *text, const char *tag)
{
  auto r = script::run(scene, text, ".");
  if (!r.ok) {
    fprintf(stderr, "  %s: line %d: %s\n", tag, r.line_no, r.error.c_str());
  }
  test_assert(r.ok);
  return r.ok;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  Scene scene(256, 256, /*headless=*/true);

  /* Fine detail at L3, snapshot its deltas. */
  if (!runOk(scene,
             "make_cube subdivs=4 size=0.5\n"
             "multires_init levels=3 level=3\n"
             "set_brush_tool tool=draw\n"
             "set_brush radius=0.2 strength=0.5\n"
             "set_backend backend=cpp\n"
             "stroke origin=0,0,0.25 normal=0,0,1\n"
             "save_pos id=fineL3\n"
             "save_disp id=fineL3 level=3\n"
             "save_disp id=preL2 level=2\n",
             "setup"))
  {
    return 1;
  }
  test_assert(scene.multires != nullptr);
  test_assert(scene.multires->activeLevel() == 3);

  Vector<float3> l3Before;
  snapshot(scene.mesh, l3Before);
  int l3Verts = scene.mesh->v.count;

  /* Switching down propagates the L3 detail into L2, so both levels' stored
   * deltas move — but the L3 SURFACE is untouched by the re-encode (the
   * restriction is paired with an exact re-expression against the new base). */
  if (!runOk(scene,
             "multires_level level=2\n"
             "assert_disp id=preL2 level=2 changed=1\n"
             "multires_level level=3\n"
             "assert_disp id=fineL3 level=3 changed=1\n"
             "assert_pos id=fineL3 eps=1e-4\n"
             "save_disp id=preL2 level=2\n",
             "propagate-down"))
  {
    return 1;
  }

  /* Coarse stroke at L2: coarse deltas change and the rematerialized fine
   * surface follows the coarse edit. */
  if (!runOk(scene,
             "multires_level level=2\n"
             "stroke origin=0,0,0.25 normal=0,0,1\n"
             "assert_disp id=preL2 level=2 changed=1\n"
             "multires_level level=3\n",
             "ride-along"))
  {
    return 1;
  }
  test_assert(scene.mesh->v.count == l3Verts);
  int moved = countMoved(scene.mesh, l3Before);
  fprintf(stderr, "ride-along: L3 verts moved by the L2 stroke: %d\n", moved);
  test_assert(moved > 0);

  /* Undo/redo fidelity across a level switch. */
  if (!runOk(scene,
             "multires_level level=2\n"
             "save_pos id=l2pre\n"
             "stroke origin=0.25,0,0 normal=1,0,0\n"
             "save_pos id=l2post\n"
             "multires_level level=3\n"
             "multires_level level=2\n"
             "undo\n"
             "assert_pos id=l2pre eps=1e-5\n"
             "redo\n"
             "assert_pos id=l2post eps=1e-5\n",
             "undo-across-switch"))
  {
    return 1;
  }

  /* Level-aware undo: from L3, undo auto-switches to the stroke's level. */
  if (!runOk(scene,
             "multires_level level=3\n"
             "undo\n"
             "assert_pos id=l2pre eps=1e-5\n",
             "auto-switch-undo"))
  {
    return 1;
  }
  test_assert(scene.multires->activeLevel() == 2);

  /* Dyntopo is refused on a level mesh (Mesh::topoLocked): a level's topology
   * comes from the grid tables, so a remesh would strand every level's
   * displacement. The stroke still sculpts; only the topology stays put. */
  test_assert(scene.mesh->topoLocked);
  int lockedVerts = scene.mesh->v.count, lockedFaces = scene.mesh->f.count;
  Vector<float3> lockedPre;
  snapshot(scene.mesh, lockedPre);
  scene.cumSplits = scene.cumCollapses = 0;
  if (!runOk(scene,
             "dyntopo enabled=1 detail=0.02\n"
             "stroke origin=0,0.25,0 normal=0,1,0\n",
             "dyntopo-refused"))
  {
    return 1;
  }
  fprintf(stderr,
          "dyntopo-refused: splits=%lld collapses=%lld verts %d->%d\n",
          (long long)scene.cumSplits,
          (long long)scene.cumCollapses,
          lockedVerts,
          scene.mesh->v.count);
  test_assert(scene.cumSplits == 0 && scene.cumCollapses == 0);
  test_assert(scene.mesh->v.count == lockedVerts);
  test_assert(scene.mesh->f.count == lockedFaces);
  test_assert(countMoved(scene.mesh, lockedPre) > 0);

  fprintf(stderr, "multires stroke gates passed\n");

  /* Skip test_end(): attr name strings stay live in the alloc tracker
   * (mirrors the other debug_core tests). */
  return retval;
}
