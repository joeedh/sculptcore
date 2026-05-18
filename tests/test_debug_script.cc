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

  return test_end();
}
