#include "ir.h"
#include <cstring>

namespace sculptcore::brush::sbrush {

const char *typeKindName(TypeKind k)
{
  switch (k) {
  case TypeKind::Void:    return "void";
  case TypeKind::Bool:    return "bool";
  case TypeKind::Int:     return "int";
  case TypeKind::Float:   return "float";
  case TypeKind::Float2:  return "float2";
  case TypeKind::Float3:  return "float3";
  case TypeKind::Float4:  return "float4";
  case TypeKind::Vertex:  return "Vertex";
  case TypeKind::Face:    return "Face";
  case TypeKind::Struct:  return "<struct>";
  case TypeKind::Array:   return "<array>";
  case TypeKind::Unknown: return "<unknown>";
  }
  return "<bad>";
}

TypeKind parseTypeKind(litestl::util::stringref name)
{
  const char *s = name.c_str();
  if (std::strcmp(s, "void")   == 0) return TypeKind::Void;
  if (std::strcmp(s, "bool")   == 0) return TypeKind::Bool;
  if (std::strcmp(s, "int")    == 0) return TypeKind::Int;
  if (std::strcmp(s, "float")  == 0) return TypeKind::Float;
  if (std::strcmp(s, "float2") == 0) return TypeKind::Float2;
  if (std::strcmp(s, "float3") == 0) return TypeKind::Float3;
  if (std::strcmp(s, "float4") == 0) return TypeKind::Float4;
  if (std::strcmp(s, "Vertex") == 0) return TypeKind::Vertex;
  if (std::strcmp(s, "Face")   == 0) return TypeKind::Face;
  return TypeKind::Unknown;
}

// `@use(<name>)` attr categories. These are compiler-internal ids that name a
// mesh::_AttrUse bit (mesh/attribute_enums.h); the emitters write the symbolic
// enumerator, so the runtime value always comes from that header.
int parseAttrUse(litestl::util::stringref name)
{
  const char *s = name.c_str();
  if (std::strcmp(s, "unit")         == 0) return AttrUseId::Unit;
  if (std::strcmp(s, "color")        == 0) return AttrUseId::Color;
  if (std::strcmp(s, "uv")           == 0) return AttrUseId::Uv;
  if (std::strcmp(s, "polygroup")    == 0) return AttrUseId::Polygroup;
  if (std::strcmp(s, "select")       == 0) return AttrUseId::Select;
  if (std::strcmp(s, "sculpt_layer") == 0) return AttrUseId::SculptLayer;
  return AttrUseId::None;
}

const char *binOpCSym(BinOp op)
{
  switch (op) {
  case BinOp::Add: return "+";
  case BinOp::Sub: return "-";
  case BinOp::Mul: return "*";
  case BinOp::Div: return "/";
  case BinOp::Eq:  return "==";
  case BinOp::Ne:  return "!=";
  case BinOp::Lt:  return "<";
  case BinOp::Le:  return "<=";
  case BinOp::Gt:  return ">";
  case BinOp::Ge:  return ">=";
  case BinOp::And: return "&&";
  case BinOp::Or:  return "||";
  case BinOp::BitAnd: return "&";
  case BinOp::BitOr:  return "|";
  case BinOp::BitXor: return "^";
  }
  return "?";
}

const char *unaryOpCSym(UnaryOp op)
{
  switch (op) {
  case UnaryOp::Neg: return "-";
  case UnaryOp::Not: return "!";
  }
  return "?";
}

const char *assignOpCSym(AssignOp op)
{
  switch (op) {
  case AssignOp::Assign:    return "=";
  case AssignOp::AddAssign: return "+=";
  case AssignOp::SubAssign: return "-=";
  case AssignOp::MulAssign: return "*=";
  case AssignOp::DivAssign: return "/=";
  }
  return "?";
}

static bool stmtUsesNeighborLoop(const Stmt *s)
{
  if (!s)
    return false;
  if (s->kind == StmtKind::NeighborLoop)
    return true;
  for (const auto &c : s->stmts)
    if (stmtUsesNeighborLoop(c.get()))
      return true;
  return stmtUsesNeighborLoop(s->thenBranch.get()) ||
         stmtUsesNeighborLoop(s->elseBranch.get()) ||
         stmtUsesNeighborLoop(s->forInit.get()) || stmtUsesNeighborLoop(s->forStep.get());
}

bool brushUsesNeighborLoop(const Brush &brush)
{
  for (const auto &st : brush.stages)
    if (stmtUsesNeighborLoop(st.body.get()))
      return true;
  return false;
}

static bool exprIsMemberNamed(const Expr *e, const char *field)
{
  return e && e->kind == ExprKind::Member && e->name.operator==(string(field));
}

static bool exprContainsMemberNamed(const Expr *e, const char *field)
{
  if (!e)
    return false;
  if (exprIsMemberNamed(e, field))
    return true;
  if (exprContainsMemberNamed(e->lhs.get(), field) ||
      exprContainsMemberNamed(e->rhs.get(), field))
    return true;
  for (const auto &a : e->args)
    if (exprContainsMemberNamed(a.get(), field))
      return true;
  return false;
}

// Nested is sticky and beats TopLevel: it means the scan cannot classify this
// kernel, which the caller must surface rather than average away.
static void stmtScanMemberWrites(const Stmt *s, const char *field, MemberWriteKind &out)
{
  if (!s)
    return;
  if (s->kind == StmtKind::Assign) {
    const Expr *lv = s->lvalue.get();
    if (exprIsMemberNamed(lv, field)) {
      if (out == MemberWriteKind::None)
        out = MemberWriteKind::TopLevel;
    }
    else if (exprContainsMemberNamed(lv, field)) {
      out = MemberWriteKind::Nested;
    }
  }
  for (const auto &c : s->stmts)
    stmtScanMemberWrites(c.get(), field, out);
  stmtScanMemberWrites(s->thenBranch.get(), field, out);
  stmtScanMemberWrites(s->elseBranch.get(), field, out);
  stmtScanMemberWrites(s->forInit.get(), field, out);
  stmtScanMemberWrites(s->forStep.get(), field, out);
}

MemberWriteKind brushScanMemberWrites(const Brush &brush, const char *field)
{
  MemberWriteKind kind = MemberWriteKind::None;
  for (const auto &st : brush.stages)
    stmtScanMemberWrites(st.body.get(), field, kind);
  return kind;
}

bool brushWritesMember(const Brush &brush, const char *field)
{
  return brushScanMemberWrites(brush, field) != MemberWriteKind::None;
}

bool brushHasFaceStage(const Brush &brush)
{
  for (const auto &st : brush.stages)
    if (st.kind == StageKind::Face)
      return true;
  return false;
}

} // namespace sculptcore::brush::sbrush
