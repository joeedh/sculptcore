#pragma once

#include "ir.h"
#include "props/prop_struct.h"
#include <cmath>

namespace sculptcore::brush::sbrush {

// These certificates prove shared-state preservation and definite initialization,
// not finite output from arbitrary kernel arithmetic. Unknown syntax fails closed.
class PreparedPreludeProof {
  const Brush &brush_;

  static const Expr *unparen(const Expr *expr)
  {
    while (expr && expr->kind == ExprKind::Paren)
      expr = expr->lhs.get();
    return expr;
  }

  const Field *field(const string &name) const
  {
    for (const auto &f : brush_.fields)
      if (f.name == name)
        return &f;
    return nullptr;
  }

  static bool contains(const Vector<string> &names, const string &name)
  {
    for (const auto &n : names)
      if (n == name)
        return true;
    return false;
  }

  static bool literal(const Expr *expr, double &value)
  {
    expr = unparen(expr);
    if (!expr)
      return false;
    if (expr->kind == ExprKind::LitFloat) {
      value = expr->fvalue;
      return std::isfinite(value) && std::isfinite(float(value));
    }
    if (expr->kind == ExprKind::Unary && expr->unaryop == UnaryOp::Neg &&
        literal(expr->lhs.get(), value))
    {
      value = -value;
      return true;
    }
    return false;
  }

  bool hostStatement(const Stmt &stmt) const
  {
    if (stmt.kind == StmtKind::Block) {
      for (const auto &child : stmt.stmts)
        if (!hostStatement(*child))
          return false;
      return true;
    }
    if (stmt.kind != StmtKind::If || stmt.elseBranch || !stmt.thenBranch)
      return false;
    const Expr *cond = unparen(stmt.cond.get());
    if (!cond || cond->kind != ExprKind::Binary ||
        (cond->binop != BinOp::Lt && cond->binop != BinOp::Gt))
      return false;
    const Expr *name = unparen(cond->lhs.get());
    if (!name || name->kind != ExprKind::Ident)
      return false;
    const Field *f = field(name->name);
    if (!f || f->kind != FieldKind::Uniform || f->type != TypeKind::Float || !f->hasRange)
      return false;
    const Stmt *assignment = stmt.thenBranch.get();
    while (assignment->kind == StmtKind::Block && assignment->stmts.size() == 1)
      assignment = assignment->stmts[0].get();
    if (assignment->kind != StmtKind::Assign || assignment->assignOp != AssignOp::Assign)
      return false;
    const Expr *target = unparen(assignment->lvalue.get());
    double threshold, assigned;
    if (!target || target->kind != ExprKind::Ident || target->name != f->name ||
        !literal(cond->rhs.get(), threshold) ||
        !literal(assignment->rvalue.get(), assigned) || threshold != assigned ||
        threshold != (cond->binop == BinOp::Lt ? f->rangeMin : f->rangeMax))
      return false;
    props::ScalarDeclaration declaration{f->name,
                                         props::Prop::FLOAT32,
                                         f->hasDefault,
                                         f->defaultValue,
                                         true,
                                         f->rangeMin,
                                         f->rangeMax,
                                         false};
    props::ScalarDomain domain;
    if (props::normalizeScalarDeclaration(declaration, domain) !=
        props::PropError::ERROR_NONE)
      return false;
    // emitExpr writes 17 significant decimal digits followed by 'f'; the
    // comparison therefore uses the float rounding of this parsed double.
    return cond->binop == BinOp::Lt ? domain.min >= double(float(threshold))
                                    : domain.max <= double(float(threshold));
  }

  bool floatExpression(const Expr *expr, const Vector<string> &initialized) const
  {
    expr = unparen(expr);
    if (!expr)
      return false;
    if (expr->kind == ExprKind::LitFloat)
      return std::isfinite(expr->fvalue) && std::isfinite(float(expr->fvalue));
    if (expr->kind == ExprKind::Ident) {
      if (contains(initialized, expr->name))
        return true;
      const Field *f = field(expr->name);
      return f && f->kind == FieldKind::Uniform && f->type == TypeKind::Float;
    }
    if (expr->kind == ExprKind::Unary)
      return expr->unaryop == UnaryOp::Neg &&
             floatExpression(expr->lhs.get(), initialized);
    if (expr->kind == ExprKind::Binary)
      return (expr->binop == BinOp::Add || expr->binop == BinOp::Sub ||
              expr->binop == BinOp::Mul || expr->binop == BinOp::Div) &&
             floatExpression(expr->lhs.get(), initialized) &&
             floatExpression(expr->rhs.get(), initialized);
    return false;
  }

public:
  explicit PreparedPreludeProof(const Brush &brush) : brush_(brush)
  {
  }

  bool host(const Stage &stage) const
  {
    return stage.kind == StageKind::Host && stage.returnType == TypeKind::Void &&
           stage.params.size() == 0 && stage.body && hostStatement(*stage.body);
  }

  bool reduce(const Stage &stage) const
  {
    if (stage.kind != StageKind::Reduce || stage.returnType != TypeKind::Void ||
        !stage.body || stage.body->kind != StmtKind::Block)
      return false;
    Vector<string> writable, initialized;
    for (const auto &param : stage.params) {
      if (param.type != TypeKind::Float || param.dir != ParamDir::Out ||
          field(param.name) || contains(writable, param.name))
        return false;
      writable.append(param.name);
    }
    for (const auto &stmt : stage.body->stmts) {
      string target;
      if (stmt->kind == StmtKind::DeclLocal) {
        if (stmt->declType != TypeKind::Float || field(stmt->name) ||
            contains(writable, stmt->name) ||
            !floatExpression(stmt->expr.get(), initialized))
          return false;
        target = stmt->name;
        writable.append(target);
      } else if (stmt->kind == StmtKind::Assign && stmt->assignOp == AssignOp::Assign) {
        const Expr *lvalue = unparen(stmt->lvalue.get());
        if (!lvalue || lvalue->kind != ExprKind::Ident ||
            !contains(writable, lvalue->name) ||
            !floatExpression(stmt->rvalue.get(), initialized))
          return false;
        target = lvalue->name;
      } else {
        return false;
      }
      if (!contains(initialized, target))
        initialized.append(target);
    }
    for (const auto &param : stage.params)
      if (!contains(initialized, param.name))
        return false;
    return true;
  }

  bool reduceCalls(const Stage *vertex) const
  {
    if (!vertex)
      return false;
    Vector<string> inputs, produced;
    for (size_t i = 1; i < vertex->params.size(); i++) {
      const auto &p = vertex->params[i];
      if (p.type != TypeKind::Float || p.dir != ParamDir::In || field(p.name) ||
          contains(inputs, p.name) || p.name == vertex->params[0].name)
        return false;
      inputs.append(p.name);
    }
    for (const auto &stage : brush_.stages) {
      if (stage.kind != StageKind::Reduce)
        continue;
      if (!reduce(stage))
        return false;
      for (const auto &p : stage.params) {
        if (!contains(inputs, p.name))
          return false;
        if (!contains(produced, p.name))
          produced.append(p.name);
      }
    }
    for (const auto &name : inputs)
      if (!contains(produced, name))
        return false;
    return true;
  }
};

} // namespace sculptcore::brush::sbrush
