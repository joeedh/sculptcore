#pragma once
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
  POSE = 8,
  TEXDRAW = 9,
  // Clay-family plane brushes. CLAY/SCRAPE/FILL all run the `plane` kernel
  // (the bridge sets planeoff/planeSide per tool); WINGSCRAPE runs its own.
  SCRAPE = 10,
  FILL = 11,
  WINGSCRAPE = 12,
  COLOR = 13,
  POLYGROUP = 14,
  BSMOOTH = 15,
  GRAB = 16,
  SNAKEHOOK = 17,
  COLORSMOOTH = 18,
  // Feature-align smooth (topology rake): a weighted Laplacian smooth biased to
  // a per-vertex cross field seeded from boundary features + curvature.
  FEATURE_ALIGN = 19,
  // Draw into the bound sculpt-layer delta attribute instead of positions
  // (the displace compositor folds it into evaluated v.co post-dab).
  LAYERDRAW = 20,
};
MAKE_ENUM_CLASS(SculptBrushes, _SculptBrushes, int);
} // namespace sculptcore::brush

namespace litestl::binding {
template <> struct Binder<sculptcore::brush::SculptBrushes> {
  static const types::Enum *bind()
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
    e->addItem("POSE", SculptBrushes::POSE);
    e->addItem("TEXDRAW", SculptBrushes::TEXDRAW);
    e->addItem("SCRAPE", SculptBrushes::SCRAPE);
    e->addItem("FILL", SculptBrushes::FILL);
    e->addItem("WINGSCRAPE", SculptBrushes::WINGSCRAPE);
    e->addItem("COLOR", SculptBrushes::COLOR);
    e->addItem("POLYGROUP", SculptBrushes::POLYGROUP);
    e->addItem("BSMOOTH", SculptBrushes::BSMOOTH);
    e->addItem("GRAB", SculptBrushes::GRAB);
    e->addItem("SNAKEHOOK", SculptBrushes::SNAKEHOOK);
    e->addItem("COLORSMOOTH", SculptBrushes::COLORSMOOTH);
    e->addItem("FEATURE_ALIGN", SculptBrushes::FEATURE_ALIGN);
    e->addItem("LAYERDRAW", SculptBrushes::LAYERDRAW);
    return e;
  }
};
} // namespace litestl::binding