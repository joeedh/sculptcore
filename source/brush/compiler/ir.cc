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

static bool stmtWritesMember(const Stmt *s, const char *field)
{
  if (!s)
    return false;
  if (s->kind == StmtKind::Assign && exprIsMemberNamed(s->lvalue.get(), field))
    return true;
  for (const auto &c : s->stmts)
    if (stmtWritesMember(c.get(), field))
      return true;
  return stmtWritesMember(s->thenBranch.get(), field) ||
         stmtWritesMember(s->elseBranch.get(), field) ||
         stmtWritesMember(s->forInit.get(), field) ||
         stmtWritesMember(s->forStep.get(), field);
}

bool brushWritesMember(const Brush &brush, const char *field)
{
  for (const auto &st : brush.stages)
    if (stmtWritesMember(st.body.get(), field))
      return true;
  return false;
}

bool brushHasFaceStage(const Brush &brush)
{
  for (const auto &st : brush.stages)
    if (st.kind == StageKind::Face)
      return true;
  return false;
}

} // namespace sculptcore::brush::sbrush
