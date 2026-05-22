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

  return test_end();
}
