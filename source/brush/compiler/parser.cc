#include "parser.h"
#include <cstdio>

namespace sculptcore::brush::sbrush {

namespace {

struct Parser {
  const Vector<Token> *tokens;
  int pos = 0;
  string filename;
  Vector<ParseError> errors;
  // Set during parseBrush so deep-parser helpers can resolve struct names.
  Brush *currentBrush = nullptr;

  const Token &peek(int off = 0) const
  {
    int i = pos + off;
    if (i >= (int)tokens->size()) i = (int)tokens->size() - 1;
    return (*tokens)[i];
  }

  const Token &advance()
  {
    const Token &t = (*tokens)[pos];
    if (pos + 1 < (int)tokens->size()) pos++;
    return t;
  }

  bool check(TokKind k) const { return peek().kind == k; }

  bool match(TokKind k)
  {
    if (check(k)) { advance(); return true; }
    return false;
  }

  void error(const char *msg, const Token &t)
  {
    ParseError e;
    e.message = string(msg);
    e.line = t.line;
    e.col = t.col;
    errors.append(e);
  }

  void errorf(const Token &t, const char *fmt, const char *arg)
  {
    char buf[256];
    std::snprintf(buf, sizeof(buf), fmt, arg);
    ParseError e;
    e.message = string(buf);
    e.line = t.line;
    e.col = t.col;
    errors.append(e);
  }

  bool expect(TokKind k, const char *ctx)
  {
    if (check(k)) { advance(); return true; }
    char buf[256];
    std::snprintf(buf, sizeof(buf), "expected '%s' %s, got '%s'",
                  tokKindName(k), ctx, tokKindName(peek().kind));
    error(buf, peek());
    return false;
  }

  // === toplevel ===

  // Resolves a user-defined struct name in the current brush, or nullptr.
  const StructDef *findStruct(stringref name) const
  {
    if (!currentBrush) return nullptr;
    for (const auto &s : currentBrush->structs) {
      if (string(s.name).operator==(string(name.c_str()))) return &s;
    }
    return nullptr;
  }

  std::unique_ptr<Brush> parseBrush()
  {
    auto brush = std::make_unique<Brush>();
    brush->sourceFile = filename;
    currentBrush = brush.get();

    // optional @brush("name")
    if (match(TokKind::At)) {
      // `brush` is a keyword, so the lexer emits KwBrush here, not Ident.
      if (!match(TokKind::KwBrush)) {
        error("expected 'brush' after '@'", peek());
      }
      expect(TokKind::LParen, "after @brush");
      if (check(TokKind::StringLit)) {
        brush->attrName = peek().text;
        advance();
      } else {
        error("expected string after @brush(", peek());
      }
      expect(TokKind::RParen, "after @brush(...");
    }

    if (!expect(TokKind::KwBrush, "at start of brush declaration")) return brush;
    if (!check(TokKind::Ident)) { error("expected brush identifier", peek()); return brush; }
    brush->cppName = peek().text;
    advance();
    if (!expect(TokKind::LBrace, "after brush name")) return brush;

    while (!check(TokKind::RBrace) && !check(TokKind::Eof)) {
      if (check(TokKind::KwUniform) || check(TokKind::KwCtx)) {
        parseField(*brush);
      } else if (check(TokKind::KwStruct)) {
        parseStruct(*brush);
      } else if (check(TokKind::KwVertex) || check(TokKind::KwReduce) || check(TokKind::KwHost)) {
        parseStage(*brush);
      } else {
        errorf(peek(), "unexpected token '%s' in brush body", tokKindName(peek().kind));
        advance();
      }
    }
    expect(TokKind::RBrace, "to close brush body");
    currentBrush = nullptr;
    return brush;
  }

