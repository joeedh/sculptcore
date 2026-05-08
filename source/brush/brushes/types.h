#include "litestl/binding/binding.h"
#include "litestl/util/compiler_util.h"

namespace sculptcore::brush {
enum class _SculptBrushes {
  DRAW = 0,
};
MAKE_ENUM_CLASS(SculptBrushes, _SculptBrushes, int);
} // namespace sculptcore::brush

namespace litestl::binding {
template <std::same_as<sculptcore::brush::SculptBrushes> T> static const types::Enum *Bind()
{
  using namespace sculptcore::brush;
  types::Enum *e = new types::Enum("sculptcore::brush::SculptBrushes", sizeof(SculptBrushes));
  e->addItem("DRAW", SculptBrushes::DRAW);
  return e;
}
} // namespace litestl::binding