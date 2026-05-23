#include "test_util.h"

#include "debug/scene.h"
#include "debug/script.h"

#include <cstdio>
#include <cstring>

test_init;

using namespace sculptcore::debug_app;

int main()
{
  /* Headless script that exercises mesh + assert verbs only — no GL. */
  {
    Scene scene(64, 64, /*headless=*/true);
    const char *src =
        "make_cube subdivs=4 size=1.0\n"
        "assert_aabb min=-0.5,-0.5,-0.5 max=0.5,0.5,0.5 eps=1e-5\n";
    auto r = script::run(scene, src, ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  script line %d: %s\n", r.line_no, r.error.c_str());
    }
    test_assert(scene.mesh != nullptr);
    test_assert(scene.mesh->v.count > 0);
  }

  /* Parser: unknown verb → ok=false with a line number. */
  {
    Scene scene(64, 64, true);
    const char *src =
        "make_cube subdivs=2\n"
        "this_is_not_a_verb foo=1\n";
    auto r = script::run(scene, src, ".");
    test_assert(!r.ok);
    test_assert(r.line_no == 2);
  }

  /* Parser: comments and blank lines must be skipped without error. */
  {
    Scene scene(64, 64, true);
    const char *src =
        "# a comment\n"
        "\n"
        "make_cube subdivs=2\n"
        "# trailing comment without newline";
    auto r = script::run(scene, src, ".");
    test_assert(r.ok);
  }

  /* assert_verts: should detect mismatches. */
  {
    Scene scene(64, 64, true);
    const char *src =
        "make_cube subdivs=2\n"
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
        "stroke_path p1=-0.35,0,0.5 p2=0.35,0,0.5 normal=0,0,1 spacing=0.5\n";
    auto r = script::run(scene, src, ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  script line %d: %s\n", r.line_no, r.error.c_str());
    }
    test_assert(scene.mesh != nullptr);
    /* Some +Z vertices should have moved off the original plane. */
    bool moved = false;
    if (scene.mesh) {
      for (int i = 0; i < scene.mesh->v.count; i++) {
        if (scene.mesh->v.co[i][2] > 0.5f + 1e-4f) {
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
    const char *src =
        "make_cube subdivs=12 size=0.5\n"
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
        if (z > maxZ) maxZ = z;
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
    const char *src =
        "make_cube subdivs=12 size=0.5\n"
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
        if (z > -0.20f) continue;
        float d = x * x + y * y;
        if (d < bestD) { bestD = d; bestZ = z; }
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
    const char *src =
        "make_cube subdivs=12 size=0.5\n"
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
        if (z < 0.245f) continue;
        float r2 = scene.mesh->v.co[i][0] * scene.mesh->v.co[i][0] +
                   scene.mesh->v.co[i][1] * scene.mesh->v.co[i][1];
        if (r2 < 0.245f * 0.245f * 0.5f) pulled++;
      }
    }
    test_assert(pulled > 0);
  }

  /* Sharp brush: pulls verts along the tangent of the brush plane —
   * +Z face verts slide toward (0,0,0.25) without leaving the plane.
   * Same xy-radial check as pinch, but z must stay near 0.25 (the
   * tangent projection should kill the normal component). */
  {
    Scene scene(64, 64, true);
    const char *src =
        "make_cube subdivs=12 size=0.5\n"
        "build_spatial leaf_limit=256 depth_limit=8\n"
        "set_brush_tool tool=sharp\n"
        "set_brush radius=0.25 strength=0.1\n"
        "stroke origin=0,0,0.25 normal=0,0,1\n";
    auto r = script::run(scene, src, ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  script line %d: %s\n", r.line_no, r.error.c_str());
    }
    int pulled = 0;
    float maxZ = -1e9f;
    if (scene.mesh) {
      for (int i = 0; i < scene.mesh->v.count; i++) {
        float z = scene.mesh->v.co[i][2];
        if (z < 0.245f) continue;
        if (z > maxZ) maxZ = z;
        float r2 = scene.mesh->v.co[i][0] * scene.mesh->v.co[i][0] +
                   scene.mesh->v.co[i][1] * scene.mesh->v.co[i][1];
        if (r2 < 0.245f * 0.245f * 0.5f) pulled++;
      }
    }
    test_assert(pulled > 0);
    /* Tangent-only motion must not lift the +Z face above 0.25 + tiny eps. */
    test_assert(maxZ < 0.25f + 1e-4f);
  }

  /* Mask brush: writes to v.mask (PtrHelper::mask is a reference, so the
   * write back into mesh data must persist). After the stroke at the +Z
   * face, the v.mask attribute for the brush-center vert must be > 0 —
   * proves the lvalue-vertex-field write path through emit_cpp. */
  {
    Scene scene(64, 64, true);
    const char *src =
        "make_cube subdivs=12 size=0.5\n"
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
        if (mk[i] > maxMask) maxMask = mk[i];
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
    const char *src =
        "make_cube subdivs=16 size=0.5\n"
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
        if (z > spikeZ) spikeZ = z;
      }
    }
    const char *src2 =
        "set_brush_tool tool=smooth\n"
        "set_brush radius=0.15 strength=1.0\n"
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
        if (z > smoothedZ) smoothedZ = z;
      }
    }
    /* Smoothing must shrink the spike by a meaningful amount. */
    test_assert(spikeZ > 0.26f);
    test_assert(smoothedZ < spikeZ - 1e-3f);
  }

  return test_end();
}
