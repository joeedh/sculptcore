#include "lexer.h"
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace sculptcore::brush::sbrush {

using litestl::util::string;
using litestl::util::stringref;

namespace {

struct KW {
  const char *text;
  TokKind kind;
};

static const KW keywords[] = {
  {"brush",    TokKind::KwBrush},
  {"uniform",  TokKind::KwUniform},
  {"ctx",      TokKind::KwCtx},
  {"vertex",   TokKind::KwVertex},
  {"reduce",   TokKind::KwReduce},
  {"host",     TokKind::KwHost},
  {"inout",    TokKind::KwInout},
  {"in",       TokKind::KwIn},
  {"out",      TokKind::KwOut},
  {"if",       TokKind::KwIf},
  {"else",     TokKind::KwElse},
  {"return",   TokKind::KwReturn},
  {"continue", TokKind::KwContinue},
  {"true",     TokKind::KwTrue},
  {"false",    TokKind::KwFalse},
  {"for_neighbor", TokKind::KwForNeighbor},
  {"struct",   TokKind::KwStruct},
};

struct Lexer {
  const char *src;
  int len;
  int pos = 0;
  int line = 1;
  int col = 1;
  LexResult result;
  string filename;

  char peek(int off = 0) const
  {
    return (pos + off < len) ? src[pos + off] : '\0';
  }

  char advance()
  {
    char c = src[pos++];
    if (c == '\n') {
      line++;
      col = 1;
    } else {
      col++;
    }
    return c;
  }

  bool match(char c)
  {
    if (peek() == c) {
      advance();
      return true;
    }
    return false;
  }

  void err(const char *msg, int l, int c)
  {
    LexError e;
    e.message = string(msg);
    e.line = l;
    e.col = c;
    result.errors.append(e);
  }

  void skipWsAndComments()
  {
    while (pos < len) {
      char c = peek();
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        advance();
      } else if (c == '/' && peek(1) == '/') {
        while (pos < len && peek() != '\n') advance();
      } else if (c == '/' && peek(1) == '*') {
        advance(); advance();
        while (pos < len && !(peek() == '*' && peek(1) == '/')) advance();
        if (pos < len) { advance(); advance(); }
      } else {
        break;
      }
    }
  }

  void emit(Token t)
  {
    result.tokens.append(t);
  }

  void lexIdentOrKw(int startLine, int startCol)
  {
    int start = pos - 1;
    while (pos < len && (isalnum((unsigned char)peek()) || peek() == '_')) {
      advance();
    }
    int n = pos - start;
    string text;
    for (int i = 0; i < n; i++) text += src[start + i];

    Token t;
    t.line = startLine;
    t.col = startCol;
    t.text = text;

    for (const auto &k : keywords) {
      if (strlen(k.text) == (size_t)n && std::memcmp(k.text, src + start, n) == 0) {
        t.kind = k.kind;
        emit(t);
        return;
      }
    }
    t.kind = TokKind::Ident;
    emit(t);
  }

  void lexNumber(int startLine, int startCol)
  {
    int start = pos - 1;
    bool isFloat = false;
    while (pos < len && isdigit((unsigned char)peek())) advance();
    if (peek() == '.' && isdigit((unsigned char)peek(1))) {
      isFloat = true;
      advance();
      while (pos < len && isdigit((unsigned char)peek())) advance();
    }
    if (peek() == 'e' || peek() == 'E') {
      isFloat = true;
      advance();
      if (peek() == '+' || peek() == '-') advance();
      while (pos < len && isdigit((unsigned char)peek())) advance();
    }
    if (peek() == 'f' || peek() == 'F') {
      isFloat = true;
      advance();
    }

    int n = pos - start;
    string text;
    for (int i = 0; i < n; i++) text += src[start + i];

    char buf[64];
    int copyN = n;
    if (copyN >= (int)sizeof(buf)) copyN = sizeof(buf) - 1;
    std::memcpy(buf, src + start, copyN);
    buf[copyN] = 0;
    // strip trailing f for parsing
    if (copyN > 0 && (buf[copyN - 1] == 'f' || buf[copyN - 1] == 'F')) {
      buf[copyN - 1] = 0;
    }

    Token t;
    t.line = startLine;
    t.col = startCol;
    t.text = text;
    if (isFloat) {
      t.kind = TokKind::FloatLit;
      t.fvalue = strtod(buf, nullptr);
    } else {
      t.kind = TokKind::IntLit;
      t.ivalue = strtoll(buf, nullptr, 10);
    }
    emit(t);
  }

  void lexString(int startLine, int startCol)
  {
    string text;
    while (pos < len && peek() != '"') {
      char c = advance();
      if (c == '\\' && pos < len) {
        char e = advance();
        switch (e) {
        case 'n': text += '\n'; break;
        case 't': text += '\t'; break;
        case 'r': text += '\r'; break;
        case '"': text += '"'; break;
        case '\\': text += '\\'; break;
        default: text += e; break;
        }
      } else {
        text += c;
      }
    }
    if (pos >= len) {
      err("unterminated string literal", startLine, startCol);
    } else {
      advance(); // closing "
    }
    Token t;
    t.kind = TokKind::StringLit;
    t.line = startLine;
    t.col = startCol;
    t.text = text;
    emit(t);
  }

  void run()
  {
    while (pos < len) {
      skipWsAndComments();
      if (pos >= len) break;
      int sl = line, sc = col;
      char c = advance();
      if (isalpha((unsigned char)c) || c == '_') {
        lexIdentOrKw(sl, sc);
        continue;
      }
      if (isdigit((unsigned char)c)) {
        lexNumber(sl, sc);
        continue;
      }
      if (c == '"') {
        lexString(sl, sc);
        continue;
      }
      Token t;
      t.line = sl;
      t.col = sc;
      switch (c) {
      case '{': t.kind = TokKind::LBrace; break;
      case '}': t.kind = TokKind::RBrace; break;
      case '(': t.kind = TokKind::LParen; break;
      case ')': t.kind = TokKind::RParen; break;
      case '[': t.kind = TokKind::LBracket; break;
      case ']': t.kind = TokKind::RBracket; break;
      case ',': t.kind = TokKind::Comma; break;
      case ';': t.kind = TokKind::Semicolon; break;
      case '.': t.kind = TokKind::Dot; break;
      case '@': t.kind = TokKind::At; break;
      case '+': t.kind = match('=') ? TokKind::AddAssign : TokKind::Plus; break;
      case '-': t.kind = match('=') ? TokKind::SubAssign : TokKind::Minus; break;
      case '*': t.kind = match('=') ? TokKind::MulAssign : TokKind::Star; break;
      case '/': t.kind = match('=') ? TokKind::DivAssign : TokKind::Slash; break;
      case '=': t.kind = match('=') ? TokKind::Eq : TokKind::Assign; break;
      case '!': t.kind = match('=') ? TokKind::Ne : TokKind::Not; break;
      case '<': t.kind = match('=') ? TokKind::Le : TokKind::Lt; break;
      case '>': t.kind = match('=') ? TokKind::Ge : TokKind::Gt; break;
      case '&':
        if (match('&')) { t.kind = TokKind::AndAnd; }
        else { err("expected '&&'", sl, sc); continue; }
        break;
      case '|':
        if (match('|')) { t.kind = TokKind::OrOr; }
        else { err("expected '||'", sl, sc); continue; }
        break;
      default:
        {
          char buf[64];
          std::snprintf(buf, sizeof(buf), "unexpected character '%c' (0x%02x)", c, (unsigned char)c);
          err(buf, sl, sc);
          continue;
        }
      }
      emit(t);
    }
    Token eof;
    eof.kind = TokKind::Eof;
    eof.line = line;
    eof.col = col;
    emit(eof);
  }
};

} // namespace

