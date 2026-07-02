/* Layerdraw stroke + undo/redo on a 2-layer stack (displacementAndSubSurf F1
 * gate). The layerdraw kernel writes the bound sculpt layer's deltas; the
 * executor's LayerEditScope bracket folds them into evaluated v.co, and the
 * kernel's `save vertex co, no, slayer` puts positions and the layer in the
 * same meshlog step — so one undo press reverts both together.
 *
 * Drives the debug-app `stroke` verb (with layer= retargeting) then
 * undoes/redoes through scene.meshLog directly, mirroring test_paint_undo.cc. */
#include "test_util.h"

#include "debug/scene.h"
#include "debug/script.h"

#include "mesh/attribute.h"
#include "mesh/mesh.h"

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

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  Scene scene(256, 256, /*headless=*/true);
  auto r = script::run(scene,
                       "make_cube subdivs=12 size=0.5\n"
                       "build_spatial leaf_limit=256 depth_limit=8\n"
                       "layer_add name=layerA\n"
                       "layer_add name=layerB\n"
                       "set_brush_tool tool=layerdraw\n"
                       "set_brush radius=0.25 strength=1.0\n"
                       "set_backend backend=cpp\n",
                       ".");
  test_assert(r.ok);
  if (!r.ok) {
    fprintf(stderr, "  setup line %d: %s\n", r.line_no, r.error.c_str());
    return 1;
  }

  Mesh *m = scene.mesh;
  Vector<float3> s0;
  snapshot(m, s0);

  // Stroke into layer A: layer gains deltas AND evaluated positions move.
  r = script::run(scene, "stroke origin=0,0,0.25 normal=0,0,1 layer=layerA\n", ".");
  test_assert(r.ok);
  if (!r.ok) {
    fprintf(stderr, "  strokeA line %d: %s\n", r.line_no, r.error.c_str());
    return 1;
  }
  int movedA = countMoved(m, s0);
  fprintf(stderr, "layer stroke A: moved=%d\n", movedA);
  test_assert(movedA > 0);

  AttrRef aref = m->v.attrs.find_attribute(AttrType::FLOAT3, "layerA");
  test_assert(aref.exists());
  AttrData<float3> *da = aref.get_data<float3>();
  int touchedA = 0;
  for (int v : m->v) {
    float3 d = da->safe_get(v);
    if (d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f) {
      touchedA++;
    }
  }
  fprintf(stderr, "layer stroke A: layer verts touched=%d\n", touchedA);
  test_assert(touchedA > 0);

  Vector<float3> s1;
  snapshot(m, s1);

  // Second stroke into layer B (same spot) — the 2-layer stack.
  r = script::run(scene, "stroke origin=0,0,0.25 normal=0,0,1 layer=layerB\n", ".");
  test_assert(r.ok);
  int movedB = countMoved(m, s1);
  fprintf(stderr, "layer stroke B: moved=%d\n", movedB);
  test_assert(movedB > 0);

  Vector<float3> s2;
  snapshot(m, s2);

  // Undo B: back to s1, layerB zeroed; layerA intact.
  scene.meshLog.undo(m, scene.tree);
  test_assert(countMoved(m, s1) == 0);
  {
    AttrRef bref = m->v.attrs.find_attribute(AttrType::FLOAT3, "layerB");
    test_assert(bref.exists());
    AttrData<float3> *db = bref.get_data<float3>();
    int touchedB = 0;
    for (int v : m->v) {
      float3 d = db->safe_get(v);
      if (d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f) {
        touchedB++;
      }
    }
    fprintf(stderr, "after undo B: layerB verts touched=%d\n", touchedB);
    test_assert(touchedB == 0);
  }

  // Undo A: full round-trip to the pre-stroke state.
  scene.meshLog.undo(m, scene.tree);
  test_assert(countMoved(m, s0) == 0);
  {
    int touched = 0;
    for (int v : m->v) {
      float3 d = da->safe_get(v);
      if (d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f) {
        touched++;
      }
    }
    fprintf(stderr, "after undo A: layerA verts touched=%d\n", touched);
    test_assert(touched == 0);
  }

  // Redo both: exact replay to s2.
  scene.meshLog.redo(m, scene.tree);
  test_assert(countMoved(m, s1) == 0);
  scene.meshLog.redo(m, scene.tree);
  test_assert(countMoved(m, s2) == 0);

  // Weight round-trip through the compositor keeps positions consistent.
  r = script::run(scene, "layer_set name=layerA weight=0.5\n", ".");
  test_assert(r.ok);
  test_assert(countMoved(m, s2) > 0);
  r = script::run(scene, "layer_set name=layerA weight=1.0\n", ".");
  test_assert(r.ok);
  test_assert(countMoved(m, s2, 1e-4f) == 0);

  /* Return retval directly (not test_end()): the debug Scene/GPU infrastructure
   * leaves allocations live at exit (mirrors test_paint_undo.cc). */
  return retval;
}
