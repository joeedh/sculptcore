#pragma once

#include "brush/brushes/types.h"

#include "litestl/util/vector.h"

namespace sculptcore::mesh {
struct Mesh;
}
namespace sculptcore::spatial {
struct SpatialNode;
}

namespace sculptcore::brush {

struct Brush;
struct CommandExecutor;

/** Context handed to every per-tool hook: the executor plus the dab's mesh and
 * in-region spatial nodes. `m` may be null when the node set is empty. */
struct BrushHookCtx {
  CommandExecutor &exec;
  mesh::Mesh *m = nullptr;
  litestl::util::Vector<spatial::SpatialNode *> *nodes = nullptr;
  Brush *brush = nullptr;
  bool isFirstOfStep = false;
};

using BrushHookFn = void (*)(BrushHookCtx &);

/** The host-side passes a tool needs around its kernel, as a uniform shape.
 * The executors invoke each phase without knowing which tool wants it, so a
 * tool with host work is one table row in brush_hooks.cc rather than a
 * conditional at every call site. */
struct BrushHooks {
  /** Stroke start (first dab), before the per-dab topology freeze; topology
   * links are live. E.g. BSMOOTH/FEATURE_ALIGN's boundary-class refresh. */
  BrushHookFn stepPreFreeze = nullptr;
  /** Every dab, after freeze/thaw and before the kernel runs. E.g. ENHANCE's
   * difference-of-smooths region fill and FEATURE_ALIGN's cross-field update
   * (both live-links tools, so topology is thawed here). */
  BrushHookFn dabPre = nullptr;
  /** Every dab, after the kernel ran. E.g. POLYGROUP's boundary-dirty mark. */
  BrushHookFn dabPost = nullptr;
  /** Safe before prepared execution, independent of stage scalars and dab order. */
  bool preparedInvariant = false;
  /** Grids use the read-only zero boundary column and need no mesh refresh. */
  bool preparedGridSkip = false;
  /** Command-specific preparation runs inside exec after base initialization. */
  bool preparedPerCommand = false;
  bool preparedStageHooks = false;
};

/** Tool-keyed hook lookup; null when the tool needs no host-side passes. */
const BrushHooks *brushHooksFor(SculptBrushes tool);

inline bool preparedHooksSupported(SculptBrushes tool, bool grid)
{
  const auto *hooks = brushHooksFor(tool);
  if (hooks && hooks->preparedStageHooks)
    return !grid;
  if (hooks && hooks->preparedPerCommand)
    return !grid && !hooks->stepPreFreeze && !hooks->dabPost;
  return !hooks || (hooks->preparedInvariant && !hooks->dabPre && !hooks->dabPost &&
                    (!grid || hooks->preparedGridSkip));
}

} // namespace sculptcore::brush