  void parseField(Brush &brush)
  {
    FieldKind kind;
    if (match(TokKind::KwUniform)) kind = FieldKind::Uniform;
    else { advance(); kind = FieldKind::Ctx; }

    if (!check(TokKind::Ident)) { error("expected type in field declaration", peek()); return; }
    TypeKind ty = parseTypeKind(stringref(peek().text.c_str()));
    if (ty == TypeKind::Unknown) {
      errorf(peek(), "unknown type '%s' in field declaration", peek().text.c_str());
    }
    advance();
    // Multi-var field decl: `uniform float a, b, c;`
    while (true) {
      if (!check(TokKind::Ident)) { error("expected field name", peek()); return; }
      Field f;
      f.kind = kind;
      f.type = ty;
      f.name = peek().text;
      advance();
      brush.fields.append(f);
      if (!match(TokKind::Comma)) break;
    }
    expect(TokKind::Semicolon, "after field declaration");
  }

  void parseStruct(Brush &brush)
  {
    advance(); // 'struct'
    if (!check(TokKind::Ident)) { error("expected struct name after 'struct'", peek()); return; }
    StructDef sd;
    sd.name = peek().text;
    advance();
    if (!expect(TokKind::LBrace, "after struct name")) return;
    while (!check(TokKind::RBrace) && !check(TokKind::Eof)) {
      if (!check(TokKind::Ident)) {
        errorf(peek(), "expected field type in struct, got '%s'", tokKindName(peek().kind));
        advance();
        continue;
      }
      TypeKind ty = parseTypeKind(stringref(peek().text.c_str()));
      if (ty == TypeKind::Unknown) {
        errorf(peek(), "unknown type '%s' in struct field", peek().text.c_str());
      }
      advance();
      while (true) {
        if (!check(TokKind::Ident)) { error("expected struct field name", peek()); break; }
        StructField sf;
        sf.type = ty;
        sf.name = peek().text;
        advance();
        sd.fields.append(sf);
        if (!match(TokKind::Comma)) break;
      }
      expect(TokKind::Semicolon, "after struct field declaration");
    }
    expect(TokKind::RBrace, "to close struct body");
    brush.structs.append(std::move(sd));
  }

  void parseStage(Brush &brush)
  {
    Stage st;
    if (match(TokKind::KwVertex)) st.kind = StageKind::Vertex;
    else if (match(TokKind::KwReduce)) st.kind = StageKind::Reduce;
    else { advance(); st.kind = StageKind::Host; }

    if (!check(TokKind::Ident)) { error("expected return type in stage declaration", peek()); return; }
    st.returnType = parseTypeKind(stringref(peek().text.c_str()));
    if (st.returnType == TypeKind::Unknown) {
      errorf(peek(), "unknown return type '%s' in stage declaration", peek().text.c_str());
    }
    advance();

    if (!check(TokKind::Ident)) { error("expected stage name", peek()); return; }
    st.name = peek().text;
    advance();

    expect(TokKind::LParen, "after stage name");
    while (!check(TokKind::RParen) && !check(TokKind::Eof)) {
      Param p;
      if (match(TokKind::KwInout)) p.dir = ParamDir::InOut;
      else if (match(TokKind::KwIn)) p.dir = ParamDir::In;
      else if (match(TokKind::KwOut)) p.dir = ParamDir::Out;

      if (!check(TokKind::Ident)) { error("expected param type", peek()); break; }
      p.type = parseTypeKind(stringref(peek().text.c_str()));
      if (p.type == TypeKind::Unknown) {
        // Maybe a user-defined struct from the current brush.
        if (const StructDef *sd = findStruct(stringref(peek().text.c_str()))) {
          p.type = TypeKind::Struct;
          p.structName = sd->name;
        } else {
          errorf(peek(), "unknown param type '%s'", peek().text.c_str());
        }
      }
      advance();
      if (!check(TokKind::Ident)) { error("expected param name", peek()); break; }
      p.name = peek().text;
      advance();
      st.params.append(p);
      if (!check(TokKind::RParen)) {
        if (!expect(TokKind::Comma, "between params")) break;
      }
    }
    expect(TokKind::RParen, "to close stage params");
    st.body = parseBlock();
    brush.stages.append(std::move(st));
  }

