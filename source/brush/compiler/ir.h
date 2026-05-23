#pragma once

#include "litestl/util/string.h"
#include "litestl/util/vector.h"
#include <memory>

// sbrush IR — typed AST shared across all backend emitters.
//
// Wave 1 scope: enough to express draw.sbrush body. The IR is a tree,
// not strict SSA — backends that need SSA can build it from this. The
// emit_cpp backend lowers directly from this tree.

namespace sculptcore::brush::sbrush {

using litestl::util::string;
using litestl::util::stringref;
using litestl::util::Vector;

enum class TypeKind : int {
  Void,
  Bool,
  Int,
  Float,
  Float2,
  Float3,
  Float4,
  Vertex, // special: parameter type of vertex stage
  Unknown,
};

const char *typeKindName(TypeKind k);
TypeKind parseTypeKind(stringref name);

enum class BinOp : int {
  Add, Sub, Mul, Div,
  Eq, Ne, Lt, Le, Gt, Ge,
  And, Or,
};

enum class UnaryOp : int {
  Neg, Not,
};

enum class AssignOp : int {
  Assign, AddAssign, SubAssign, MulAssign, DivAssign,
};

const char *binOpCSym(BinOp op);
const char *unaryOpCSym(UnaryOp op);
const char *assignOpCSym(AssignOp op);

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

enum class ExprKind : int {
  LitFloat, LitInt, LitBool,
  Ident,
  Member,   // base.field
  Binary,
  Unary,
  Call,
  Paren,
};

struct Expr {
  ExprKind kind;
  TypeKind type = TypeKind::Unknown;
  int line = 0;

  // LitFloat
  double fvalue = 0.0;
  // LitInt
  long long ivalue = 0;
  // LitBool
  bool bvalue = false;
  // Ident, Member.field, Call.name
  string name;
  // Binary
  BinOp binop = BinOp::Add;
  // Unary
  UnaryOp unaryop = UnaryOp::Neg;
  // Binary/Unary/Member/Paren
  ExprPtr lhs;
  ExprPtr rhs;
  // Call
  Vector<ExprPtr> args;

  Expr() = default;
  Expr(ExprKind k) : kind(k) {}
};

enum class StmtKind : int {
  Block,
  DeclLocal,    // <type> <name> = <init>;
  Assign,       // <lvalue> [op]= <rvalue>;
  If,           // if (<cond>) <then> [else <else>]
  Continue,
  Return,
  ExprStmt,
};

struct Stmt {
  StmtKind kind;
  int line = 0;

  // Block
  Vector<StmtPtr> stmts;
  // DeclLocal
  TypeKind declType = TypeKind::Unknown;
  string name;
  // DeclLocal.init / ExprStmt.expr / Return.value
  ExprPtr expr;
  // Assign
  AssignOp assignOp = AssignOp::Assign;
  ExprPtr lvalue;
  ExprPtr rvalue;
  // If
  ExprPtr cond;
  StmtPtr thenBranch;
  StmtPtr elseBranch;

  Stmt() = default;
  Stmt(StmtKind k) : kind(k) {}
};

enum class StageKind : int {
  Vertex,
  Reduce,  // Wave 4
  Host,    // Wave 4
};

struct Param {
  string name;
  TypeKind type = TypeKind::Unknown;
  bool inOut = false; // `inout` qualifier
};

struct Stage {
  StageKind kind;
  string name;          // e.g. "apply"
  TypeKind returnType = TypeKind::Void;
  Vector<Param> params;
  StmtPtr body;
};

enum class FieldKind : int {
  Uniform,
  Ctx,
};

struct Field {
  FieldKind kind;
  TypeKind type;
  string name;
};

struct Brush {
  string attrName;       // from @brush("draw") -> "draw"
  string cppName;        // brush Draw { ... } -> "Draw"
  Vector<Field> fields;
  Vector<Stage> stages;
  string sourceFile;
};

} // namespace sculptcore::brush::sbrush
