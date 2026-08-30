// Wave 6: boundary-aware smooth brush.
//   A) With no boundary flags, bsmooth must reduce to plain Laplacian smoothing
//      — identical to the `smooth` brush on the same cube + stroke.
//   B) Marking every edge sharp classifies every vertex as a boundary vert, so
//      bsmooth projects displacements into the tangent plane — the result must
//      differ from the unconstrained (no-flag) bsmooth somewhere.
// The boundary classification is recomputed (boundary::recomputeDirty) before
// the constrained stroke.
#include "test_util.h"

#include "debug/scene.h"
#include "debug/script.h"

#include "mesh/boundary.h"
#include "mesh/mesh.h"

#include <cstdio>
#include <string>
#include <vector>

test_init;

using namespace sculptcore::debug_app;
using namespace sculptcore::mesh;
using namespace litestl::math;
namespace bnd = sculptcore::mesh::boundary;

static const char *SETUP = "make_cube subdivs=10 size=0.5\n"
                           "build_spatial leaf_limit=256 depth_limit=8\n"
                           "set_brush radius=0.3 strength=1.0\n"
                           "set_backend backend=cpp\n";

static void capture(Mesh *m, std::vector<float3> &out)
{
  out.resize(m->v.count);
  for (int i = 0; i < m->v.count; i++)
    out[i] = m->v.co[i];
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  std::vector<float3> smoothPos, bsmoothPos, sharpPos;

  {
    Scene s(128, 128, /*headless=*/true);
    auto r =
        script::run(s,
                    (std::string(SETUP) + "set_brush_tool tool=smooth\n"
                                          "stroke origin=0.25,0.25,0.25 normal=1,1,1\n")
                        .c_str(),
                    ".");
    test_assert(r.ok);
    capture(s.mesh, smoothPos);
  }
  {
    Scene s(128, 128, /*headless=*/true);
    auto r =
        script::run(s,
                    (std::string(SETUP) + "set_brush_tool tool=bsmooth\n"
                                          "stroke origin=0.25,0.25,0.25 normal=1,1,1\n")
                        .c_str(),
                    ".");
    test_assert(r.ok);
    capture(s.mesh, bsmoothPos);
  }

  // A) bsmooth with no boundaries == smooth.
  test_assert(smoothPos.size() == bsmoothPos.size());
  double maxd = 0.0;
  for (size_t i = 0; i < smoothPos.size(); i++) {
    double d = double((bsmoothPos[i] - smoothPos[i]).length());
    if (d > maxd)
      maxd = d;
  }
  fprintf(stderr, "bsmooth-vs-smooth maxdiff=%g\n", maxd);
  test_assert(maxd < 1e-4);

  {
    // B) every edge sharp -> every vert is a boundary vert -> projection active.
    Scene s(128, 128, /*headless=*/true);
    auto r = script::run(
        s, (std::string(SETUP) + "set_brush_tool tool=bsmooth\n").c_str(), ".");
    test_assert(r.ok);
    Mesh *m = s.mesh;
    for (int e = 0; e < m->e.count; e++) {
      bnd::setEdgeFlag(m, bnd::EDGE_SHARP, e, true);
    }
    bnd::recomputeDirty(m);
    int sharpVerts = 0;
    for (int v = 0; v < m->v.count; v++) {
      if (bnd::vertClass(m, v) & bnd::BC_SHARP)
        sharpVerts++;
    }
    fprintf(stderr, "sharpVerts=%d / %d\n", sharpVerts, m->v.count);
    test_assert(sharpVerts > 0);
    auto r2 = script::run(s, "stroke origin=0.25,0.25,0.25 normal=1,1,1\n", ".");
    test_assert(r2.ok);
    capture(m, sharpPos);
  }

  // The constrained (projected) stroke must differ from the unconstrained one.
  int diffVerts = 0;
  double maxd2 = 0.0;
  for (size_t i = 0; i < bsmoothPos.size() && i < sharpPos.size(); i++) {
    double d = double((sharpPos[i] - bsmoothPos[i]).length());
    if (d > 1e-4)
      diffVerts++;
    if (d > maxd2)
      maxd2 = d;
  }
  fprintf(stderr, "sharp-vs-bsmooth maxdiff=%g diffVerts=%d\n", maxd2, diffVerts);
  test_assert(diffVerts > 0);

  // C) GPU parity: bsmooth with a boundary (all edges sharp) must match cpp vs
  // wgsl bit-modulo-fp — exercising the GPU vclass upload + neighbor-attr +
  // bitwise + projection path.
  std::vector<float3> cppB, wgslB;
  {
    Scene s(128, 128, /*headless=*/true);
    auto r = script::run(
        s, (std::string(SETUP) + "set_brush_tool tool=bsmooth\n").c_str(), ".");
    test_assert(r.ok);
    Mesh *m = s.mesh;
    for (int e = 0; e < m->e.count; e++)
      bnd::setEdgeFlag(m, bnd::EDGE_SHARP, e, true);
    bnd::recomputeDirty(m);
    auto r2 = script::run(
        s, "set_backend backend=cpp\nstroke origin=0.25,0.25,0.25 normal=1,1,1\n", ".");
    test_assert(r2.ok);
    capture(m, cppB);
  }
  {
    Scene s(128, 128, /*headless=*/true);
    auto r = script::run(
        s, (std::string(SETUP) + "set_brush_tool tool=bsmooth\n").c_str(), ".");
    test_assert(r.ok);
    if (!s.ensureGPU()) {
      fprintf(stderr, "no GPU; skipping bsmooth GPU parity\n");
    } else {
      Mesh *m = s.mesh;
      for (int e = 0; e < m->e.count; e++)
        bnd::setEdgeFlag(m, bnd::EDGE_SHARP, e, true);
      bnd::recomputeDirty(m);
      auto r2 = script::run(
          s,
          "set_backend backend=wgsl\nstroke origin=0.25,0.25,0.25 normal=1,1,1\n",
          ".");
      test_assert(r2.ok);
      capture(m, wgslB);
    }
  }
  // GPU parity is only checked when a device was available above (wgslB is empty
  // otherwise — the GPU section self-skips and this test still "passes", so a
  // green run on a deviceless box does NOT mean cpp==wgsl was verified). The
  // tolerance is 1e-3 (per-vertex position L2): the boundary-aware path is
  // close but not bit-identical across backends, so this is a behavioural
  // bound, not an exactness check.
  if (!wgslB.empty() && wgslB.size() == cppB.size()) {
    double maxd3 = 0.0;
    for (size_t i = 0; i < cppB.size(); i++) {
      double d = double((cppB[i] - wgslB[i]).length());
      if (d > maxd3)
        maxd3 = d;
    }
    fprintf(stderr, "bsmooth cpp-vs-wgsl maxdiff=%g\n", maxd3);
    test_assert(maxd3 < 1e-3);
  }

  return test_end();
}
