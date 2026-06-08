#pragma once

// Tests-only OBJ importer shim. The implementation now lives in the reusable,
// header-only source/mesh/utils/obj_io.h (so the remesh CLI / debug app share
// it); this one-arg overload preserves the long-standing test behavior of
// fan-triangulating every face on import, so existing tests need no churn.

#include "mesh/utils/obj_io.h"

namespace sculptcore::mesh {

static inline Mesh *loadObj(const char *path)
{
  return loadObj(path, false);
}

} // namespace sculptcore::mesh
