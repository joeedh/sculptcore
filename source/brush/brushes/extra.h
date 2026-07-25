#pragma once

/** Extra (out-of-repo) sbrush kernels, compiled in-build from the dirs in the
 * SCULPTCORE_EXTRA_KERNEL_DIRS cache var (see "Extra kernel dirs" in
 * documentation/brush_compute.md). When enabled, the generated registry
 * provides the enum items, factories and metadata; otherwise these inline
 * no-ops keep the executor's dispatch/live-links hooks compiling unchanged. */

#ifdef SCULPTCORE_EXTRA_BRUSHES

#include "sculptcore_extra_brushes.gen.h"

#else

#include "brush/brush_command.h"

namespace sculptcore::brush {

inline constexpr int extraBrushCount = 0;

inline bool extraBrushUsesForNeighbor(int /*id*/)
{
  return false;
}

} // namespace sculptcore::brush

namespace sculptcore::brush::command {

template <CommandTypes TYPES, sculptcore::brush::AccumMode AccMode>
inline bool createExtraBrush(int /*id*/, bool /*csrNeighbors*/,
                             BrushCommandDef<CommandCtx<TYPES>> & /*def*/)
{
  return false;
}

} // namespace sculptcore::brush::command

#endif
