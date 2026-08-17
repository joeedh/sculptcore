#pragma once
#include "types.h"
// Every built-in kernel header plus the id-keyed factory dispatch
// (createBuiltinBrush), generated from the kernels' @tool annotations paired
// with tools.txt — so adding a brush is a kernel + a tools.txt line, never an
// edit to a dispatch switch.
#include "generated/builtin_brushes.gen.h"
#include "extra.h"

static_assert(sculptcore::brush::builtinBrushCount ==
                  sculptcore::brush::SculptBrushesBuiltinCount,
              "tools.txt and the SculptBrushes enum disagree on the built-in count");
