#pragma once

#include "litestl/util/string.h"

namespace sculptcore::debug_app {

struct Scene;

namespace script {

struct RunResult {
  bool ok = true;
  int line_no = 0;
  litestl::util::string error;
};

/** Execute a script source string against `scene`. Lines are
 *  `verb key=value key=value` with `#` comments. `out_dir` is the
 *  base directory for relative paths in `screenshot`/`dump_state`. */
RunResult run(Scene &scene, const char *source, const char *out_dir);

/** Convenience: read `path` and call run(). */
RunResult runFile(Scene &scene, const char *path, const char *out_dir);

} // namespace script
} // namespace sculptcore::debug_app