  // === statements ===

  StmtPtr parseBlock()
  {
    auto block = std::make_unique<Stmt>(StmtKind::Block);
    block->line = peek().line;
    if (!expect(TokKind::LBrace, "to open block")) return block;
    while (!check(TokKind::RBrace) && !check(TokKind::Eof)) {
      auto s = parseStmt();
      if (s) block->stmts.append(std::move(s));
    }
    expect(TokKind::RBrace, "to close block");
    return block;
  }

  StmtPtr parseStmt()
  {
    if (check(TokKind::LBrace)) return parseBlock();
    if (check(TokKind::KwIf)) return parseIf();
    if (check(TokKind::KwForNeighbor)) return parseNeighborLoop();
    if (check(TokKind::KwReturn)) {
      auto s = std::make_unique<Stmt>(StmtKind::Return);
      s->line = peek().line;
      advance();
      if (!check(TokKind::Semicolon)) s->expr = parseExpr();
      expect(TokKind::Semicolon, "after return");
      return s;
    }
    if (check(TokKind::KwContinue)) {
      auto s = std::make_unique<Stmt>(StmtKind::Continue);
      s->line = peek().line;
      advance();
      expect(TokKind::Semicolon, "after continue");
      return s;
    }
    // declaration: <type-ident> <name> [= expr] ;
    if (check(TokKind::Ident)) {
      TypeKind t = parseTypeKind(stringref(peek().text.c_str()));
      const StructDef *sd = nullptr;
      if (t == TypeKind::Unknown) {
        sd = findStruct(stringref(peek().text.c_str()));
        if (sd) t = TypeKind::Struct;
      }
      if (t != TypeKind::Unknown && peek(1).kind == TokKind::Ident) {
        auto s = std::make_unique<Stmt>(StmtKind::DeclLocal);
        s->line = peek().line;
        s->declType = t;
        if (sd) s->declStructName = sd->name;
        advance(); // type
        s->name = peek().text;
        advance(); // name
        if (match(TokKind::Assign)) s->expr = parseExpr();
        expect(TokKind::Semicolon, "after local declaration");
        return s;
      }
    }
    // assignment or expr stmt
    auto lhs = parseExpr();
    if (!lhs) return nullptr;
    AssignOp op;
    bool isAssign = true;
    if (match(TokKind::Assign)) op = AssignOp::Assign;
    else if (match(TokKind::AddAssign)) op = AssignOp::AddAssign;
    else if (match(TokKind::SubAssign)) op = AssignOp::SubAssign;
    else if (match(TokKind::MulAssign)) op = AssignOp::MulAssign;
    else if (match(TokKind::DivAssign)) op = AssignOp::DivAssign;
    else isAssign = false;

    if (isAssign) {
      auto s = std::make_unique<Stmt>(StmtKind::Assign);
      s->line = lhs->line;
      s->assignOp = op;
      s->lvalue = std::move(lhs);
      s->rvalue = parseExpr();
      expect(TokKind::Semicolon, "after assignment");
      return s;
    }
    auto s = std::make_unique<Stmt>(StmtKind::ExprStmt);
    s->line = lhs->line;
    s->expr = std::move(lhs);
    expect(TokKind::Semicolon, "after expression statement");
    return s;
  }

  StmtPtr parseIf()
  {
    auto s = std::make_unique<Stmt>(StmtKind::If);
    s->line = peek().line;
    advance(); // if
    expect(TokKind::LParen, "after 'if'");
    s->cond = parseExpr();
    expect(TokKind::RParen, "after if-condition");
    s->thenBranch = parseStmt();
    if (match(TokKind::KwElse)) s->elseBranch = parseStmt();
    return s;
  }

