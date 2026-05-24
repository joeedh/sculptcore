#include "litestl/binding/binding.h"
#include "litestl/util/compiler_util.h"

namespace sculptcore::brush {
enum class _SculptBrushes {
  DRAW = 0,
  INFLATE = 1,
  CLAY = 2,
  PINCH = 3,
  SHARP = 4,
  MASK = 5,
  SMOOTH = 6,
  KELVINLET = 7,
};
MAKE_ENUM_CLASS(SculptBrushes, _SculptBrushes, int);
} // namespace sculptcore::brush

namespace litestl::binding {
template <std::same_as<sculptcore::brush::SculptBrushes> T> static const types::Enum *Bind()
{
  using namespace sculptcore::brush;
  types::Enum *e = new types::Enum("sculptcore::brush::SculptBrushes", sizeof(SculptBrushes));
  e->addItem("DRAW", SculptBrushes::DRAW);
  e->addItem("INFLATE", SculptBrushes::INFLATE);
  e->addItem("CLAY", SculptBrushes::CLAY);
  e->addItem("PINCH", SculptBrushes::PINCH);
  e->addItem("SHARP", SculptBrushes::SHARP);
  e->addItem("MASK", SculptBrushes::MASK);
  e->addItem("SMOOTH", SculptBrushes::SMOOTH);
  e->addItem("KELVINLET", SculptBrushes::KELVINLET);
  return e;
}
} // namespace litestl::binding