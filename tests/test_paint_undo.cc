/* Regression for the §Why-2 fix (documentation/plans/attr-saver.md): paint
 * brushes must capture the LAYER they paint, not the hardcoded co/no/f.no set.
 * Before per-brush `save` declarations the color/mask brushes recorded only
 * positions, so undo "restored" unchanged geometry and silently kept the painted
 * values. With `save vertex color;` / `save vertex mask;` the generated *Pre
 * stages now snapshot the painted layer through the AttrSaver gate, so a
 * paint-then-undo round-trip returns the layer to its pre-stroke state.
 *
 * Drives the debug-app `stroke` verb (cpp backend, one meshlog step) then undoes
 * through scene.meshLog directly — mirroring test_dyntopo_stroke_undo.cc. */
#include "test_util.h"

#include "debug/scene.h"
#include "debug/script.h"

#include "mesh/attribute.h"
#include "mesh/mesh.h"

#include <cmath>
#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::debug_app;
using namespace sculptcore::mesh;
using namespace litestl::math;

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  // --- color: paint a per-vertex color layer, undo restores it to zero ---
  {
    Scene scene(256, 256, /*headless=*/true);
    auto r = script::run(scene,
                         "make_cube subdivs=12 size=0.5\n"
                         "build_spatial leaf_limit=256 depth_limit=8\n"
                         "set_brush_tool tool=color\n"
                         "set_brush radius=0.25 strength=1.0\n"
                         "set_backend backend=cpp\n",
                         ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  color setup line %d: %s\n", r.line_no, r.error.c_str());
      return 1;
    }

    Mesh *m = scene.mesh;
    AttrRef &cref = m->v.attrs.ensure(AttrType::FLOAT4, "color", /*materialize=*/true);
    AttrData<float4> *col = cref.get_data<float4>();
    test_assert(col != nullptr);
    for (int i = 0; i < m->v.count; i++) {
      (*col)[i] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    r = script::run(scene, "stroke origin=0,0,0.25 normal=0,0,1\n", ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  color stroke line %d: %s\n", r.line_no, r.error.c_str());
      return 1;
    }

    // The brush actually painted some verts.
    int painted = 0;
    for (int i = 0; i < m->v.count; i++) {
      if ((*col)[i][0] > 0.01f)
        painted++;
    }
    fprintf(stderr, "color: painted=%d\n", painted);
    test_assert(painted > 0);

    // Undo the stroke — must restore every vertex's color to the pre-stroke 0.
    scene.meshLog.undo(m, scene.tree);
    int notRestored = 0;
    for (int i = 0; i < m->v.count; i++) {
      float4 c = (*col)[i];
      if (c[0] != 0.0f || c[1] != 0.0f || c[2] != 0.0f || c[3] != 0.0f) {
        notRestored++;
      }
    }
    fprintf(stderr, "color: notRestored=%d\n", notRestored);
    test_assert(notRestored == 0);
  }

  // --- mask: paint the .spatial.v.mask layer, undo restores it to zero ---
  // The mask layer is materialized (and zeroed) by build_spatial.
  {
    Scene scene(256, 256, /*headless=*/true);
    auto r = script::run(scene,
                         "make_cube subdivs=12 size=0.5\n"
                         "build_spatial leaf_limit=256 depth_limit=8\n"
                         "set_brush_tool tool=mask\n"
                         "set_brush radius=0.25 strength=1.0\n"
                         "set_backend backend=cpp\n",
                         ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  mask setup line %d: %s\n", r.line_no, r.error.c_str());
      return 1;
    }

    Mesh *m = scene.mesh;
    AttrRef mref = m->v.attrs.find_attribute(AttrType::FLOAT, ".spatial.v.mask");
    AttrData<float> *mk = mref.get_data<float>();
    test_assert(mk != nullptr);
    for (int i = 0; i < m->v.count; i++) {
      test_assert((*mk)[i] == 0.0f);
    }

    r = script::run(scene, "stroke origin=0,0,0.25 normal=0,0,1\n", ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  mask stroke line %d: %s\n", r.line_no, r.error.c_str());
      return 1;
    }

    int painted = 0;
    for (int i = 0; i < m->v.count; i++) {
      if ((*mk)[i] > 0.01f)
        painted++;
    }
    fprintf(stderr, "mask: painted=%d\n", painted);
    test_assert(painted > 0);

    scene.meshLog.undo(m, scene.tree);
    int notRestored = 0;
    for (int i = 0; i < m->v.count; i++) {
      if ((*mk)[i] != 0.0f)
        notRestored++;
    }
    fprintf(stderr, "mask: notRestored=%d\n", notRestored);
    test_assert(notRestored == 0);
  }

  /* Return retval directly (not test_end()): the debug Scene/GPU infrastructure
   * leaves allocations live at exit, so test_end()'s leak gate would false-fail
   * (mirrors test_dyntopo_stroke_undo / test_live_stroke). */
  return retval;
}
