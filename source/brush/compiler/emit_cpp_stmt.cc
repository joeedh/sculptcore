/** Statement lowering (emitStmt) and the literal / enum-name helpers the
 * generated source spells values with. */

#include "emit_cpp_internal.h"

namespace sculptcore::brush::sbrush::cpp_emit {

// Emit the C++ spelling of an IR type. Struct types use their
// user-defined name; everything else maps to the engine math types.
void Emit::emitTypeRef(TypeKind ty, const string &structName)
{
  if (ty == TypeKind::Struct) {
    out += structName;
  } else {
    out += typeKindName(ty);
  }
}

void Emit::emitStmt(const Stmt &s)
{
  switch (s.kind) {
  case StmtKind::Block: {
    writeIndent();
    out += "{\n";
    indent++;
    int savedLocals = (int)locals.size();
    for (const auto &c : s.stmts)
      emitStmt(*c);
    while ((int)locals.size() > savedLocals)
      locals.pop_back();
    indent--;
    writeIndent();
    out += "}\n";
    break;
  }
  case StmtKind::DeclLocal:
    writeIndent();
    if (dualBody && (s.declType == TypeKind::Float || s.declType == TypeKind::Float3)) {
      // EvalD body: float/float3 locals carry their derivative.
      out += s.declType == TypeKind::Float ? "sbdual" : "sbdual3";
      out += " ";
      out += s.name;
      if (s.expr) {
        out += " = ";
        emitDual(*s.expr);
      }
      out += ";\n";
      locals.append(LocalVar{s.name, s.declType, /*dual=*/true});
      break;
    }
    emitTypeRef(s.declType, s.declStructName);
    out += " ";
    out += s.name;
    if (s.expr) {
      out += " = ";
      emitExpr(*s.expr);
    }
    out += ";\n";
    locals.append(LocalVar{s.name, s.declType});
    break;
  case StmtKind::Assign: {
    if (!certifiedPrelude && !preparedAssignmentSafe(*s.lvalue)) {
      preparedStateUnsafe = true;
    }
    const Expr *storeTarget = s.lvalue.get();
    while (storeTarget->kind == ExprKind::Paren)
      storeTarget = storeTarget->lhs.get();
    if (extrasMode && storeTarget->kind == ExprKind::Ident) {
      stringref name(storeTarget->name.c_str());
      const Field *field = findField(name);
      if (!isLocal(name) && !isStageParam(name) && field && fieldUsesStore(*field)) {
        writeIndent();
        out += currentStage && currentStage->kind == StageKind::Host
                   ? "brush.setEvaluatedNamed"
                   : "ctx.brush.setEvaluatedNamed";
        out += storeSuffix(field->type);
        out += "(kExtraSlot_";
        out += field->name;
        out += ", ";
        const char *op = assignOpCSym(s.assignOp);
        if (op[0] != '=') {
          emitExpr(*s.lvalue);
          char symbol[4] = {' ', op[0], ' ', 0};
          out += symbol;
          out += "(";
          emitExpr(*s.rvalue);
          out += ")";
        } else {
          emitExpr(*s.rvalue);
        }
        out += ");\n";
        break;
      }
    }
    if (dualBody && s.lvalue->kind == ExprKind::Ident &&
        isDualLocal(stringref(s.lvalue->name.c_str())))
    {
      // Dual assignment. Compound ops expand to `x = x <op> (rhs)` — the
      // prelude defines only the binary operators.
      writeIndent();
      out += s.lvalue->name;
      out += " = ";
      const char *op = assignOpCSym(s.assignOp);
      if (op[0] != '=') {
        out += s.lvalue->name;
        out += " ";
        char b[2] = {op[0], 0};
        out += b;
        out += " (";
        emitDual(*s.rvalue);
        out += ")";
      } else {
        emitDual(*s.rvalue);
      }
      out += ";\n";
      break;
    }
    if (dualBody && s.lvalue->kind == ExprKind::Member && s.lvalue->lhs &&
        s.lvalue->lhs->kind == ExprKind::Ident &&
        isDualLocal(stringref(s.lvalue->lhs->name.c_str())))
    {
      // Writing one component would need Jacobian row surgery — no
      // shipped eval justifies that yet.
      errf("component assignment to dual '%s' is not differentiable",
           s.lvalue->lhs->name.c_str());
      break;
    }
    writeIndent();
    emitExpr(*s.lvalue);
    out += " ";
    out += assignOpCSym(s.assignOp);
    out += " ";
    emitExpr(*s.rvalue);
    out += ";\n";
    break;
  }
  case StmtKind::If: {
    writeIndent();
    out += "if (";
    emitExpr(*s.cond);
    out += ") ";
    if (s.thenBranch && s.thenBranch->kind == StmtKind::Block) {
      out += "{\n";
      indent++;
      int savedLocals = (int)locals.size();
      for (const auto &c : s.thenBranch->stmts)
        emitStmt(*c);
      while ((int)locals.size() > savedLocals)
        locals.pop_back();
      indent--;
      writeIndent();
      out += "}";
    } else if (s.thenBranch) {
      out += "\n";
      indent++;
      emitStmt(*s.thenBranch);
      indent--;
      writeIndent();
    }
    if (s.elseBranch) {
      out += " else ";
      if (s.elseBranch->kind == StmtKind::Block) {
        out += "{\n";
        indent++;
        int savedLocals = (int)locals.size();
        for (const auto &c : s.elseBranch->stmts)
          emitStmt(*c);
        while ((int)locals.size() > savedLocals)
          locals.pop_back();
        indent--;
        writeIndent();
        out += "}\n";
      } else if (s.elseBranch->kind == StmtKind::If) {
        // else-if chaining
        emitStmt(*s.elseBranch);
      } else {
        out += "\n";
        indent++;
        emitStmt(*s.elseBranch);
        indent--;
      }
    } else {
      out += "\n";
    }
    break;
  }
  case StmtKind::For: {
    // `for (<init> <cond>; <step>) { <body> }`. Render init and step
    // into scratch buffers so we can trim the trailing newline (and,
    // for the step, the trailing semicolon — the C-for closes with
    // `)` instead).
    writeIndent();
    out += "for (";
    auto renderFrag = [&](const Stmt &child, bool stripSemi) {
      string saved = out;
      out = string("");
      int savedIndent = indent;
      indent = 0;
      emitStmt(child);
      indent = savedIndent;
      string frag = out;
      out = saved;
      int n = (int)frag.size();
      while (n > 0 && frag[n - 1] == '\n')
        n--;
      if (stripSemi && n > 0 && frag[n - 1] == ';')
        n--;
      for (int i = 0; i < n; i++) {
        char tmp[2] = {frag[i], 0};
        out += tmp;
      }
    };
    if (s.forInit)
      renderFrag(*s.forInit, /*stripSemi=*/false);
    out += " ";
    emitExpr(*s.cond);
    out += "; ";
    if (s.forStep)
      renderFrag(*s.forStep, /*stripSemi=*/true);
    out += ") ";
    if (s.thenBranch && s.thenBranch->kind == StmtKind::Block) {
      out += "{\n";
      indent++;
      int savedLocals = (int)locals.size();
      for (const auto &c : s.thenBranch->stmts)
        emitStmt(*c);
      while ((int)locals.size() > savedLocals)
        locals.pop_back();
      indent--;
      writeIndent();
      out += "}\n";
    } else if (s.thenBranch) {
      out += "\n";
      indent++;
      emitStmt(*s.thenBranch);
      indent--;
    } else {
      out += ";\n";
    }
    break;
  }
  case StmtKind::Continue:
    writeIndent();
    out += "continue;\n";
    break;
  case StmtKind::Return:
    writeIndent();
    out += "return";
    if (s.expr) {
      out += " ";
      if (dualBody) {
        emitDual(*s.expr);
      } else {
        emitExpr(*s.expr);
      }
    }
    out += ";\n";
    break;
  case StmtKind::ExprStmt:
    writeIndent();
    emitExpr(*s.expr);
    out += ";\n";
    break;
  case StmtKind::NeighborLoop: {
    // for_neighbor (nb in <outer>) { body }
    // Enumerate <outer>'s 1-ring vertex indices through the NbrSrc policy
    // (live disk walk or cached CSR — chosen at brush-command creation),
    // binding nb as a reference-bundle with .co/.no/.v just like the main
    // vertex iter target. A real for loop (not a lambda) keeps continue/
    // break in the body working; NbrSrc monomorphizes the iteration.
    neighborLoopUsed = true;
    writeIndent();
    out += "{\n";
    indent++;
    writeIndent();
    out += "int __outer_v = ";
    emitExpr(*s.lvalue);
    out += ".v;\n";
    writeIndent();
    out += "for (int __nb_v : NbrSrc::range(ctx, __outer_v)) {\n";
    indent++;
    writeIndent();
    // Neighbor co reads via the AccumMode policy: the pre-dab Jacobi snapshot
    // (AccumLive) or the base position (AccumOrig); no comes through the
    // executor's nbrNo (the domain seam), v stays live. `co` is by value
    // because a from-base policy computes it — aggregate init does not
    // extend a temporary's lifetime through a reference member.
    out += "struct { litestl::math::float3 co; litestl::math::float3 &no; int "
           "v; } ";
    out += s.name;
    out += " {AccMode::neighborCo(ctx, __nb_v), TYPES::nbrNo(ctx, __nb_v), __nb_v};\n";
    // Body: emit either a Block (inline) or a single statement.
    int savedLocals = (int)locals.size();
    locals.append(LocalVar{s.name, TypeKind::Unknown});
    nbrBundles.append(s.name);
    if (s.thenBranch && s.thenBranch->kind == StmtKind::Block) {
      for (const auto &c : s.thenBranch->stmts)
        emitStmt(*c);
    } else if (s.thenBranch) {
      emitStmt(*s.thenBranch);
    }
    nbrBundles.pop_back();
    while ((int)locals.size() > savedLocals)
      locals.pop_back();
    indent--;
    writeIndent();
    out += "}\n";
    indent--;
    writeIndent();
    out += "}\n";
    break;
  }
  }
}

string Emit::capitalize(const string &s)
{
  string r = s;
  if (r.size() > 0)
    r[0] = (char)std::toupper((unsigned char)r[0]);
  return r;
}

string Emit::lower(const string &s)
{
  string r = s;
  for (int i = 0; i < (int)r.size(); i++) {
    r[i] = (char)std::tolower((unsigned char)r[i]);
  }
  return r;
}

// Preserve exact parsed metadata independently of float working storage.
string Emit::doubleLit(double v)
{
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  bool hasDot = false;
  for (const char *p = buf; *p; p++) {
    if (*p == '.' || *p == 'e' || *p == 'E') {
      hasDot = true;
      break;
    }
  }
  string r = buf;
  if (!hasDot)
    r += ".0";
  return r;
}

string Emit::floatLit(double v)
{
  return doubleLit(v) + "f";
}

// DSL attr type -> C++ element type. Used inside the vertex fn, which opens
// `using namespace litestl::math;`, so the bare vector names resolve.
const char *Emit::attrCppType(TypeKind t)
{
  switch (t) {
  case TypeKind::Float:
    return "float";
  case TypeKind::Float2:
    return "float2";
  case TypeKind::Float3:
    return "float3";
  case TypeKind::Float4:
    return "float4";
  case TypeKind::Int:
    return "int";
  default:
    return "float";
  }
}

// DSL attr type -> mesh::AttrType enum spelling (for the codegen manifest).
const char *Emit::attrTypeEnum(TypeKind t)
{
  switch (t) {
  case TypeKind::Float:
    return "sculptcore::mesh::AttrType::FLOAT";
  case TypeKind::Float2:
    return "sculptcore::mesh::AttrType::FLOAT2";
  case TypeKind::Float3:
    return "sculptcore::mesh::AttrType::FLOAT3";
  case TypeKind::Float4:
    return "sculptcore::mesh::AttrType::FLOAT4";
  case TypeKind::Int:
    return "sculptcore::mesh::AttrType::INT";
  case TypeKind::Bool:
    return "sculptcore::mesh::AttrType::BOOL";
  default:
    return "sculptcore::mesh::AttrType::FLOAT";
  }
}

const char *Emit::attrDomainEnum(AttrDomain d)
{
  switch (d) {
  case AttrDomain::Vertex:
    return "sculptcore::brush::AttrElemDomain::Vertex";
  case AttrDomain::Face:
    return "sculptcore::brush::AttrElemDomain::Face";
  case AttrDomain::Edge:
    return "sculptcore::brush::AttrElemDomain::Edge";
  case AttrDomain::Corner:
    return "sculptcore::brush::AttrElemDomain::Corner";
  }
  return "sculptcore::brush::AttrElemDomain::Vertex";
}

// `@use(<category>)` -> mesh::AttrUse enum spelling. Emitted as an int since
// BrushAttrManifestEntry::use is a plain int (the flags class isn't needed —
// an attr carries exactly one category).
const char *Emit::attrUseEnum(int use)
{
  switch (use) {
  case AttrUseId::Unit:
    return "int(sculptcore::mesh::AttrUse::UNIT)";
  case AttrUseId::Color:
    return "int(sculptcore::mesh::AttrUse::COLOR)";
  case AttrUseId::Uv:
    return "int(sculptcore::mesh::AttrUse::UV)";
  case AttrUseId::Polygroup:
    return "int(sculptcore::mesh::AttrUse::POLYGROUP)";
  case AttrUseId::Select:
    return "int(sculptcore::mesh::AttrUse::SELECT)";
  case AttrUseId::SculptLayer:
    return "int(sculptcore::mesh::AttrUse::SCULPT_LAYER)";
  default:
    return "0";
  }
}

} // namespace sculptcore::brush::sbrush::cpp_emit