LexResult lex(stringref source, stringref filename)
{
  Lexer lx;
  lx.src = source.c_str();
  lx.len = (int)source.size();
  lx.filename = string(filename.c_str());
  lx.run();
  return std::move(lx.result);
}

const char *tokKindName(TokKind k)
{
  switch (k) {
  case TokKind::Eof: return "eof";
  case TokKind::Ident: return "ident";
  case TokKind::IntLit: return "int-lit";
  case TokKind::FloatLit: return "float-lit";
  case TokKind::StringLit: return "string-lit";
  case TokKind::LBrace: return "{";
  case TokKind::RBrace: return "}";
  case TokKind::LParen: return "(";
  case TokKind::RParen: return ")";
  case TokKind::LBracket: return "[";
  case TokKind::RBracket: return "]";
  case TokKind::Comma: return ",";
  case TokKind::Semicolon: return ";";
  case TokKind::Dot: return ".";
  case TokKind::At: return "@";
  case TokKind::Plus: return "+";
  case TokKind::Minus: return "-";
  case TokKind::Star: return "*";
  case TokKind::Slash: return "/";
  case TokKind::Assign: return "=";
  case TokKind::Eq: return "==";
  case TokKind::Ne: return "!=";
  case TokKind::Lt: return "<";
  case TokKind::Le: return "<=";
  case TokKind::Gt: return ">";
  case TokKind::Ge: return ">=";
  case TokKind::AddAssign: return "+=";
  case TokKind::SubAssign: return "-=";
  case TokKind::MulAssign: return "*=";
  case TokKind::DivAssign: return "/=";
  case TokKind::AndAnd: return "&&";
  case TokKind::OrOr: return "||";
  case TokKind::Not: return "!";
  case TokKind::KwBrush: return "brush";
  case TokKind::KwUniform: return "uniform";
  case TokKind::KwCtx: return "ctx";
  case TokKind::KwVertex: return "vertex";
  case TokKind::KwReduce: return "reduce";
  case TokKind::KwHost: return "host";
  case TokKind::KwInout: return "inout";
  case TokKind::KwIn: return "in";
  case TokKind::KwOut: return "out";
  case TokKind::KwIf: return "if";
  case TokKind::KwElse: return "else";
  case TokKind::KwReturn: return "return";
  case TokKind::KwContinue: return "continue";
  case TokKind::KwTrue: return "true";
  case TokKind::KwFalse: return "false";
  case TokKind::KwForNeighbor: return "for_neighbor";
  case TokKind::KwStruct: return "struct";
  }
  return "?";
}

} // namespace sculptcore::brush::sbrush
