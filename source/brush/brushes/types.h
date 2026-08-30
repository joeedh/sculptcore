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
  // Enhance details: for_neighbor kernel that subtracts the normal-direction
  // Laplacian (unsharp) to amplify surface detail — the inverse of smooth.
  ENHANCE = 21,
  // Draw along the autodiff gradient of an imported texture field — the
  // cross-backend gate for grad() through Tex.eval (texture-scripts T2).
  TEXGRAD = 22,
};
MAKE_ENUM_CLASS(SculptBrushes, _SculptBrushes, int);

/** First id available to extra (out-of-repo) kernels; see brushes/extra.h.
 * Generated extras code references this constant, never a literal count. */
inline constexpr int SculptBrushesBuiltinCount = 23;
static_assert(int(_SculptBrushes::TEXGRAD) == SculptBrushesBuiltinCount - 1,
              "SculptBrushesBuiltinCount must track the last built-in enum item");
} // namespace sculptcore::brush

namespace litestl::binding {
template <> struct Binder<sculptcore::brush::SculptBrushes> {
  static const types::Enum *bind()
  {
    using namespace sculptcore::brush;
    types::Enum *e =
        new types::Enum("sculptcore::brush::SculptBrushes", sizeof(SculptBrushes));
    // Built-in items, in id order — generated from brushes/tools.txt, which is
    // also what the id-keyed factory dispatch is generated from, so a name can
    // never bind to one id here and dispatch as another there.
#include "generated/builtin_brushes_enum.inc"
#ifdef SCULPTCORE_EXTRA_BRUSHES
    // Extra (out-of-repo) kernels — ids follow the built-ins; see extra.h.
#include "sculptcore_extra_brushes_enum.inc"
#endif
    return e;
  }
};
} // namespace litestl::binding