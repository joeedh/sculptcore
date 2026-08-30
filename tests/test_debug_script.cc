#include "test_util.h"

#include "debug/scene.h"
#include "debug/script.h"

#include <cmath>
#include <cstdio>
#include <cstring>

test_init;

using namespace sculptcore::debug_app;

int main()
{
  /* Headless script that exercises mesh + assert verbs only — no GL. */
  {
    Scene scene(64, 64, /*headless=*/true);
    const char *src = "make_cube subdivs=4 size=1.0\n"
                      "assert_aabb min=-0.5,-0.5,-0.5 max=0.5,0.5,0.5 eps=1e-5\n";
    auto r = script::run(scene, src, ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  script line %d: %s\n", r.line_no, r.error.c_str());
    }
    test_assert(scene.mesh != nullptr);
    test_assert(scene.mesh->v.count > 0);
  }

  /* Dyntopo: triangulate a quad cube, refine under a dab. The mesh must stay
   * manifold (assert_manifold in-script) and grow substantially under the
   * brush. Triangulated subdivs=6 cube starts at 152 verts / 300 tris. */
  {
    Scene scene(64, 64, /*headless=*/true);
    const char *src = "make_cube subdivs=6 size=0.5\n"
                      "triangulate\n"
                      "build_spatial leaf_limit=256 depth_limit=8\n"
                      "set_brush radius=0.2 strength=0.0\n"
                      "dyntopo enabled=1 detail=0.04 mode=subdivide\n"
                      "stroke origin=0,0,0.25 normal=0,0,1\n"
                      "assert_manifold\n";
    auto r = script::run(scene, src, ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  dyntopo line %d: %s\n", r.line_no, r.error.c_str());
    }
    test_assert(scene.mesh != nullptr);
    test_assert(scene.mesh->v.count > 300); /* grew well past the base 152 */
    test_assert(scene.mesh->f.count > 600);
  }

  /* Dyntopo undo (M4): a dyntopo stroke (refine + deform) is fully undoable
   * back to the pre-stroke triangulated cube (152 v / 300 f), staying manifold.
   * The deform and the topology edit are separate steps, hence two undos. */
  {
    Scene scene(64, 64, /*headless=*/true);
    const char *src = "make_cube subdivs=6 size=0.5\n"
                      "triangulate\n"
                      "build_spatial leaf_limit=256 depth_limit=8\n"
                      "set_brush radius=0.2 strength=0.5\n"
                      "dyntopo enabled=1 detail=0.04 mode=subdivide\n"
                      "stroke origin=0,0,0.25 normal=0,0,1\n"
                      "assert_manifold\n"
                      "undo\n"
                      "undo\n"
                      "assert_manifold\n";
    auto r = script::run(scene, src, ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  dyntopo-undo line %d: %s\n", r.line_no, r.error.c_str());
    }
    test_assert(scene.mesh != nullptr);
    test_assert(scene.mesh->v.count == 152); /* restored to the triangulated cube */
    test_assert(scene.mesh->f.count == 300);
  }

  /* Parser: unknown verb → ok=false with a line number. */
  {
    Scene scene(64, 64, true);
    const char *src = "make_cube subdivs=2\n"
                      "this_is_not_a_verb foo=1\n";
    auto r = script::run(scene, src, ".");
    test_assert(!r.ok);
    test_assert(r.line_no == 2);
  }

  /* Parser: comments and blank lines must be skipped without error. */
  {
    Scene scene(64, 64, true);
    const char *src = "# a comment\n"
                      "\n"
                      "make_cube subdivs=2\n"
                      "# trailing comment without newline";
    auto r = script::run(scene, src, ".");
    test_assert(r.ok);
  }

  /* assert_verts: should detect mismatches. */
  {
    Scene scene(64, 64, true);
    const char *src = "make_cube subdivs=2\n"
                      "assert_verts n=9999999\n";
    auto r = script::run(scene, src, ".");
    test_assert(!r.ok);
    test_assert(r.line_no == 2);
  }

  /* stroke_path with `spacing=` walks the segment in world-space at
   * radius * spacing intervals and must displace at least some verts.
   * Sanity-checks the new dab-spacing path without locking in an exact
   * count. */
  {
    Scene scene(64, 64, true);
    const char *src =
        "make_cube subdivs=16 size=0.5\n"
        "build_spatial leaf_limit=256 depth_limit=8\n"
        "set_brush radius=0.18 strength=0.3 spacing=0.5\n"
        "stroke_path p1=-0.35,0,0.25 p2=0.35,0,0.25 normal=0,0,1 spacing=0.5\n";
    auto r = script::run(scene, src, ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  script line %d: %s\n", r.line_no, r.error.c_str());
    }
    test_assert(scene.mesh != nullptr);
    /* Some +Z face verts should have lifted above the original z=0.25 face. */
    bool moved = false;
    if (scene.mesh) {
      for (int i = 0; i < scene.mesh->v.count; i++) {
        if (scene.mesh->v.co[i][2] > 0.25f + 1e-4f) {
          moved = true;
          break;
        }
      }
    }
    test_assert(moved);
  }

  /* Inflate brush: pushes each vertex along its own normal, gated by
   * (1 - mask). Default mask is 0, so the +Z stroke center must lift
   * verts above z=0.25 — proving the sbrush-generated kernel runs and
   * that v.no and v.mask resolve correctly through the iterator. */
  {
    Scene scene(64, 64, true);
    const char *src = "make_cube subdivs=12 size=0.5\n"
                      "build_spatial leaf_limit=256 depth_limit=8\n"
                      "set_brush_tool tool=inflate\n"
                      "set_brush radius=0.25 strength=0.5\n"
                      "stroke origin=0,0,0.25 normal=0,0,1\n";
    auto r = script::run(scene, src, ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  script line %d: %s\n", r.line_no, r.error.c_str());
    }
    /* Center of brush is on the +Z face; the center vert should move
     * upward by ~strength*radius*0.1 = 0.0125 — well above the noise
     * threshold. */
    float maxZ = -1e9f;
    if (scene.mesh) {
      for (int i = 0; i < scene.mesh->v.count; i++) {
        float z = scene.mesh->v.co[i][2];
        if (z > maxZ)
          maxZ = z;
      }
    }
    test_assert(maxZ > 0.25f + 5e-3f);
  }

  /* Clay brush: at stroke (0,0,0) with normal (0,0,1), the bottom-face
   * center vert (originally z=-0.25) is below the cutting plane (h<0)
   * and well inside the brush — it must rise. Proves clay's plane test
   * and that ctx.surfaceNo/Pos resolved correctly. */
  {
    Scene scene(64, 64, true);
    const char *src = "make_cube subdivs=12 size=0.5\n"
                      "build_spatial leaf_limit=256 depth_limit=8\n"
                      "set_brush_tool tool=clay\n"
                      "set_brush radius=0.5 strength=0.5\n"
                      "stroke origin=0,0,0 normal=0,0,1\n";
    auto r = script::run(scene, src, ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  script line %d: %s\n", r.line_no, r.error.c_str());
    }
    /* Find the vert nearest the bottom-face center (xy ≈ 0, z ≈ -0.25). */
    float bestD = 1e9f;
    float bestZ = -1e9f;
    if (scene.mesh) {
      for (int i = 0; i < scene.mesh->v.count; i++) {
        float x = scene.mesh->v.co[i][0];
        float y = scene.mesh->v.co[i][1];
        float z = scene.mesh->v.co[i][2];
        if (z > -0.20f)
          continue;
        float d = x * x + y * y;
        if (d < bestD) {
          bestD = d;
          bestZ = z;
        }
      }
    }
    test_assert(bestZ > -0.25f + 1e-3f);
  }

  /* Pinch brush: at stroke (0,0,0.25) on the +Z face, surface verts get
   * pulled toward the stroke center. Verts that started on the cube's
   * outer radius (x=±0.25 or y=±0.25 with z=0.25) must end with reduced
   * xy magnitude — proves the length()/dot() member-call intrinsics and
   * the conditional guard wired correctly. */
  {
    Scene scene(64, 64, true);
    const char *src = "make_cube subdivs=12 size=0.5\n"
                      "build_spatial leaf_limit=256 depth_limit=8\n"
                      "set_brush_tool tool=pinch\n"
                      "set_brush radius=0.25 strength=0.1\n"
                      "stroke origin=0,0,0.25 normal=0,0,1\n";
    auto r = script::run(scene, src, ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  script line %d: %s\n", r.line_no, r.error.c_str());
    }
    /* Count verts on the +Z face whose xy moved inward (toward 0,0). */
    int pulled = 0;
    if (scene.mesh) {
      for (int i = 0; i < scene.mesh->v.count; i++) {
        float z = scene.mesh->v.co[i][2];
        if (z < 0.245f)
          continue;
        float r2 = scene.mesh->v.co[i][0] * scene.mesh->v.co[i][0] +
                   scene.mesh->v.co[i][1] * scene.mesh->v.co[i][1];
        if (r2 < 0.245f * 0.245f * 0.5f)
          pulled++;
      }
    }
    test_assert(pulled > 0);
  }

  /* Sharp brush: pushes verts along the brush surface normal (ridge) and
   * pinches the displaced region toward the brush axis by `pinch`. Run the
   * same stroke with pinch=0 and pinch=0.9: both must lift the +Z face
   * (the normal push), and the pinched run must pull the footprint's verts
   * measurably closer to the brush axis than the unpinched run — proving
   * the tangent pull routes through the `pinch` @static uniform. */
  {
    auto runSharp = [&](const char *pinchArg, float &r_maxZ, double &r_radSum) {
      Scene scene(64, 64, true);
      std::string src;
      src += "make_cube subdivs=12 size=0.5\n";
      src += "build_spatial leaf_limit=256 depth_limit=8\n";
      src += "set_brush_tool tool=sharp\n";
      src += "set_brush radius=0.25 strength=0.1";
      src += pinchArg;
      src += "\n";
      src += "stroke origin=0,0,0.25 normal=0,0,1\n";
      auto r = script::run(scene, src.c_str(), ".");
      test_assert(r.ok);
      if (!r.ok) {
        fprintf(stderr, "  sharp script line %d: %s\n", r.line_no, r.error.c_str());
      }
      r_maxZ = -1e9f;
      r_radSum = 0.0;
      if (scene.mesh) {
        for (int i = 0; i < scene.mesh->v.count; i++) {
          float z = scene.mesh->v.co[i][2];
          if (z < 0.245f)
            continue;
          if (z > r_maxZ)
            r_maxZ = z;
          float x = scene.mesh->v.co[i][0], y = scene.mesh->v.co[i][1];
          r_radSum += std::sqrt(double(x * x + y * y));
        }
      }
    };
    float maxZPlain = 0.0f, maxZPinch = 0.0f;
    double radPlain = 0.0, radPinch = 0.0;
    runSharp("", maxZPlain, radPlain);
    runSharp(" pinch=0.9", maxZPinch, radPinch);
    /* Both runs displace along the normal: the +Z face rises. */
    test_assert(maxZPlain > 0.25f + 1e-3f);
    test_assert(maxZPinch > 0.25f + 1e-3f);
    /* The pinched run pulls the face's verts toward the brush axis. */
    test_assert(radPinch < radPlain - 1e-3);
  }

  /* Mask brush: writes to v.mask (PtrHelper::mask is a reference, so the
   * write back into mesh data must persist). After the stroke at the +Z
   * face, the v.mask attribute for the brush-center vert must be > 0 —
   * proves the lvalue-vertex-field write path through emit_cpp. */
  {
    Scene scene(64, 64, true);
    const char *src = "make_cube subdivs=12 size=0.5\n"
                      "build_spatial leaf_limit=256 depth_limit=8\n"
                      "set_brush_tool tool=mask\n"
                      "set_brush radius=0.25 strength=0.5\n"
                      "stroke origin=0,0,0.25 normal=0,0,1\n";
    auto r = script::run(scene, src, ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  script line %d: %s\n", r.line_no, r.error.c_str());
    }
    float maxMask = 0.0f;
    if (scene.mesh && scene.tree) {
      auto &mk = scene.tree->treeMesh.v.mask;
      for (int i = 0; i < scene.mesh->v.count; i++) {
        if (mk[i] > maxMask)
          maxMask = mk[i];
      }
    }
    test_assert(maxMask > 1e-4f);
  }

  /* Smooth brush: averages each vert's neighbors and lerps. Drop a
   * single tall spike on the +Z face first (so there's something *to*
   * smooth), then smooth the same area and check the spike got shorter.
   * Proves the for_neighbor / EdgeOfVertIter expansion compiles and the
   * inline neighbor-view struct exposes nb.co correctly. */
  {
    Scene scene(64, 64, true);
    const char *src = "make_cube subdivs=16 size=0.5\n"
                      "build_spatial leaf_limit=256 depth_limit=8\n"
                      "set_brush_tool tool=inflate\n"
                      "set_brush radius=0.05 strength=4.0\n"
                      "stroke origin=0,0,0.25 normal=0,0,1\n";
    auto r = script::run(scene, src, ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  script line %d: %s\n", r.line_no, r.error.c_str());
    }
    float spikeZ = -1e9f;
    if (scene.mesh) {
      for (int i = 0; i < scene.mesh->v.count; i++) {
        float z = scene.mesh->v.co[i][2];
        if (z > spikeZ)
          spikeZ = z;
      }
    }
    /* The smooth kernel lerps each vert toward its neighbor average by
     * s = strength*falloff, so keep strength <= 1 (a larger factor
     * overshoots the mean and oscillates instead of settling) and repeat
     * so the spike measurably redistributes. */
    const char *src2 = "set_brush_tool tool=smooth\n"
                       "set_brush radius=0.15 strength=1.0\n"
                       "stroke origin=0,0,0.25 normal=0,0,1\n"
                       "stroke origin=0,0,0.25 normal=0,0,1\n"
                       "stroke origin=0,0,0.25 normal=0,0,1\n";
    auto r2 = script::run(scene, src2, ".");
    test_assert(r2.ok);
    if (!r2.ok) {
      fprintf(stderr, "  smooth script line %d: %s\n", r2.line_no, r2.error.c_str());
    }
    float smoothedZ = -1e9f;
    if (scene.mesh) {
      for (int i = 0; i < scene.mesh->v.count; i++) {
        float z = scene.mesh->v.co[i][2];
        if (z > smoothedZ)
          smoothedZ = z;
      }
    }
    /* Smoothing must shrink the spike by a meaningful amount. */
    test_assert(spikeZ > 0.26f);
    test_assert(smoothedZ < spikeZ - 1e-3f);
  }

  /* set_falloff: swapping the brush's falloff curve must change the
   * displacement profile of an otherwise-identical stroke. We run the
   * same inflate stroke twice — once with the default Smoothstep and
   * once with Gaussian — and compare the +Z face's peak rise. Gaussian
   * decays faster (exp(-9*(1-t)^2) hits ~0.001 at t=0.1) so the peak
   * rise should be measurably *lower* than smoothstep's t^2(3-2t),
   * which still returns ~0.028 at t=0.1. Confirms set_falloff routes
   * through to Brush::falloff_kind and CommandCtx::strength dispatches
   * on it. */
  {
    float zSmoothstep = 0.0f, zGaussian = 0.0f;
    for (int pass = 0; pass < 2; pass++) {
      Scene scene(64, 64, true);
      const char *src1 = "make_cube subdivs=12 size=0.5\n"
                         "build_spatial leaf_limit=256 depth_limit=8\n"
                         "set_brush_tool tool=inflate\n"
                         "set_brush radius=0.25 strength=0.5\n";
      auto r1 = script::run(scene, src1, ".");
      test_assert(r1.ok);
      if (pass == 1) {
        auto r2 = script::run(scene, "set_falloff kind=gaussian\n", ".");
        test_assert(r2.ok);
      }
      auto r3 = script::run(scene, "stroke origin=0,0,0.25 normal=0,0,1\n", ".");
      test_assert(r3.ok);
      float maxZ = -1e9f;
      if (scene.mesh) {
        for (int i = 0; i < scene.mesh->v.count; i++) {
          float z = scene.mesh->v.co[i][2];
          if (z > maxZ)
            maxZ = z;
        }
      }
      if (pass == 0)
        zSmoothstep = maxZ;
      else
        zGaussian = maxZ;
    }
    test_assert(zSmoothstep > 0.25f + 5e-3f);
    test_assert(zGaussian > 0.25f + 5e-3f);
    /* Gaussian falls off faster than smoothstep across the radius, so
     * the total volume of displaced verts is smaller. The peak vert
     * (closest to brush center, t≈1) sees ~1 from both curves so the
     * peak heights are close — but the *near-edge* verts barely move
     * under gaussian, so the volume integral and thus the peak when
     * the brush is centered between verts differs. A loose `not equal`
     * test would be fragile under fp; instead assert gaussian doesn't
     * exceed smoothstep by any meaningful amount (proves the dispatch
     * actually changed behavior rather than no-oping). */
    test_assert(zGaussian <= zSmoothstep + 1e-4f);
    float diff = zSmoothstep - zGaussian;
    if (diff < 0)
      diff = -diff;
    test_assert(diff > 1e-5f); /* gaussian must measurably differ */
  }

  /* FalloffKind::Curve LUT path. Two checks:
   *  (a) `kind=curve` with the default smoothstep-shaped LUT must
   *      match `kind=smoothstep` within fp tolerance — verifies the
   *      LUT default ships in lockstep with the analytic curve.
   *  (b) overwriting the LUT with the `inverse` preset (1-t) must
   *      drop the brush-center displacement well below the smoothstep
   *      run, since the inverse curve gives ~0 strength at t=1
   *      (center). Confirms the LUT actually drives the kernel rather
   *      than being shadowed by the analytic fast path. */
  {
    auto runInflate = [&](const char *prelude) -> float {
      Scene scene(64, 64, true);
      std::string src;
      src += "make_cube subdivs=12 size=0.5\n";
      src += "build_spatial leaf_limit=256 depth_limit=8\n";
      src += "set_brush_tool tool=inflate\n";
      src += "set_brush radius=0.25 strength=0.5\n";
      src += prelude;
      src += "stroke origin=0,0,0.25 normal=0,0,1\n";
      auto r = script::run(scene, src.c_str(), ".");
      test_assert(r.ok);
      /* Measure the brush-*center* vert (nearest xy to 0,0 on the +Z
       * face), not the global max. Inflate lifts along the normal, so
       * under the inverse curve (≈0 strength at t=1) the edge verts rise
       * just as high as the center does under smoothstep — a global max
       * can't tell the two apart. The center vert can. */
      float bestD = 1e9f, centerZ = 0.0f;
      if (scene.mesh) {
        for (int i = 0; i < scene.mesh->v.count; i++) {
          float z = scene.mesh->v.co[i][2];
          if (z < 0.245f)
            continue;
          float x = scene.mesh->v.co[i][0], y = scene.mesh->v.co[i][1];
          float d = x * x + y * y;
          if (d < bestD) {
            bestD = d;
            centerZ = z;
          }
        }
      }
      return centerZ;
    };
    float zSmoothstep = runInflate("");
    float zCurveDefault = runInflate("set_falloff kind=curve\n");
    float zCurveInverse = runInflate("set_falloff_curve preset=inverse\n"
                                     "set_falloff kind=curve\n");
    /* (a) Default curve ~= smoothstep. The LUT has 255 segments so
     * linear-interp samples differ from the analytic curve by at most
     * the curvature times (1/255)^2; a 2e-3 z-tolerance is well above
     * that and well below the inverse delta. */
    float diffDefault = zSmoothstep - zCurveDefault;
    if (diffDefault < 0)
      diffDefault = -diffDefault;
    test_assert(diffDefault < 2e-3f);
    /* (b) Inverse-LUT brush must lift the center *less* than the
     * smoothstep brush. Difference well above noise threshold. */
    test_assert(zSmoothstep - zCurveInverse > 5e-3f);
  }

  /* FalloffShape spatial metric. Inflate at the +Z face center (the face
   * spans x,y in [-0.25,0.25]; radius 0.25). The cube metric
   * max(|dx|,|dy|,|dz|) covers a square footprint that strictly contains
   * the spherical disc — every vert sees t_cube >= t_spherical, and the
   * corners the disc misses get lifted — so summed +Z is strictly larger.
   * Linear with dir=X weights only the x-projection, lifting a full band
   * across y, so it differs measurably from the disc. Confirms
   * set_falloff shape=/dir= routes through Brush::falloffDist. */
  {
    auto sumZ = [&](const char *prelude) -> double {
      Scene scene(64, 64, true);
      std::string src;
      src += "make_cube subdivs=12 size=0.5\n";
      src += "build_spatial leaf_limit=256 depth_limit=8\n";
      src += "set_brush_tool tool=inflate\n";
      src += "set_brush radius=0.25 strength=0.5\n";
      src += prelude;
      src += "stroke origin=0,0,0.25 normal=0,0,1\n";
      auto r = script::run(scene, src.c_str(), ".");
      test_assert(r.ok);
      double s = 0.0;
      if (scene.mesh) {
        for (int i = 0; i < scene.mesh->v.count; i++) {
          s += scene.mesh->v.co[i][2];
        }
      }
      return s;
    };
    double zSpherical = sumZ("set_falloff shape=spherical\n");
    double zCube = sumZ("set_falloff shape=cube\n");
    double zLinear = sumZ("set_falloff shape=linear dir=1,0,0\n");
    /* cube footprint contains the disc -> strictly more upward lift */
    test_assert(zCube > zSpherical + 1e-3);
    /* linear band differs measurably from the spherical disc */
    double dl = zLinear - zSpherical;
    if (dl < 0)
      dl = -dl;
    test_assert(dl > 1e-3);
  }

  /* Pose brush: three cage anchors stay put, the +Z anchor moves to z=0.7.
   * +Z face verts must lift (closest to the moved anchor); the -Z face
   * center must stay nearly fixed (closer to the stationary anchors).
   * Proves Array<float3, 4> ctx fields and subscript exprs lower through
   * both the emitter and the script verbs. */
  {
    Scene scene(64, 64, true);
    const char *src = "make_cube subdivs=12 size=0.5\n"
                      "build_spatial leaf_limit=256 depth_limit=8\n"
                      "set_brush_tool tool=pose\n"
                      "set_brush radius=0.6 strength=1.0\n"
                      "set_pose_cage_rest idx=0 pos=0.5,0,0\n"
                      "set_pose_cage_rest idx=1 pos=-0.5,0,0\n"
                      "set_pose_cage_rest idx=2 pos=0,0.5,0\n"
                      "set_pose_cage_rest idx=3 pos=0,0,0.5\n"
                      "set_pose_cage_now  idx=0 pos=0.5,0,0\n"
                      "set_pose_cage_now  idx=1 pos=-0.5,0,0\n"
                      "set_pose_cage_now  idx=2 pos=0,0.5,0\n"
                      "set_pose_cage_now  idx=3 pos=0,0,0.7\n"
                      "stroke origin=0,0,0.5 normal=0,0,1\n";
    auto r = script::run(scene, src, ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  pose script line %d: %s\n", r.line_no, r.error.c_str());
    }
    /* Max z on the +Z face must rise above the starting 0.25. */
    float maxZ = -1e9f;
    /* Min |z + 0.25| on the -Z face — must stay near the original. */
    float worstBottom = 0.0f;
    if (scene.mesh) {
      for (int i = 0; i < scene.mesh->v.count; i++) {
        float z = scene.mesh->v.co[i][2];
        if (z > maxZ)
          maxZ = z;
        if (z < -0.20f) {
          float d = z + 0.25f;
          if (d < 0)
            d = -d;
          if (d > worstBottom)
            worstBottom = d;
        }
      }
    }
    test_assert(maxZ > 0.25f + 1e-3f);
    test_assert(worstBottom < 0.05f);
  }

  /* Brush texture modulation (Wave 2). A draw stroke on the +Z face with a
   * `rampx` grayscale texture under the GLOBAL coord space, whose tile spans
   * world [-1, 1]^2 — texel = (co.x + 1) / 2, growing with co.x — so the +x
   * half of the brush footprint must rise strictly more than the -x half.
   * Exercises the full path: sampleBrushTex intrinsic -> the generated
   * draw kernel -> CommandCtx::sampleBrushTex -> Brush::sampleTexBilinear,
   * plus the set_texture / set_coord_space verbs. GLOBAL is the only mode
   * testable here because the stroke verb leaves renderMatrix unset. */
  {
    Scene scene(64, 64, true);
    const char *src = "make_cube subdivs=12 size=0.5\n"
                      "build_spatial leaf_limit=256 depth_limit=8\n"
                      "set_brush_tool tool=draw\n"
                      "set_brush radius=0.25 strength=10.0\n"
                      "set_texture pattern=rampx width=64 height=64\n"
                      "set_coord_space space=global\n"
                      "stroke origin=0,0,0.25 normal=0,0,1\n";
    auto r = script::run(scene, src, ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  texture script line %d: %s\n", r.line_no, r.error.c_str());
    }
    /* Peak rise on the +x half vs the -x half of the +Z face. */
    float maxZRight = -1e9f, maxZLeft = -1e9f;
    if (scene.mesh) {
      for (int i = 0; i < scene.mesh->v.count; i++) {
        float z = scene.mesh->v.co[i][2];
        if (z < 0.24f)
          continue; /* +Z face only */
        float x = scene.mesh->v.co[i][0];
        if (x > 0.05f) {
          if (z > maxZRight)
            maxZRight = z;
        } else if (x < -0.05f) {
          if (z > maxZLeft)
            maxZLeft = z;
        }
      }
    }
    /* Both halves lift (the ramp is nonzero across the face), the +x half
     * strictly more. */
    test_assert(maxZRight > 0.25f + 1e-3f);
    test_assert(maxZRight > maxZLeft + 1e-3f);

    /* Same stroke with the texture cleared lifts both halves equally,
     * proving the asymmetry above came from the texture, not geometry. */
    Scene scene2(64, 64, true);
    const char *src2 = "make_cube subdivs=12 size=0.5\n"
                       "build_spatial leaf_limit=256 depth_limit=8\n"
                       "set_brush_tool tool=draw\n"
                       "set_brush radius=0.25 strength=10.0\n"
                       "stroke origin=0,0,0.25 normal=0,0,1\n";
    auto r2 = script::run(scene2, src2, ".");
    test_assert(r2.ok);
    float maxZRight2 = -1e9f, maxZLeft2 = -1e9f;
    if (scene2.mesh) {
      for (int i = 0; i < scene2.mesh->v.count; i++) {
        float z = scene2.mesh->v.co[i][2];
        if (z < 0.24f)
          continue;
        float x = scene2.mesh->v.co[i][0];
        if (x > 0.05f) {
          if (z > maxZRight2)
            maxZRight2 = z;
        } else if (x < -0.05f) {
          if (z > maxZLeft2)
            maxZLeft2 = z;
        }
      }
    }
    float sym = maxZRight2 - maxZLeft2;
    if (sym < 0)
      sym = -sym;
    test_assert(maxZLeft2 > 0.25f + 1e-3f); /* untextured: -x half rises too */
    test_assert(sym < 5e-3f);               /* and roughly symmetric */
  }

  /* STROKE_CURVED coord space (Wave 2). A multi-dab stroke_path runs along
   * +Y on the +Z face; with a `rampx` texture sampled in stroke_curved space
   * the texel value equals the arc length along the stroke, so displacement
   * must grow from the -Y end to the +Y end. The discriminator vs GLOBAL: the
   * rampx texel field is Y-independent there, so only the StrokePath
   * arc-length mapping produces a far-ward (+Y) gradient. Exercises
   * Brush::strokePath / sampleStrokeUV + the executor push/reset wiring. */
  {
    Scene scene(64, 64, true);
    const char *src = "make_cube subdivs=16 size=0.5\n"
                      "build_spatial leaf_limit=256 depth_limit=8\n"
                      "set_brush_tool tool=draw\n"
                      "set_brush radius=0.15 strength=8.0\n"
                      "set_texture pattern=rampx width=64 height=64\n"
                      "set_coord_space space=stroke_curved\n"
                      "stroke_path p1=0,-0.2,0.25 p2=0,0.2,0.25 steps=8\n";
    auto r = script::run(scene, src, ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  stroke_curved script line %d: %s\n", r.line_no, r.error.c_str());
    }
    /* Peak rise at the far (+Y) end vs the near (-Y) end of the stroke. */
    float maxZFar = -1e9f, maxZNear = -1e9f;
    if (scene.mesh) {
      for (int i = 0; i < scene.mesh->v.count; i++) {
        float z = scene.mesh->v.co[i][2];
        if (z < 0.24f)
          continue; /* +Z face only */
        float y = scene.mesh->v.co[i][1];
        if (y > 0.1f) {
          if (z > maxZFar)
            maxZFar = z;
        } else if (y < -0.1f) {
          if (z > maxZNear)
            maxZNear = z;
        }
      }
    }
    /* Arc length ~0 at the -Y end (texel ~0, no lift) and ~0.4 at the +Y
     * end (texel ~0.4, clear lift): a monotone gradient along the stroke. */
    test_assert(maxZFar > 0.25f + 1e-3f);
    test_assert(maxZFar > maxZNear + 1e-3f);

    /* Same stroke under GLOBAL: the rampx texel field only varies with co.x,
     * so the texture drives no +Y gradient — any far/near difference is
     * dab-order accumulation, which favors the *near* end. This isolates the
     * STROKE_CURVED behavior above (the only mode whose UV tracks distance
     * *along* the stroke). */
    Scene scene2(64, 64, true);
    const char *src2 = "make_cube subdivs=16 size=0.5\n"
                       "build_spatial leaf_limit=256 depth_limit=8\n"
                       "set_brush_tool tool=draw\n"
                       "set_brush radius=0.15 strength=8.0\n"
                       "set_texture pattern=rampx width=64 height=64\n"
                       "set_coord_space space=global\n"
                       "stroke_path p1=0,-0.2,0.25 p2=0,0.2,0.25 steps=8\n";
    auto r2 = script::run(scene2, src2, ".");
    test_assert(r2.ok);
    float maxZFarG = -1e9f, maxZNearG = -1e9f;
    if (scene2.mesh) {
      for (int i = 0; i < scene2.mesh->v.count; i++) {
        float z = scene2.mesh->v.co[i][2];
        if (z < 0.24f)
          continue;
        float y = scene2.mesh->v.co[i][1];
        if (y > 0.1f) {
          if (z > maxZFarG)
            maxZFarG = z;
        } else if (y < -0.1f) {
          if (z > maxZNearG)
            maxZNearG = z;
        }
      }
    }
    test_assert(maxZFarG > 0.25f + 1e-3f); /* still lifts */
    /* No texture-driven far-ward gradient (stroke_curved's discriminator). */
    test_assert(maxZFarG - maxZNearG < 1e-3f);
  }

  return test_end();
}