  StmtPtr parseNeighborLoop()
  {
    // for_neighbor (<innerName> in <outer>) <body>
    auto s = std::make_unique<Stmt>(StmtKind::NeighborLoop);
    s->line = peek().line;
    advance(); // for_neighbor
    expect(TokKind::LParen, "after 'for_neighbor'");
    if (!check(TokKind::Ident)) {
      error("expected inner name after 'for_neighbor ('", peek());
      return s;
    }
    s->name = peek().text;
    advance();
    expect(TokKind::KwIn, "after inner name");
    s->lvalue = parseExpr();
    expect(TokKind::RParen, "after for_neighbor outer expression");
    s->thenBranch = parseStmt();
    return s;
  }

  // === expressions (precedence-climbing) ===

  ExprPtr parseExpr() { return parseOr(); }

  ExprPtr parseOr()
  {
    auto lhs = parseAnd();
    while (check(TokKind::OrOr)) {
      int line = peek().line;
      advance();
      auto rhs = parseAnd();
      auto e = std::make_unique<Expr>(ExprKind::Binary);
      e->line = line;
      e->binop = BinOp::Or;
      e->lhs = std::move(lhs);
      e->rhs = std::move(rhs);
      lhs = std::move(e);
    }
    return lhs;
  }

  ExprPtr parseAnd()
  {
    auto lhs = parseEquality();
    while (check(TokKind::AndAnd)) {
      int line = peek().line;
      advance();
      auto rhs = parseEquality();
      auto e = std::make_unique<Expr>(ExprKind::Binary);
      e->line = line;
      e->binop = BinOp::And;
      e->lhs = std::move(lhs);
      e->rhs = std::move(rhs);
      lhs = std::move(e);
    }
    return lhs;
  }

  ExprPtr parseEquality()
  {
    auto lhs = parseRel();
    while (check(TokKind::Eq) || check(TokKind::Ne)) {
      BinOp op = check(TokKind::Eq) ? BinOp::Eq : BinOp::Ne;
      int line = peek().line;
      advance();
      auto rhs = parseRel();
      auto e = std::make_unique<Expr>(ExprKind::Binary);
      e->line = line;
      e->binop = op;
      e->lhs = std::move(lhs);
      e->rhs = std::move(rhs);
      lhs = std::move(e);
    }
    return lhs;
  }

  ExprPtr parseRel()
  {
    auto lhs = parseAdd();
    while (check(TokKind::Lt) || check(TokKind::Le) || check(TokKind::Gt) || check(TokKind::Ge)) {
      BinOp op;
      if (check(TokKind::Lt)) op = BinOp::Lt;
      else if (check(TokKind::Le)) op = BinOp::Le;
      else if (check(TokKind::Gt)) op = BinOp::Gt;
      else op = BinOp::Ge;
      int line = peek().line;
      advance();
      auto rhs = parseAdd();
      auto e = std::make_unique<Expr>(ExprKind::Binary);
      e->line = line;
      e->binop = op;
      e->lhs = std::move(lhs);
      e->rhs = std::move(rhs);
      lhs = std::move(e);
    }
    return lhs;
  }

  ExprPtr parseAdd()
  {
    auto lhs = parseMul();
    while (check(TokKind::Plus) || check(TokKind::Minus)) {
      BinOp op = check(TokKind::Plus) ? BinOp::Add : BinOp::Sub;
      int line = peek().line;
      advance();
      auto rhs = parseMul();
      auto e = std::make_unique<Expr>(ExprKind::Binary);
      e->line = line;
      e->binop = op;
      e->lhs = std::move(lhs);
      e->rhs = std::move(rhs);
      lhs = std::move(e);
    }
    return lhs;
  }

