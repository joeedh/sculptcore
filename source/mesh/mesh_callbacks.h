#pragma once

#include "litestl/util/function.h"

namespace sculptcore::mesh {

/**
 * Optional callback bundle threaded through Mesh topology operations.
 *
 * Each topology op (make_vertex, make_edge, make_face, kill_vertex,
 * kill_edge, kill_face) takes a trailing `MeshCallbacks *cb = nullptr`
 * argument. The op fires the relevant callbacks at each create / kill /
 * change event it produces; callbacks that are unset (empty
 * `util::function`) are skipped, as is the whole struct when `cb` is null.
 *
 * Convention: the struct pointer AND each individual callback are
 * null-checked at the call site, so a producer can subscribe to only the
 * events it cares about.
 *
 * "Change" means an element survived the op but had its topology / disk /
 * radial pointers rewritten (e.g. a vertex's incident-edge pointer when
 * an edge is created or killed on it).
 *
 * Kill callbacks are fired BEFORE the underlying free, so consumers can
 * still read attributes by index.
 */
struct MeshCallbacks {
  litestl::util::function<void(int v)> onVertCreate;
  litestl::util::function<void(int v)> onVertKill;
  litestl::util::function<void(int v)> onVertChange;

  litestl::util::function<void(int e)> onEdgeCreate;
  litestl::util::function<void(int e)> onEdgeKill;
  litestl::util::function<void(int e)> onEdgeChange;

  litestl::util::function<void(int c)> onCornerCreate;
  litestl::util::function<void(int c)> onCornerKill;
  litestl::util::function<void(int c)> onCornerChange;

  litestl::util::function<void(int l)> onListCreate;
  litestl::util::function<void(int l)> onListKill;
  litestl::util::function<void(int l)> onListChange;

  litestl::util::function<void(int f)> onFaceCreate;
  litestl::util::function<void(int f)> onFaceKill;
  litestl::util::function<void(int f)> onFaceChange;
};

} // namespace sculptcore::mesh
