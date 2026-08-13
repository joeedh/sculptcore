#pragma once

#include "ir.h"
#include "lexer.h"
#include <memory>

namespace sculptcore::brush::sbrush {

struct ParseError {
  string message;
  int line = 0;
  int col = 0;
};

struct ParseResult {
  // Exactly one of these is set: `brush` for a .sbrush source, `unit` for a
  // .stex source (top-level `texture`/`sampler` declarations).
  std::unique_ptr<Brush> brush;
  std::unique_ptr<TextureUnit> unit;
  Vector<ParseError> errors;
};

ParseResult parse(const Vector<Token> &tokens, stringref filename);

} // namespace sculptcore::brush::sbrush
