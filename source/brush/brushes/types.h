#include "litestl/util/compiler_util.h"

namespace sculptcore::brush {
enum class _SculptBrushes {
  DRAW = 0,
};
MAKE_ENUM_CLASS(SculptBrushes, _SculptBrushes, int);
} // namespace sculptcore::brush