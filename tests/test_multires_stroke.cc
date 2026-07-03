/* Multires sculpt loop (displacementAndSubSurf plan, S4 gate). Drives the
 * debug-app multires verbs end-to-end on a cube cage:
 *   - ride-along invariance: sculpt fine detail at L3, then a coarse stroke
 *     at L2 — L3's stored deltas stay BIT-identical (assert_disp eps=0) while
 *     the rematerialized L3 surface follows the coarse edit (verts moved);
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

  /* Coarse stroke at L2: fine deltas bit-preserved, coarse deltas changed,
   * and the rematerialized fine surface follows. */
  if (!runOk(scene,
             "multires_level level=2\n"
             "stroke origin=0,0,0.25 normal=0,0,1\n"
             "assert_disp id=fineL3 level=3 eps=0\n"
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

  fprintf(stderr, "multires stroke gates passed\n");

  /* Skip test_end(): attr name strings stay live in the alloc tracker
   * (mirrors the other debug_core tests). */
  return retval;
}