  ExprPtr parseMul()
  {
    auto lhs = parseUnary();
    while (check(TokKind::Star) || check(TokKind::Slash)) {
      BinOp op = check(TokKind::Star) ? BinOp::Mul : BinOp::Div;
      int line = peek().line;
      advance();
      auto rhs = parseUnary();
      auto e = std::make_unique<Expr>(ExprKind::Binary);
      e->line = line;
      e->binop = op;
      e->lhs = std::move(lhs);
      e->rhs = std::move(rhs);
      lhs = std::move(e);
    }
    return lhs;
  }

  ExprPtr parseUnary()
  {
    if (check(TokKind::Minus) || check(TokKind::Not)) {
      UnaryOp op = check(TokKind::Minus) ? UnaryOp::Neg : UnaryOp::Not;
      int line = peek().line;
      advance();
      auto rhs = parseUnary();
      auto e = std::make_unique<Expr>(ExprKind::Unary);
      e->line = line;
      e->unaryop = op;
      e->lhs = std::move(rhs);
      return e;
    }
    return parsePostfix();
  }

  ExprPtr parsePostfix()
  {
    auto e = parsePrimary();
    while (true) {
      if (check(TokKind::Dot)) {
        int line = peek().line;
        advance();
        if (!check(TokKind::Ident)) { error("expected member name after '.'", peek()); break; }
        auto m = std::make_unique<Expr>(ExprKind::Member);
        m->line = line;
        m->name = peek().text;
        m->lhs = std::move(e);
        advance();
        e = std::move(m);
      } else if (check(TokKind::LParen)) {
        if (!e || e->kind != ExprKind::Ident) {
          error("call requires an identifier", peek());
          break;
        }
        int line = peek().line;
        advance();
        auto call = std::make_unique<Expr>(ExprKind::Call);
        call->line = line;
        call->name = e->name;
        e.reset();
        while (!check(TokKind::RParen) && !check(TokKind::Eof)) {
          call->args.append(parseExpr());
          if (!check(TokKind::RParen)) {
            if (!expect(TokKind::Comma, "between call args")) break;
          }
        }
        expect(TokKind::RParen, "to close call");
        e = std::move(call);
      } else {
        break;
      }
    }
    return e;
  }

  ExprPtr parsePrimary()
  {
    const Token &t = peek();
    if (t.kind == TokKind::FloatLit) {
      auto e = std::make_unique<Expr>(ExprKind::LitFloat);
      e->line = t.line;
      e->fvalue = t.fvalue;
      e->type = TypeKind::Float;
      advance();
      return e;
    }
    if (t.kind == TokKind::IntLit) {
      auto e = std::make_unique<Expr>(ExprKind::LitInt);
      e->line = t.line;
      e->ivalue = t.ivalue;
      e->type = TypeKind::Int;
      advance();
      return e;
    }
    if (t.kind == TokKind::KwTrue || t.kind == TokKind::KwFalse) {
      auto e = std::make_unique<Expr>(ExprKind::LitBool);
      e->line = t.line;
      e->bvalue = (t.kind == TokKind::KwTrue);
      e->type = TypeKind::Bool;
      advance();
      return e;
    }
    if (t.kind == TokKind::Ident) {
      auto e = std::make_unique<Expr>(ExprKind::Ident);
      e->line = t.line;
      e->name = t.text;
      advance();
      return e;
    }
    if (t.kind == TokKind::LParen) {
      advance();
      auto e = parseExpr();
      expect(TokKind::RParen, "to close parens");
      auto p = std::make_unique<Expr>(ExprKind::Paren);
      p->line = t.line;
      p->lhs = std::move(e);
      return p;
    }
    errorf(t, "expected primary expression, got '%s'", tokKindName(t.kind));
    advance();
    return nullptr;
  }
};

} // namespace

ParseResult parse(const Vector<Token> &tokens, stringref filename)
{
  Parser p;
  p.tokens = &tokens;
  p.filename = string(filename.c_str());
  ParseResult r;
  r.brush = p.parseBrush();
  r.errors = std::move(p.errors);
  return r;
}

} // namespace sculptcore::brush::sbrush
