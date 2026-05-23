#pragma once

#include "ir.h"
#include "litestl/util/string.h"

namespace sculptcore::brush::sbrush {

struct EmitResult {
  string text;
  Vector<string> errors;
};

EmitResult emitCpp(const Brush &brush);

} // namespace sculptcore::brush::sbrush
