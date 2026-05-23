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
  std::unique_ptr<Brush> brush;
  Vector<ParseError> errors;
};

ParseResult parse(const Vector<Token> &tokens, stringref filename);

} // namespace sculptcore::brush::sbrush
