#pragma once

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

namespace sculptcore::brush::sbrush {

using litestl::util::string;
using litestl::util::stringref;
using litestl::util::Vector;

enum class TokKind : int {
  Eof,
  Ident,
  IntLit,
  FloatLit,
  StringLit,
  // single-char punct
  LBrace, RBrace, LParen, RParen, LBracket, RBracket,
  Comma, Semicolon, Dot, At,
  // operators (multi-char handled in lexer)
  Plus, Minus, Star, Slash,
  Assign,           // =
  Eq, Ne, Lt, Le, Gt, Ge,
  AddAssign, SubAssign, MulAssign, DivAssign,
  AndAnd, OrOr, Not,
  // keywords
  KwBrush, KwUniform, KwCtx, KwVertex, KwReduce, KwHost,
  KwInout, KwIn, KwOut,
  KwIf, KwElse, KwReturn, KwContinue, KwTrue, KwFalse,
  KwForNeighbor, KwStruct, KwFor, KwTexture,
  KwAttr, KwFace, KwEdge, KwCorner,
};

const char *tokKindName(TokKind k);

struct Token {
  TokKind kind = TokKind::Eof;
  int line = 0;
  int col = 0;
  // Ident / StringLit: text. For StringLit, the surrounding quotes are stripped.
  string text;
  // IntLit / FloatLit
  long long ivalue = 0;
  double fvalue = 0.0;
};

struct LexError {
  string message;
  int line = 0;
  int col = 0;
};

struct LexResult {
  Vector<Token> tokens;
  Vector<LexError> errors;
};

LexResult lex(litestl::util::stringref source, litestl::util::stringref filename);

} // namespace sculptcore::brush::sbrush
