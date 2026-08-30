// Non-accumulate brush mode (plans/nonAccumMode.md): matches Blender's
// "Accumulate off". Within a stroke, deform dabs measure falloff from each
// vertex's frozen stroke-start position and add the resulting displacement to
// the live position, so the brush footprint stays pinned to the original surface
// and repeated coverage sums with no height cap. This drives the C++ executor's
// AccumOrig path through the debug-app script harness (`set_brush nonaccum=1`,
// `stroke repeat=N`) and asserts the Blender-matching invariants:
//   (a) additive      — non-accum builds linearly (repeat=8 == 8x one dab,
//                       repeat=16 == 2x repeat=8, no cap); accumulate re-reads
//                       the live (bulging) surface and tapers below it;
//   (b) base fallback — a non-accum smooth stroke leaves unstamped neighbors
//                       reading their live position (no collapse toward origin);
//   (c) cross-stroke  — a fresh non-accum stroke re-stamps at the current
//                       surface (generation bump), so four separate strokes over
//                       a fixed brush origin differ from one repeat=4 stroke.
#include "test_util.h"

#include "debug/scene.h"
#include "debug/script.h"

#include "mesh/mesh.h"

#include <cmath>
#include <cstdio>
#include <string>

test_init;

using namespace sculptcore::debug_app;
using namespace sculptcore::mesh;
using namespace litestl::math;

