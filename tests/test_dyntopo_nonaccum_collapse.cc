// The non-accumulate base must survive a dyntopo COLLAPSE.
//
// A collapse merges v_kill into v_keep and places the survivor at the edge
// midpoint. The survivor's derived base (`co - .brush.disp.vec`) has to land on
// the *stroke-start* surface, the same way its live position lands on the live
// surface -- otherwise the non-accumulate write-back (`live += want - base`)
// measures each survivor from a base displaced by a per-collapse, direction-
// arbitrary amount, which reads as surface noise rather than a uniform offset.
//
// The check is exact rather than statistical: the cube's +Z face starts
// perfectly flat at z=0.25, so every base on it -- including every survivor's,
// since the midpoint of two 0.25s is 0.25 -- must still read z=0.25 after any
// number of collapses. Two ways that broke:
//
//   (a) double-application: collapseEdge interpolated the field AND the dyntopo
//       driver shifted it again by the survivor's motion, adding half the
//       collapsed edge vector (whose z is nonzero once the dome rises);
//   (b) validity laundering: the field was blended while its `.brush.disp.gen`
//       guard was copied from src0, so an unstamped endpoint contributed its
//       unmaterialized page default under a stamp that still read as valid.
//
// test_brush_nonaccum (d) covers the same path via peak push with a 20%
// tolerance, which both defects slip through.
#include "test_util.h"

#include "debug/scene.h"
#include "debug/script.h"

#include "mesh/attribute.h"
#include "mesh/mesh.h"

#include <cmath>
#include <cstdio>

test_init;

using namespace sculptcore::debug_app;
using namespace sculptcore::mesh;
using namespace litestl::math;

// Scoped away from main() so the Scene is destroyed before test_end() runs its
// leak report.
static void runTest()
{
  Scene scene(256, 256, /*headless=*/true);

  // The base cube's edges are ~0.5/19 = 0.026, so l_min must be raised well
  // above that for a collapse-only dab to bite (l_min defaults to 0.4*detail,
  // which at any sane detail sits below the base edge -- that is why
  // test_brush_nonaccum (d) ends up *subdividing*). flip/smooth stay off: both
  // would partially launder a bad snapshot, and this is a collapse test.
  auto r = script::run(scene,
                       "make_cube subdivs=20 size=0.5\n"
                       "build_spatial leaf_limit=256 depth_limit=8\n"
                       "set_backend backend=cpp\n"
                       "dyntopo enabled=1 mode=collapse detail=0.25 min=0.07 "
                       "flip=0 smooth=0\n"
                       "set_brush_tool tool=draw\n"
                       "set_brush radius=0.25 strength=0.5 nonaccum=1\n"
                       "stroke origin=0,0,0.25 normal=0,0,1 repeat=8\n",
                       ".");
  test_assert(r.ok);
  if (!r.ok) {
    fprintf(stderr, "  line %d: %s\n", r.line_no, r.error.c_str());
    return;
  }

  Mesh *m = scene.mesh;
  test_assert(m->v.attrs.has(AttrType::FLOAT3, ".brush.disp.vec"));
  test_assert(m->v.attrs.has(AttrType::INT, ".brush.disp.gen"));
  auto *dispVec =
      m->v.attrs.find_attribute(AttrType::FLOAT3, ".brush.disp.vec").get_data<float3>();
  auto *dispGen =
      m->v.attrs.find_attribute(AttrType::INT, ".brush.disp.gen").get_data<int>();

  int checked = 0;
  float worst = 0.0f, push = 0.0f;
  bool allFinite = true;
  for (int v : m->v) {
    float3 co = m->v.co[v];
    allFinite &= std::isfinite(co[0]) && std::isfinite(co[1]) && std::isfinite(co[2]);
    push = std::fmax(push, co[2] - 0.25f);

    // Interior of the raised +Z face (the -Z face is far outside the dab and the
    // side faces stay below z=0.2), restricted to verts the stroke stamped.
    if (co[2] < 0.2f || std::fabs(co[0]) > 0.22f || std::fabs(co[1]) > 0.22f) {
      continue;
    }
    if (dispGen->safe_get(v) == 0) {
      continue;
    }
    worst = std::fmax(worst, std::fabs((co - dispVec->safe_get(v))[2] - 0.25f));
    checked++;
  }

  fprintf(stderr,
          "verts=%d (base 2168) push=%.5f checked=%d worst|base.z-0.25|=%.6f\n",
          m->v.count,
          push,
          checked,
          worst);
  test_assert(allFinite);
  test_assert(m->v.count < 2168); // the dab really did collapse geometry
  test_assert(push > 0.0f);       // and the draw still pushed the surface out

  // A collapse-only dab decimates the region to ~l_min spacing, so a couple of
  // dozen survivors IS the whole dome; this just guards against sampling nothing.
  test_assert(checked > 15);

  // Exact invariant: a flat face's derived base stays flat. The slack is
  // for fp blend only -- both defects above miss by 1e-2 or worse.
  test_assert(worst < 1e-4f);
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  runTest();
  return test_end();
}
