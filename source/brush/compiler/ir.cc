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
  case TypeKind::Struct:  return "<struct>";
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
  return TypeKind::Unknown;
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

} // namespace sculptcore::brush::sbrush