// Build a fresh subdivided cube, run `strokeScript` (the per-test stroke lines),
// and return the peak outward push of a +Z draw stroke: the +Z face rests at
// z=0.25, so the answer is max(co.z) - 0.25.
static float drawPush(const char *strokeScript)
{
  Scene scene(256, 256, /*headless=*/true);
  std::string src = "make_cube subdivs=12 size=0.5\n"
                    "build_spatial leaf_limit=256 depth_limit=8\n"
                    "set_brush_tool tool=draw\n"
                    "set_backend backend=cpp\n";
  src += strokeScript;
  auto r = script::run(scene, src.c_str(), ".");
  test_assert(r.ok);
  if (!r.ok) {
    fprintf(stderr, "  line %d: %s\n", r.line_no, r.error.c_str());
    return 0.0f;
  }
  Mesh *m = scene.mesh;
  float maxz = -1e30f;
  for (int i = 0; i < m->v.count; i++) {
    maxz = std::fmax(maxz, m->v.co[i][2]);
  }
  return maxz - 0.25f;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  // (a) Additive (Blender "Accumulate off"). The draw kernel pushes v.co by
  // surfaceNo*strength(v.co)*r/2. Under AccumOrig the displacement is measured
  // from the frozen stroke-start base and added to live each dab, so the peak
  // grows linearly with no cap: repeat=8 == 8x one dab, repeat=16 == 2x repeat=8.
  // Accumulate re-reads the live position each dab; as the surface bulges away
  // from the fixed brush center the falloff there decays, so it grows sublinearly
  // and stays below the non-accum push.
  float na1 = drawPush("set_brush radius=0.25 strength=0.5 nonaccum=1\n"
                       "stroke origin=0,0,0.25 normal=0,0,1 repeat=1\n");
  float na8 = drawPush("set_brush radius=0.25 strength=0.5 nonaccum=1\n"
                       "stroke origin=0,0,0.25 normal=0,0,1 repeat=8\n");
  float na16 = drawPush("set_brush radius=0.25 strength=0.5 nonaccum=1\n"
                        "stroke origin=0,0,0.25 normal=0,0,1 repeat=16\n");
  float ac8 = drawPush("set_brush radius=0.25 strength=0.5 nonaccum=0\n"
                       "stroke origin=0,0,0.25 normal=0,0,1 repeat=8\n");
  fprintf(stderr, "(a) na1=%.5f na8=%.5f na16=%.5f ac8=%.5f\n", na1, na8, na16, ac8);
  test_assert(na1 > 0.0f);                                  // the dab actually pushed
  test_assert(std::fabs(na8 - 8.0f * na1) < 0.02f * na8);   // linear: 8 dabs == 8x
  test_assert(std::fabs(na16 - 2.0f * na8) < 0.02f * na16); // no cap: 16 == 2x 8
  test_assert(ac8 < na8 * 0.6f); // accumulate tapers below non-accum

  // (c) Cross-stroke. Each `stroke` verb bumps the non-accumulate generation, so
  // a fresh stroke re-stamps every vert at its current (already-pushed) position
  // and measures anew from there. A single repeat=4 stroke keeps one frozen base
  // and grows linearly (== 4x one dab). Four separate strokes over the fixed
  // brush origin re-base each time onto the risen surface, whose distance to the
  // (unmoved) brush center has grown, so the later increments shrink and the
  // total lands below the single-stroke linear push -- proof the old stamps were
  // dropped rather than reused.
  float na_rep4 = drawPush("set_brush radius=0.25 strength=0.5 nonaccum=1\n"
                           "stroke origin=0,0,0.25 normal=0,0,1 repeat=4\n");
  float na_x4 = drawPush("set_brush radius=0.25 strength=0.5 nonaccum=1\n"
                         "stroke origin=0,0,0.25 normal=0,0,1\n"
                         "stroke origin=0,0,0.25 normal=0,0,1\n"
                         "stroke origin=0,0,0.25 normal=0,0,1\n"
                         "stroke origin=0,0,0.25 normal=0,0,1\n");
  fprintf(stderr, "(c) na_rep4=%.5f na_x4=%.5f\n", na_rep4, na_x4);
  test_assert(std::fabs(na_rep4 - 4.0f * na1) < 0.02f * na_rep4); // one stroke: linear 4x
  test_assert(na_x4 > 0.0f);            // fresh strokes still push
  test_assert(na_x4 < na_rep4 - 1e-3f); // re-based onto the risen surface

  // (b) Base fallback. A non-accum smooth on the flat +Z face: stamped verts
  // hold orig==live==0.25 and unstamped neighbors (outside the dab) must read
  // their live z=0.25. If the fallback wrongly returned the unmaterialized
  // orig.co (0), the neighbor average would drag the face inward toward the
  // origin. Confirm the interior face verts stay put (no collapse) and finite.
  {
    Scene scene(256, 256, /*headless=*/true);
    auto r = script::run(scene,
                         "make_cube subdivs=12 size=0.5\n"
                         "build_spatial leaf_limit=256 depth_limit=8\n"
                         "set_backend backend=cpp\n"
                         "set_brush_tool tool=smooth\n"
                         "set_brush radius=0.25 strength=0.9 nonaccum=1\n"
                         "stroke origin=0,0,0.25 normal=0,0,1 repeat=4\n",
                         ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  (b) line %d: %s\n", r.line_no, r.error.c_str());
      return 1;
    }
    Mesh *m = scene.mesh;
    float minInteriorZ = 1e30f;
    bool allFinite = true;
    int interior = 0;
    for (int i = 0; i < m->v.count; i++) {
      float3 co = m->v.co[i];
      allFinite &= std::isfinite(co[0]) && std::isfinite(co[1]) && std::isfinite(co[2]);
      // Interior +Z-face verts: well inside the dab, away from the cube edges.
      if (std::fabs(co[0]) < 0.18f && std::fabs(co[1]) < 0.18f && co[2] > 0.15f) {
        minInteriorZ = std::fmin(minInteriorZ, co[2]);
        interior++;
      }
    }
    fprintf(stderr,
            "(b) interior=%d minInteriorZ=%.5f finite=%d\n",
            interior,
            minInteriorZ,
            int(allFinite));
    test_assert(allFinite);           // no NaN/inf from a bad base read
    test_assert(interior > 0);        // we actually sampled the face
    test_assert(minInteriorZ > 0.2f); // face held its z (no inward collapse)
  }

  // (d) Dyntopo coherence. detail=0.08 is coarser than the base cube's ~0.042
  // edges, so dyntopo *collapses* under the dab; with tangential smooth on too,
  // both the collapse-survivor and smooth coherence shifts in dyntopo.h fire.
  // Each shifts a stamped vert's stroke-start snapshot by the same delta the op
  // moves it, so the non-accum draw keeps measuring from a coherent surface. If
  // those shifts were missing the verts would "snap back" each dab and the push
  // would run away (or NaN). Assert the dab remeshed the surface yet the push
  // stays finite and lands on the same linear push as the non-dyntopo run
  // (repeat=6 == 6x one dab), confirming the snapshot tracked the remesh motion.
  float na6 = drawPush("set_brush radius=0.25 strength=0.5 nonaccum=1\n"
                       "stroke origin=0,0,0.25 normal=0,0,1 repeat=6\n");
  {
    Scene scene(256, 256, /*headless=*/true);
    auto r = script::run(scene,
                         "make_cube subdivs=12 size=0.5\n"
                         "build_spatial leaf_limit=256 depth_limit=8\n"
                         "set_backend backend=cpp\n"
                         "dyntopo enabled=1 detail=0.08 flip=1 smooth=1\n"
                         "set_brush_tool tool=draw\n"
                         "set_brush radius=0.25 strength=0.5 nonaccum=1\n"
                         "stroke origin=0,0,0.25 normal=0,0,1 repeat=6\n",
                         ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  (d) line %d: %s\n", r.line_no, r.error.c_str());
      return 1;
    }
    Mesh *m = scene.mesh;
    float maxz = -1e30f;
    bool allFinite = true;
    for (int i = 0; i < m->v.count; i++) {
      float3 co = m->v.co[i];
      allFinite &= std::isfinite(co[0]) && std::isfinite(co[1]) && std::isfinite(co[2]);
      maxz = std::fmax(maxz, co[2]);
    }
    float push = maxz - 0.25f;
    fprintf(
        stderr, "(d) verts=%d push=%.5f finite=%d\n", m->v.count, push, int(allFinite));
    test_assert(allFinite);        // coherent snapshot => no NaN/runaway
    test_assert(m->v.count < 866); // dab remeshed (collapsed; cube starts at 866)
    test_assert(push > 0.0f);      // the draw moved the surface out
    test_assert(std::fabs(push - na6) <
                0.2f * na6); // matches the non-dyntopo linear push
  }

  // (e) Envelope retention. A moving non-accum stroke (left to right across the
  // +Z face): mid-path verts get their full push while the brush is over them,
  // then later dabs only cover them weakly. The additive accumulator only ever
  // adds, so the push is retained; without it each later dab rewrites live from
  // base with its (fading) falloff and the trailing edge snaps back (~10% of
  // the full push instead of ~100%).
  {
    Scene scene(256, 256, /*headless=*/true);
    auto r = script::run(scene,
                         "make_cube subdivs=12 size=0.5\n"
                         "build_spatial leaf_limit=256 depth_limit=8\n"
                         "set_backend backend=cpp\n"
                         "set_brush_tool tool=draw\n"
                         "set_brush radius=0.25 strength=0.5 nonaccum=1\n"
                         "stroke_path p1=-0.2,0,0.25 p2=0.2,0,0.25 steps=6 "
                         "normal=0,0,1\n",
                         ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  (e) line %d: %s\n", r.line_no, r.error.c_str());
      return 1;
    }
    Mesh *m = scene.mesh;
    float pushMid = 0.0f, pushEnd = 0.0f;
    for (int i = 0; i < m->v.count; i++) {
      float3 co = m->v.co[i];
      if (co[2] < 0.2f || std::fabs(co[1]) > 0.1f)
        continue;
      if (std::fabs(co[0]) < 0.05f)
        pushMid = std::fmax(pushMid, co[2] - 0.25f);
      if (std::fabs(co[0] - 0.2f) < 0.05f)
        pushEnd = std::fmax(pushEnd, co[2] - 0.25f);
    }
    fprintf(stderr, "(e) pushMid=%.5f pushEnd=%.5f\n", pushMid, pushEnd);
    test_assert(pushEnd > 0.0f);           // the stroke reached the far end
    test_assert(pushMid > 0.7f * pushEnd); // trailing edge held its push
  }

  // (f) Falloff-shaped profile. With additive (Blender "Accumulate off") the
  // per-dab increment carries the brush falloff, so repeated dabs build a
  // falloff-shaped dome -- higher at the center than the band edge -- rather than
  // the flat plateau the old capped-layer write-back produced.
  {
    Scene scene(256, 256, /*headless=*/true);
    auto r = script::run(scene,
                         "make_cube subdivs=12 size=0.5\n"
                         "build_spatial leaf_limit=256 depth_limit=8\n"
                         "set_backend backend=cpp\n"
                         "set_brush_tool tool=draw\n"
                         "set_brush radius=0.25 strength=0.5 nonaccum=1\n"
                         "stroke origin=0,0,0.25 normal=0,0,1 repeat=8\n",
                         ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  (f) line %d: %s\n", r.line_no, r.error.c_str());
      return 1;
    }
    Mesh *m = scene.mesh;
    float minPush = 1e30f, maxPush = 0.0f;
    int band = 0;
    for (int i = 0; i < m->v.count; i++) {
      float3 co = m->v.co[i];
      if (co[2] < 0.2f || std::fabs(co[0]) > 0.1f || std::fabs(co[1]) > 0.1f)
        continue;
      minPush = std::fmin(minPush, co[2] - 0.25f);
      maxPush = std::fmax(maxPush, co[2] - 0.25f);
      band++;
    }
    fprintf(stderr, "(f) band=%d minPush=%.5f maxPush=%.5f\n", band, minPush, maxPush);
    test_assert(band > 0);
    test_assert(maxPush > 0.0f);
    test_assert(minPush < 0.85f * maxPush); // falloff-shaped dome, not a plateau
  }

  // (g) Displacement base (plans/2026-07-26-0909-brush-displacement-base-
  // attribute.md). The from-base position is derived as `co - .brush.disp.vec`,
  // so the §2 invariant — every brush delta is added to disp — must hold
  // exactly: on a stamped vert, disp is what the stroke moved it by.
  {
    Scene scene(256, 256, /*headless=*/true);
    auto rp = script::run(scene,
                          "make_cube subdivs=12 size=0.5\n"
                          "build_spatial leaf_limit=256 depth_limit=8\n"
                          "set_backend backend=cpp\n",
                          ".");
    test_assert(rp.ok);
    if (!rp.ok) {
      fprintf(stderr, "  (g) setup line %d: %s\n", rp.line_no, rp.error.c_str());
      return 1;
    }
    Mesh *m = scene.mesh;
    litestl::util::Vector<float3> before;
    before.resize(m->v.count);
    for (int i = 0; i < m->v.count; i++) {
      before[i] = m->v.co[i];
    }

    auto rs = script::run(scene,
                          "set_brush_tool tool=draw\n"
                          "set_brush radius=0.25 strength=0.5 nonaccum=1\n"
                          "stroke_path p1=-0.2,0,0.25 p2=0.2,0,0.25 steps=6 "
                          "normal=0,0,1\n",
                          ".");
    test_assert(rs.ok);
    if (!rs.ok) {
      fprintf(stderr, "  (g) stroke line %d: %s\n", rs.line_no, rs.error.c_str());
      return 1;
    }
    // Static topology: the vert set the stroke saw is the one snapshotted.
    test_assert(m->v.count == int(before.size()));

    test_assert(m->v.attrs.has(AttrType::FLOAT3, ".brush.disp.vec"));
    test_assert(m->v.attrs.has(AttrType::INT, ".brush.disp.gen"));
    auto *disp =
        m->v.attrs.find_attribute(AttrType::FLOAT3, ".brush.disp.vec").get_data<float3>();
    auto *dispGen =
        m->v.attrs.find_attribute(AttrType::INT, ".brush.disp.gen").get_data<int>();
    int stamped = 0, moved = 0;
    float maxInvariant = 0.0f, maxDisp = 0.0f, maxUnstamped = 0.0f;
    for (int i = 0; i < m->v.count; i++) {
      if (dispGen->safe_get(i) != int(scene.strokeGen)) {
        // Nothing outside the stamped set may have moved.
        maxUnstamped = std::fmax(maxUnstamped, (m->v.co[i] - before[i]).length());
        continue;
      }
      stamped++;
      float3 d = disp->safe_get(i);
      maxDisp = std::fmax(maxDisp, d.length());
      if (d.length() > 1e-6f) {
        moved++;
      }
      maxInvariant = std::fmax(maxInvariant, (d - (m->v.co[i] - before[i])).length());
    }
    fprintf(stderr,
            "(g) stamped=%d moved=%d maxDisp=%.5f inv=%.8f unstamped=%.8f\n",
            stamped,
            moved,
            maxDisp,
            maxInvariant,
            maxUnstamped);
    test_assert(stamped > 0); // the attrs really were stamped
    test_assert(moved > 0);   // and really accumulated a displacement
    test_assert(maxDisp > 0.01f);
    test_assert(maxInvariant < 1e-6f); // disp == co - base on every stamped vert
    test_assert(maxUnstamped < 1e-6f);
  }

  return test_end();
}
