#include "emit_opencl.h"
#include "../kernels/ir/intrinsics.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace sculptcore::brush::sbrush {

using litestl::util::string;
using litestl::util::stringref;
using litestl::util::Vector;

namespace {

// OpenCL C has native float2/3/4 + vector math, so types map directly.
// Unknown lowers to float to keep the gate moving (typed parsing rejects
// truly bogus input upstream).
const char *clType(TypeKind k)
{
  switch (k) {
  case TypeKind::Void: return "void";
  case TypeKind::Bool: return "bool";
  case TypeKind::Int: return "int";
  case TypeKind::Float: return "float";
  case TypeKind::Float2: return "float2";
  case TypeKind::Float3: return "float3";
  case TypeKind::Float4: return "float4";
  default: return "float";
  }
}

bool hasNeighborLoop(const Stmt *s)
{
  if (!s) return false;
  if (s->kind == StmtKind::NeighborLoop) return true;
  for (const auto &c : s->stmts) {
    if (hasNeighborLoop(c.get())) return true;
  }
  if (hasNeighborLoop(s->thenBranch.get())) return true;
  if (hasNeighborLoop(s->elseBranch.get())) return true;
  return false;
}

struct Emit {
  const Brush *brush;
  const Stage *vertexStage = nullptr;
  string vertexParamName;  // e.g. "v"

  const Stage *currentStage = nullptr;

  string out;
  Vector<string> errors;
  int indent = 0;

  struct LocalVar {
    string name;
    // Lowered to sbdual/sbdual3 inside a texture EvalD body (dualBody).
    bool dual = false;
  };
  Vector<LocalVar> locals;
  // out/inout reduce params lower to pointers; an identifier ref derefs.
  Vector<string> ptrParams;

  // Set while emitting a texture eval body — gates texture-calls-texture.
  const TextureDef *currentTexture = nullptr;

  bool usesNeighbors = false;

  bool gradUsed = false;
  string gradVar;  // float3 var being differentiated, rendered
  // True while emitting a texture EvalD body: float/float3 locals lower to
  // sbdual/sbdual3, dual contexts route through emitDual, and emitExpr
  // projects dual names back to `.v`.
  bool dualBody = false;

  struct NbBinding {
    string name;
    string idxVar;
  };
  Vector<NbBinding> nbStack;

  const NbBinding *findNb(stringref name) const
  {
    for (int i = (int)nbStack.size() - 1; i >= 0; i--) {
      if (string(nbStack[i].name).operator==(string(name.c_str()))) return &nbStack[i];
    }
    return nullptr;
  }

  string resolveVertIndex(const Expr &e)
  {
    if (e.kind == ExprKind::Ident) {
      if (isVertexParam(stringref(e.name.c_str()))) return string("sb_vidx");
      if (auto *nb = findNb(stringref(e.name.c_str()))) return nb->idxVar;
    }
    err("for_neighbor outer must be the vertex bundle or an enclosing neighbor");
    return string("sb_vidx");
  }

  void err(const char *msg) { errors.append(string(msg)); }
  void errf(const char *fmt, const char *arg)
  {
    char buf[256];
    std::snprintf(buf, sizeof(buf), fmt, arg);
    errors.append(string(buf));
  }

  void writeIndent() { for (int i = 0; i < indent; i++) out += "  "; }
  void write(const char *s) { out += s; }
  void write(const string &s) { out += s; }

  bool isLocal(stringref name) const
  {
    for (const auto &l : locals) {
      if (string(l.name).operator==(string(name.c_str()))) return true;
    }
    return false;
  }
  bool isDualLocal(stringref name) const
  {
    for (const auto &l : locals) {
      if (l.dual && string(l.name).operator==(string(name.c_str()))) return true;
    }
    return false;
  }
  bool isPtrParam(stringref name) const
  {
    for (const auto &l : ptrParams) {
      if (string(l).operator==(string(name.c_str()))) return true;
    }
    return false;
  }

  const Field *findField(stringref name) const
  {
    for (const auto &f : brush->fields) {
      if (string(f.name).operator==(string(name.c_str()))) return &f;
    }
    return nullptr;
  }

  bool isStageParam(stringref name) const
  {
    if (!currentStage) return false;
    for (const auto &p : currentStage->params) {
      if (string(p.name).operator==(string(name.c_str()))) return true;
    }
    return false;
  }

  static string lower(const string &s)
  {
    string r = s;
    for (int i = 0; i < (int)r.size(); i++) r[i] = (char)std::tolower((unsigned char)r[i]);
    return r;
  }
  static string texEvalName(const TextureDef &td) { return string("tex_") + lower(td.name) + "_eval"; }

  const TextureDef *findTextureCall(stringref callName) const
  {
    for (const auto &t : brush->textures) {
      string full = t.name + ".eval";
      if (string(full).operator==(string(callName.c_str()))) return &t;
    }
    return nullptr;
  }

  bool isVertexParam(stringref name) const
  {
    if (!vertexStage || vertexStage->params.size() == 0) return false;
    return string(vertexStage->params[0].name).operator==(string(name.c_str()));
  }

  // === expression emitter ===

  void emitExpr(const Expr &e)
  {
    switch (e.kind) {
    case ExprKind::LitFloat: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.17g", e.fvalue);
      bool hasDot = false;
      for (const char *p = buf; *p; p++) {
        if (*p == '.' || *p == 'e' || *p == 'E') { hasDot = true; break; }
      }
      out += buf;
      if (!hasDot) out += ".0";
      out += "f";
      break;
    }
    case ExprKind::LitInt: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%lld", e.ivalue);
      out += buf;
      break;
    }
    case ExprKind::LitBool: out += e.bvalue ? "true" : "false"; break;
    case ExprKind::Ident: {
      stringref nm(e.name.c_str());
      if (dualBody && isDualLocal(nm)) {
        // Value context inside an EvalD body — project the dual's value.
        out += e.name;
        out += ".v";
        break;
      }
      if (isPtrParam(nm)) { out += "(*"; out += e.name; out += ")"; }
      else if (isLocal(nm) || isStageParam(nm)) out += e.name;
      else if (auto *f = findField(nm)) { out += (f->kind == FieldKind::Uniform) ? "brush_u->" : "ctx_u->"; out += e.name; }
      else out += e.name;
      break;
    }
    case ExprKind::Member: {
      if (e.lhs && e.lhs->kind == ExprKind::Ident && isVertexParam(stringref(e.lhs->name.c_str()))) {
        out += e.lhs->name; out += "_"; out += e.name;
      } else if (e.lhs && e.lhs->kind == ExprKind::Ident && findNb(stringref(e.lhs->name.c_str()))) {
        const NbBinding *nb = findNb(stringref(e.lhs->name.c_str()));
        if (std::strcmp(e.name.c_str(), "co") == 0) { out += "co_prev["; out += nb->idxVar; out += "]"; }
        else if (std::strcmp(e.name.c_str(), "no") == 0) { out += "no_buf["; out += nb->idxVar; out += "]"; }
        else if (std::strcmp(e.name.c_str(), "v") == 0) out += nb->idxVar;
        else { errf("neighbor bundle has no member '%s'", e.name.c_str()); out += "/*bad-neighbor-member*/"; }
      } else { emitExpr(*e.lhs); out += "."; out += e.name; }
      break;
    }
    case ExprKind::Index: emitExpr(*e.lhs); out += "["; emitExpr(*e.rhs); out += "]"; break;
    case ExprKind::Binary:
      out += "("; emitExpr(*e.lhs); out += " "; out += binOpCSym(e.binop); out += " "; emitExpr(*e.rhs); out += ")";
      break;
    case ExprKind::Unary: out += "("; out += unaryOpCSym(e.unaryop); emitExpr(*e.lhs); out += ")"; break;
    case ExprKind::Paren: out += "("; emitExpr(*e.lhs); out += ")"; break;
    case ExprKind::Call: {
      const char *n = e.name.c_str();
      // float2/3/4 constructors -> OpenCL vector literals (float3)(a, b, c).
      if (std::strcmp(n, "float2") == 0 || std::strcmp(n, "float3") == 0 || std::strcmp(n, "float4") == 0) {
        out += "(float"; out += n[5]; out += ")(";
        for (int i = 0; i < (int)e.args.size(); i++) { if (i > 0) out += ", "; emitExpr(*e.args[i]); }
        out += ")";
        break;
      }
      // grad(expr, var) — forward-mode gradient, dual-number rewrite. OpenCL C
      // has no operator overloads, so binary ops map to sbd_add/sub/mul/div.
      if (std::strcmp(n, "grad") == 0 && e.args.size() == 2) {
        if (dualBody) {
          err("grad() cannot appear inside a texture eval differentiated by grad()");
          out += "(float3)(0.0f)";
          break;
        }
        gradUsed = true;
        string savedVar = gradVar; gradVar = render(*e.args[1]);
        out += "("; emitDual(*e.args[0]); out += ").d";
        gradVar = savedVar;
        break;
      }
      if (const TextureDef *td = findTextureCall(stringref(e.name.c_str()))) {
        out += texEvalName(*td); out += "(";
        for (int i = 0; i < (int)e.args.size(); i++) { if (i > 0) out += ", "; emitExpr(*e.args[i]); }
        out += ")";
        break;
      }
      const IntrinsicDef *intr = findIntrinsic(stringref(e.name.c_str()));
      if (intr) {
        const char *pat = intr->emit[(int)BackendKind::Opencl].pattern;
        if (!pat) {
          errors.append(string("intrinsic '") + e.name + "' has no OpenCL emit pattern");
          out += "/*missing-opencl-intrinsic*/"; break;
        }
        Vector<string> rendered;
        for (const auto &a : e.args) { string saved = out; out = string(""); emitExpr(*a); rendered.append(out); out = saved; }
        for (const char *p = pat; *p; ) {
          if (*p == '$' && std::isdigit((unsigned char)p[1])) {
            int idx = p[1] - '0'; p += 2;
            out += (idx < (int)rendered.size()) ? rendered[idx] : string("/*bad-arg*/");
          } else if (*p == '$' && p[1] == 'v' && p[2] == 'm') {
            // The kernel's live painted mask local.
            p += 3; out += vertexParamName; out += "_mask";
          } else if (*p == '$' && p[1] == 'v') {
            // Current loop vertex index. Face kernels emit a skip stub, so this
            // only ever renders inside a vertex kernel.
            p += 2; out += "sb_vidx";
          } else { char tmp[2] = {*p, 0}; out += tmp; p++; }
        }
      } else {
        out += e.name; out += "(";
        for (int i = 0; i < (int)e.args.size(); i++) { if (i > 0) out += ", "; emitExpr(*e.args[i]); }
        out += ")";
      }
      break;
    }
    }
  }

  // Dual-number rewrite for grad(). Same shape as emit_wgsl — OpenCL C lacks
  // operator overloads, so binary ops map to sbd_* functions; var seeds the
  // Jacobian and other terms are zero-deriv constants.
  string render(const Expr &e) { string saved = out; out = string(""); emitExpr(e); string r = out; out = saved; return r; }
  bool isGradVar(const Expr &e) { string r = render(e); return string(r).operator==(string(gradVar.c_str())); }
  void emitDual(const Expr &e)
  {
    if (isGradVar(e)) { out += "sb_seed3("; emitExpr(e); out += ")"; return; }
    // Inside an EvalD body a dual local already carries its derivative.
    if (dualBody && e.kind == ExprKind::Ident && isDualLocal(stringref(e.name.c_str()))) {
      out += e.name;
      return;
    }
    switch (e.kind) {
    case ExprKind::LitFloat: case ExprKind::LitInt: out += "sb_c("; emitExpr(e); out += ")"; break;
    case ExprKind::Ident: out += dualBody ? "sb_c(" : "sb_c3("; emitExpr(e); out += ")"; break;
    case ExprKind::Member:
      if (e.lhs && isGradVar(*e.lhs)) { out += "sb_comp(sb_seed3("; emitExpr(*e.lhs); out += "), "; out += (std::strcmp(e.name.c_str(),"x")==0?"0":std::strcmp(e.name.c_str(),"y")==0?"1":"2"); out += ")"; }
      else if (dualBody && e.lhs && e.lhs->kind == ExprKind::Ident && isDualLocal(stringref(e.lhs->name.c_str()))) {
        out += "sb_comp("; out += e.lhs->name; out += ", ";
        out += (std::strcmp(e.name.c_str(),"x")==0?"0":std::strcmp(e.name.c_str(),"y")==0?"1":"2"); out += ")";
      }
      else { out += "sb_c("; emitExpr(e); out += ")"; }
      break;
    case ExprKind::Paren: out += "("; emitDual(*e.lhs); out += ")"; break;
    case ExprKind::Binary: {
      const char *f = e.binop==BinOp::Add?"sbd_add":e.binop==BinOp::Sub?"sbd_sub":e.binop==BinOp::Mul?"sbd_mul":"sbd_div";
      out += f; out += "("; emitDual(*e.lhs); out += ", "; emitDual(*e.rhs); out += ")"; break;
    }
    case ExprKind::Unary: out += "sbd_neg("; emitDual(*e.lhs); out += ")"; break;
    case ExprKind::Call: {
      const char *n = e.name.c_str();
      if (std::strcmp(n,"float3")==0) { out += "sb_v3("; for (int i=0;i<3;i++){if(i)out+=", ";emitDual(*e.args[i]);} out += ")"; break; }
      // Texture call in a dual context — dispatch to the EvalD twin with
      // dual-lifted arguments.
      if (const TextureDef *td = findTextureCall(stringref(e.name.c_str()))) {
        if (currentTexture) {
          errf("texture '%s' cannot call another texture", currentTexture->name.c_str());
          out += "sb_c(0.0f)";
          break;
        }
        out += texEvalName(*td); out += "_d(";
        for (int i = 0; i < (int)e.args.size(); i++) { if (i) out += ", "; emitDual(*e.args[i]); }
        out += ")";
        break;
      }
      if (std::strcmp(n, "grad") == 0) {
        err("nested grad() is not supported");
        out += "sb_c(0.0f)";
        break;
      }
      // Only intrinsics with an sbd_* chain rule in the prelude may appear
      // in a differentiated expression.
      static const char *kDualIntrinsics[] = {
          "sin", "cos", "sqrt", "abs", "dot", "length", "mix", "floor", "fract"};
      bool known = false;
      for (const char *k : kDualIntrinsics) known = known || std::strcmp(n, k) == 0;
      if (!known) {
        errf("'%s' has no derivative rule inside grad()", n);
        out += "sb_c(0.0f)";
        break;
      }
      out += "sbd_"; out += n; out += "(";
      for (int i = 0; i < (int)e.args.size(); i++) { if (i) out += ", "; emitDual(*e.args[i]); }
      out += ")"; break;
    }
    default: out += "sb_c(0.0f)"; break;
    }
  }
  static bool exprUsesGrad(const Expr *e) { if(!e)return false; if(e->kind==ExprKind::Call&&std::strcmp(e->name.c_str(),"grad")==0)return true; if(exprUsesGrad(e->lhs.get())||exprUsesGrad(e->rhs.get()))return true; for(const auto&a:e->args)if(exprUsesGrad(a.get()))return true; return false; }
  static bool stmtUsesGrad(const Stmt *s) { if(!s)return false; if(exprUsesGrad(s->expr.get())||exprUsesGrad(s->cond.get())||exprUsesGrad(s->lvalue.get())||exprUsesGrad(s->rvalue.get()))return true; for(const auto&c:s->stmts)if(stmtUsesGrad(c.get()))return true; return stmtUsesGrad(s->thenBranch.get())||stmtUsesGrad(s->elseBranch.get())||stmtUsesGrad(s->forInit.get())||stmtUsesGrad(s->forStep.get()); }
  bool brushUsesGrad() const { for(const auto&st:brush->stages)if(stmtUsesGrad(st.body.get()))return true; return false; }

  // === statement emitter ===

  void emitStmt(const Stmt &s)
  {
    switch (s.kind) {
    case StmtKind::Block: {
      writeIndent(); out += "{\n"; indent++;
      int sl = (int)locals.size();
      for (const auto &c : s.stmts) emitStmt(*c);
      while ((int)locals.size() > sl) locals.pop_back();
      indent--; writeIndent(); out += "}\n";
      break;
    }
    case StmtKind::DeclLocal:
      if (dualBody && (s.declType == TypeKind::Float || s.declType == TypeKind::Float3)) {
        writeIndent();
        out += (s.declType == TypeKind::Float) ? "sbdual" : "sbdual3";
        out += " "; out += s.name;
        if (s.expr) { out += " = "; emitDual(*s.expr); }
        out += ";\n"; locals.append(LocalVar{s.name, /*dual=*/true});
        break;
      }
      writeIndent();
      out += (s.declType == TypeKind::Struct) ? s.declStructName.c_str() : clType(s.declType);
      out += " "; out += s.name;
      if (s.expr) { out += " = "; emitExpr(*s.expr); }
      out += ";\n"; locals.append(LocalVar{s.name});
      break;
    case StmtKind::Assign:
      if (dualBody && s.lvalue->kind == ExprKind::Ident && isDualLocal(stringref(s.lvalue->name.c_str()))) {
        // OpenCL C has no operator overloads: compound assigns on duals expand
        // through the sbd_* function forms.
        writeIndent(); out += s.lvalue->name; out += " = ";
        char opc = assignOpCSym(s.assignOp)[0];
        if (opc != '=') {
          const char *f = opc=='+'?"sbd_add":opc=='-'?"sbd_sub":opc=='*'?"sbd_mul":"sbd_div";
          out += f; out += "("; out += s.lvalue->name; out += ", "; emitDual(*s.rvalue); out += ")";
        } else emitDual(*s.rvalue);
        out += ";\n";
        break;
      }
      if (dualBody && s.lvalue->kind == ExprKind::Member && s.lvalue->lhs &&
          s.lvalue->lhs->kind == ExprKind::Ident && isDualLocal(stringref(s.lvalue->lhs->name.c_str()))) {
        errf("component assignment to dual '%s' is not differentiable", s.lvalue->lhs->name.c_str());
        break;
      }
      writeIndent(); emitExpr(*s.lvalue); out += " "; out += assignOpCSym(s.assignOp); out += " "; emitExpr(*s.rvalue); out += ";\n";
      break;
    case StmtKind::If: {
      writeIndent(); out += "if ("; emitExpr(*s.cond); out += ") ";
      if (s.thenBranch && s.thenBranch->kind == StmtKind::Block) {
        out += "{\n"; indent++; int sl = (int)locals.size();
        for (const auto &c : s.thenBranch->stmts) emitStmt(*c);
        while ((int)locals.size() > sl) locals.pop_back();
        indent--; writeIndent(); out += "}";
      } else if (s.thenBranch) { out += "{\n"; indent++; emitStmt(*s.thenBranch); indent--; writeIndent(); out += "}"; }
      if (s.elseBranch) {
        out += " else ";
        if (s.elseBranch->kind == StmtKind::Block) {
          out += "{\n"; indent++; int sl = (int)locals.size();
          for (const auto &c : s.elseBranch->stmts) emitStmt(*c);
          while ((int)locals.size() > sl) locals.pop_back();
          indent--; writeIndent(); out += "}\n";
        } else if (s.elseBranch->kind == StmtKind::If) emitStmt(*s.elseBranch);
        else { out += "{\n"; indent++; emitStmt(*s.elseBranch); indent--; writeIndent(); out += "}\n"; }
      } else out += "\n";
      break;
    }
    case StmtKind::For: {
      writeIndent(); out += "for (";
      auto renderFrag = [&](const Stmt &child, bool stripSemi) {
        string saved = out; out = string(""); int si = indent; indent = 0;
        emitStmt(child); indent = si; string frag = out; out = saved;
        int n = (int)frag.size(); while (n > 0 && frag[n-1] == '\n') n--;
        if (stripSemi && n > 0 && frag[n-1] == ';') n--;
        for (int i = 0; i < n; i++) { char tmp[2] = {frag[i], 0}; out += tmp; }
      };
      if (s.forInit) renderFrag(*s.forInit, false);
      out += " "; emitExpr(*s.cond); out += "; ";
      if (s.forStep) renderFrag(*s.forStep, true);
      out += ") ";
      if (s.thenBranch && s.thenBranch->kind == StmtKind::Block) {
        out += "{\n"; indent++; int sl = (int)locals.size();
        for (const auto &c : s.thenBranch->stmts) emitStmt(*c);
        while ((int)locals.size() > sl) locals.pop_back();
        indent--; writeIndent(); out += "}\n";
      } else if (s.thenBranch) { out += "{\n"; indent++; emitStmt(*s.thenBranch); indent--; writeIndent(); out += "}\n"; }
      break;
    }
    case StmtKind::Continue: writeIndent(); out += "return;\n"; break;
    case StmtKind::Return: writeIndent(); out += "return"; if (s.expr) { out += " "; if (dualBody) emitDual(*s.expr); else emitExpr(*s.expr); } out += ";\n"; break;
    case StmtKind::ExprStmt: writeIndent(); emitExpr(*s.expr); out += ";\n"; break;
    case StmtKind::NeighborLoop: {
      int depth = (int)nbStack.size();
      char sfx[16]; std::snprintf(sfx, sizeof(sfx), "%d", depth);
      string outerIdx = resolveVertIndex(*s.lvalue);
      string metaVar = string("sb_nbr_meta") + sfx, niVar = string("sb_ni") + sfx, idxVar = string("sb_nb_v") + sfx;
      writeIndent(); out += "{\n"; indent++;
      writeIndent(); out += "uint2 " + metaVar + " = vert_nbr_meta[" + outerIdx + "];\n";
      writeIndent(); out += "for (uint " + niVar + " = 0u; " + niVar + " < " + metaVar + ".y; " + niVar + " = " + niVar + " + 1u) {\n";
      indent++;
      writeIndent(); out += "uint " + idxVar + " = nbr_verts[" + metaVar + ".x + " + niVar + "];\n";
      nbStack.append(NbBinding{s.name, idxVar});
      if (s.thenBranch && s.thenBranch->kind == StmtKind::Block) {
        int sl = (int)locals.size();
        for (const auto &c : s.thenBranch->stmts) emitStmt(*c);
        while ((int)locals.size() > sl) locals.pop_back();
      } else if (s.thenBranch) emitStmt(*s.thenBranch);
      nbStack.pop_back();
      indent--; writeIndent(); out += "}\n"; indent--; writeIndent(); out += "}\n";
      break;
    }
    }
  }

  // === top-level emitter ===

  void emitPrelude()
  {
    write("// AUTO-GENERATED by sbrushc (OpenCL backend) — DO NOT EDIT.\n");
    write("// Source: "); write(brush->sourceFile); write("\n\n");

    write("struct NodeMeta { uint vert_offset; uint vert_count; };\n");
    write("struct StrokeSample { float3 pos; float3 normal; float arclen; };\n");
    write("typedef struct NodeMeta NodeMeta;\n");
    write("typedef struct StrokeSample StrokeSample;\n\n");

    // Forward-mode dual prelude — emitted only when grad() is used. Mirrors the
    // cpp/wgsl duals so all backends agree; OpenCL vectors lack [] indexing, so
    // sb_idx picks components explicitly.
    if (brushUsesGrad()) {
      write("typedef struct { float v; float3 d; } sbdual;\n");
      write("typedef struct { float3 v; float3 dx, dy, dz; } sbdual3;\n");
      write("inline float sb_idx(float3 a, int i) { return (i==0)?a.x:(i==1)?a.y:a.z; }\n");
      write("inline sbdual sb_c(float x) { sbdual r; r.v=x; r.d=(float3)(0.0f); return r; }\n");
      write("inline sbdual3 sb_c3(float3 p) { sbdual3 r; r.v=p; r.dx=(float3)(0.0f); r.dy=(float3)(0.0f); r.dz=(float3)(0.0f); return r; }\n");
      write("inline sbdual3 sb_seed3(float3 p) { sbdual3 r; r.v=p; r.dx=(float3)(1.0f,0.0f,0.0f); r.dy=(float3)(0.0f,1.0f,0.0f); r.dz=(float3)(0.0f,0.0f,1.0f); return r; }\n");
      write("inline sbdual sb_comp(sbdual3 a, int i) { sbdual r; r.v=sb_idx(a.v,i); r.d=(float3)(sb_idx(a.dx,i), sb_idx(a.dy,i), sb_idx(a.dz,i)); return r; }\n");
      write("inline sbdual3 sb_v3(sbdual x, sbdual y, sbdual z) { sbdual3 r; r.v=(float3)(x.v,y.v,z.v); r.dx=(float3)(x.d.x,y.d.x,z.d.x); r.dy=(float3)(x.d.y,y.d.y,z.d.y); r.dz=(float3)(x.d.z,y.d.z,z.d.z); return r; }\n");
      write("inline sbdual sbd_add(sbdual a, sbdual b) { sbdual r; r.v=a.v+b.v; r.d=a.d+b.d; return r; }\n");
      write("inline sbdual sbd_sub(sbdual a, sbdual b) { sbdual r; r.v=a.v-b.v; r.d=a.d-b.d; return r; }\n");
      write("inline sbdual sbd_neg(sbdual a) { sbdual r; r.v=-a.v; r.d=-a.d; return r; }\n");
      write("inline sbdual sbd_mul(sbdual a, sbdual b) { sbdual r; r.v=a.v*b.v; r.d=a.d*b.v + b.d*a.v; return r; }\n");
      write("inline sbdual sbd_div(sbdual a, sbdual b) { sbdual r; r.v=a.v/b.v; r.d=(a.d*b.v - b.d*a.v)/(b.v*b.v); return r; }\n");
      write("inline sbdual sbd_sin(sbdual a) { sbdual r; r.v=sin(a.v); r.d=a.d*cos(a.v); return r; }\n");
      write("inline sbdual sbd_cos(sbdual a) { sbdual r; r.v=cos(a.v); r.d=a.d*(-sin(a.v)); return r; }\n");
      write("inline sbdual sbd_sqrt(sbdual a) { float rr=sqrt(a.v); sbdual r; r.v=rr; r.d=a.d*(rr>0.0f?0.5f/rr:0.0f); return r; }\n");
      write("inline sbdual sbd_abs(sbdual a) { sbdual r; r.v=fabs(a.v); r.d=a.d*(a.v<0.0f?-1.0f:1.0f); return r; }\n");
      write("inline sbdual sbd_dot(sbdual3 a, sbdual3 b) { sbdual r; r.v=dot(a.v,b.v); r.d=a.dx*b.v.x+b.dx*a.v.x+a.dy*b.v.y+b.dy*a.v.y+a.dz*b.v.z+b.dz*a.v.z; return r; }\n");
      write("inline sbdual sbd_length(sbdual3 a) { return sbd_sqrt(sbd_dot(a,a)); }\n");
      write("inline sbdual sbd_mix(sbdual a, sbdual b, sbdual t) { return sbd_add(a, sbd_mul(sbd_sub(b,a), t)); }\n");
      write("inline sbdual sbd_floor(sbdual a) { sbdual r; r.v=floor(a.v); r.d=(float3)(0.0f); return r; }\n");
      write("inline sbdual sbd_fract(sbdual a) { sbdual r; r.v=a.v-floor(a.v); r.d=a.d; return r; }\n\n");
    }

    for (const auto &sd : brush->structs) {
      write("typedef struct {\n");
      for (const auto &f : sd.fields) { write("  "); write(clType(f.type)); write(" "); write(f.name); write(";\n"); }
      write("} "); write(sd.name); write(";\n\n");
    }

    auto isBuiltinBrushName = [](const char *n) {
      return std::strcmp(n,"strength")==0||std::strcmp(n,"radius")==0||std::strcmp(n,"spacing")==0||
             std::strcmp(n,"invert")==0||std::strcmp(n,"falloff_kind")==0||std::strcmp(n,"falloff_shape")==0||
             std::strcmp(n,"falloff_dir")==0||std::strcmp(n,"unbounded_extent")==0||std::strcmp(n,"coord_space")==0||std::strcmp(n,"tex_repeat")==0||
             std::strcmp(n,"stroke_path_count")==0;
    };
    auto isBuiltinCtxName = [](const char *n) {
      return std::strcmp(n,"surfacePos")==0||std::strcmp(n,"surfaceNo")==0||std::strcmp(n,"render_matrix")==0;
    };
    auto writeFieldMember = [&](const Field &f) {
      out += "  ";
      if (f.type == TypeKind::Array) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%d", f.arraySize);
        out += clType(f.arrayElem); out += " "; out += f.name; out += "["; out += buf; out += "]";
      } else { out += clType(f.type); out += " "; out += f.name; }
      out += ";\n";
    };

    write("typedef struct {\n");
    write("  float strength; float radius; float spacing; uint invert;\n");
    write("  uint falloff_kind; uint falloff_shape; float3 falloff_dir;\n");
    write("  uint coord_space; float tex_repeat; uint stroke_path_count;\n");
    for (const auto &f : brush->fields) { if (f.kind != FieldKind::Uniform || isBuiltinBrushName(f.name.c_str())) continue; writeFieldMember(f); }
    write("} BrushUniforms;\n\n");

    // Column-major 4x4 as a flat array — clspv rejects 16-element vectors.
    write("typedef struct { float m[16]; } sb_mat4;\n");
    write("typedef struct {\n");
    write("  float3 surfacePos; float3 surfaceNo; sb_mat4 render_matrix;\n");
    for (const auto &f : brush->fields) { if (f.kind != FieldKind::Ctx || isBuiltinCtxName(f.name.c_str())) continue; writeFieldMember(f); }
    write("} CtxUniforms;\n\n");

    write("inline float3 sb_mat4_mul_point(sb_mat4 t, float3 p) {\n");
    write("  return (float3)(t.m[0]*p.x + t.m[4]*p.y + t.m[8]*p.z  + t.m[12],\n");
    write("                  t.m[1]*p.x + t.m[5]*p.y + t.m[9]*p.z  + t.m[13],\n");
    write("                  t.m[2]*p.x + t.m[6]*p.y + t.m[10]*p.z + t.m[14]);\n");
    write("}\n\n");

    // brush_* helpers take the uniform/buffer pack explicitly; the call-site
    // macros below bind them to fixed kernel-arg names, so intrinsics emit the
    // bare brush_strength()/brush_falloff()/brush_sample_tex() forms.
    write("inline float sb_falloff(__constant BrushUniforms* bu, __constant float* lut, float t) {\n");
    write("  if (bu->falloff_kind == 1u) return t;\n");
    write("  if (bu->falloff_kind == 2u) { float u = 1.0f - t; return exp(-9.0f * u * u); }\n");
    write("  if (bu->falloff_kind == 3u) { float c = clamp(t,0.0f,1.0f); float s=c*255.0f; int i=(int)floor(s);\n");
    write("    if (i >= 255) return lut[255]; float f = s - (float)i; return lut[i]*(1.0f-f) + lut[i+1]*f; }\n");
    write("  return t * t * (3.0f - 2.0f * t);\n");
    write("}\n");
    write("inline float sb_falloff_dist(__constant BrushUniforms* bu, float3 d) {\n");
    write("  float ir = 1.0f / bu->radius;\n");
    write("  if (bu->falloff_shape == 1u) { float3 a = fabs(d); return fmax(a.x, fmax(a.y, a.z)) * ir; }\n");
    write("  if (bu->falloff_shape == 2u) return fabs(dot(d, bu->falloff_dir)) * ir;\n");
    write("  return length(d) * ir;\n");
    write("}\n");
    write("inline float2 sb_stroke_uv(__constant BrushUniforms* bu, __global StrokeSample* sp, float3 co) {\n");
    write("  if (bu->stroke_path_count == 0u) return (float2)(0.0f, 0.0f);\n");
    write("  if (bu->stroke_path_count == 1u) return (float2)(sp[0].arclen, length(co - sp[0].pos));\n");
    write("  float bd = 3.402823e+38f, ba = 0.0f, bl = 0.0f;\n");
    write("  for (uint i = 0u; i + 1u < bu->stroke_path_count; i = i + 1u) {\n");
    write("    float3 a = sp[i].pos; float3 ab = sp[i+1u].pos - a; float l2 = dot(ab, ab); float t = 0.0f;\n");
    write("    if (l2 > 0.0f) t = dot(co - a, ab) / l2; t = clamp(t, 0.0f, 1.0f);\n");
    write("    float dd = length(co - (a + ab*t)); if (dd < bd) { bd = dd; ba = sp[i].arclen + (sp[i+1u].arclen - sp[i].arclen)*t; bl = dd; }\n");
    write("  }\n");
    write("  return (float2)(ba, bl);\n");
    write("}\n");
    write("inline float sb_sample_tex(__constant BrushUniforms* bu, __constant CtxUniforms* cu, __global StrokeSample* sp,\n");
    write("                           __global float* tex, int w, int h, float3 co, float3 no) {\n");
    write("  (void)no; float2 uv;\n");
    write("  if (bu->coord_space == 1u) { float3 p = sb_mat4_mul_point(cu->render_matrix, co); uv = (float2)(p.x, p.y); }\n");
    // Aspect-corrected tiled repeat; mirrors CommandCtx::sampleBrushTex.
    write("  else if (bu->coord_space == 2u) { float3 p = sb_mat4_mul_point(cu->render_matrix, co);\n");
    write("    __constant float* m = cu->render_matrix.m;\n");
    write("    float r0 = sqrt(m[0]*m[0] + m[4]*m[4] + m[8]*m[8]);\n");
    write("    float r1 = sqrt(m[1]*m[1] + m[5]*m[5] + m[9]*m[9]);\n");
    write("    float asp = (r0 > 1e-12f && r1 > 1e-12f) ? r1 / r0 : 1.0f;\n");
    write("    float ux = p.x*asp*bu->tex_repeat, uy = p.y*bu->tex_repeat;\n");
    write("    uv = (float2)(ux - floor(ux), uy - floor(uy)); }\n");
    write("  else if (bu->coord_space == 3u) uv = sb_stroke_uv(bu, sp, co);\n");
    write("  else if (bu->coord_space == 4u) { float3 n = normalize(cu->surfaceNo); float3 r = (float3)(0.0f,0.0f,1.0f);\n");
    write("    if (fabs(n.z) >= 0.999f) r = (float3)(1.0f,0.0f,0.0f); float3 t1 = normalize(cross(r,n)); float3 t2 = cross(n,t1);\n");
    write("    float3 rel = co - cu->surfacePos; uv = (float2)(dot(rel,t1), dot(rel,t2)); }\n");
    // GLOBAL: bitmap spans world [-1,1]^2 (host bake domain), tiled.
    write("  else { float gx = co.x*0.5f + 0.5f, gy = co.y*0.5f + 0.5f;\n");
    write("    uv = (float2)(gx - floor(gx), gy - floor(gy)); }\n");
    write("  float fx = uv.x*(float)w - 0.5f, fy = uv.y*(float)h - 0.5f; float x0 = floor(fx), y0 = floor(fy);\n");
    write("  float tx = fx-x0, ty = fy-y0;\n");
    write("  int x0c=(int)clamp(x0,0.0f,(float)(w-1)), y0c=(int)clamp(y0,0.0f,(float)(h-1));\n");
    write("  int x1c=(int)clamp(x0+1.0f,0.0f,(float)(w-1)), y1c=(int)clamp(y0+1.0f,0.0f,(float)(h-1));\n");
    write("  float p00=tex[y0c*w+x0c], p10=tex[y0c*w+x1c], p01=tex[y1c*w+x0c], p11=tex[y1c*w+x1c];\n");
    write("  return mix(mix(p00,p10,tx), mix(p01,p11,tx), ty);\n");
    write("}\n");
    // Spatial + scalar term only (slider x falloff x brush texture); defined
    // after sb_sample_tex (define-before-use). Mirrors CommandCtx::strength.
    write("inline float sb_strength(__constant BrushUniforms* bu, __constant CtxUniforms* cu, __constant float* lut,\n");
    write("                         __global StrokeSample* sp, __global float* tex, int w, int h, float3 p) {\n");
    write("  float t = 1.0f - fmin(sb_falloff_dist(bu, p - cu->surfacePos), 1.0f);\n");
    write("  float s = bu->strength * sb_falloff(bu, lut, t) * sb_sample_tex(bu, cu, sp, tex, w, h, p, cu->surfaceNo);\n");
    write("  return bu->invert != 0u ? -s : s;\n");
    write("}\n\n");
    write("#define brush_falloff(t) sb_falloff(brush_u, falloff_lut, (t))\n");
    write("#define brush_strength(p) sb_strength(brush_u, ctx_u, falloff_lut, stroke_path, brush_tex, brush_tex_w, brush_tex_h, (p))\n");
    // No automask/view-normal binding on this backend yet, so masks() degrades
    // to the painted layer alone (automasks() emits 1.0f from the table).
    write("#define brush_masks(vid, m) (1.0f - (m))\n");
    write("#define brush_sample_tex(c, n) sb_sample_tex(brush_u, ctx_u, stroke_path, brush_tex, brush_tex_w, brush_tex_h, (c), (n))\n\n");
  }

  void emitSkipStub(const char *reason)
  {
    write("// AUTO-GENERATED by sbrushc (OpenCL backend) — DO NOT EDIT.\n");
    write("// Source: "); write(brush->sourceFile); write("\n// SKIPPED: "); write(reason); write("\n\n");
    write("__kernel void nop() {}\n");
  }

  // T1 texture features (params, mapPoint, samplers) are cpp/wgsl-only for now.
  void emitTextureFn(const TextureDef &td)
  {
    if (td.texParams.size() > 0 || td.usesMap || td.samplerDeps.size() > 0) {
      errf("texture '%s' uses params/mapPoint/samplers, unsupported on the opencl backend", td.name.c_str());
      return;
    }
    write("inline "); write(clType(td.returnType)); write(" "); write(texEvalName(td)); write("(");
    bool first = true;
    for (const auto &p : td.params) { if (!first) write(", "); first = false; write(clType(p.type)); write(" "); write(p.name); }
    write(") {\n");
    indent = 1;
    Stage scratch; scratch.kind = StageKind::Reduce;
    for (const auto &p : td.params) scratch.params.append(p);
    currentStage = &scratch;
    currentTexture = &td;
    if (td.body && td.body->kind == StmtKind::Block) {
      int sl = (int)locals.size();
      for (const auto &c : td.body->stmts) emitStmt(*c);
      while ((int)locals.size() > sl) locals.pop_back();
    }
    currentTexture = nullptr;
    currentStage = nullptr; indent = 0;
    write("}\n\n");
  }

  // Dual-number twin of a texture eval, called from grad() rewrites. The Eval
  // emission already reported unsupported textures, so skip those silently.
  void emitTextureFnDual(const TextureDef &td)
  {
    if (td.texParams.size() > 0 || td.usesMap || td.samplerDeps.size() > 0) return;
    write("inline "); write(td.returnType == TypeKind::Float ? "sbdual" : "sbdual3");
    write(" "); write(texEvalName(td)); write("_d(");
    bool first = true;
    for (const auto &p : td.params) {
      if (!first) write(", "); first = false;
      write(p.type == TypeKind::Float ? "sbdual" : "sbdual3"); write(" "); write(p.name);
    }
    write(") {\n");
    indent = 1;
    Stage scratch; scratch.kind = StageKind::Reduce;
    for (const auto &p : td.params) scratch.params.append(p);
    currentStage = &scratch;
    currentTexture = &td;
    dualBody = true;
    int sl = (int)locals.size();
    for (const auto &p : td.params) locals.append(LocalVar{p.name, /*dual=*/true});
    if (td.body && td.body->kind == StmtKind::Block) {
      for (const auto &c : td.body->stmts) emitStmt(*c);
    }
    while ((int)locals.size() > sl) locals.pop_back();
    dualBody = false;
    currentTexture = nullptr;
    currentStage = nullptr; indent = 0;
    write("}\n\n");
  }

  // Reduce stage -> function. out/inout params are __private pointers so the
  // callee writes back; the brush helper pack is threaded so macros resolve.
  void emitReduceStage(const Stage &st)
  {
    write("inline void "); write(st.name); write("(");
    bool first = true;
    for (const auto &p : st.params) {
      if (!first) write(", "); first = false;
      const char *ts = (p.type == TypeKind::Struct) ? p.structName.c_str() : clType(p.type);
      write(ts);
      if (p.dir == ParamDir::Out || p.dir == ParamDir::InOut) { write("* "); ptrParams.append(p.name); }
      else write(" ");
      write(p.name);
    }
    write(", __constant BrushUniforms* brush_u, __constant CtxUniforms* ctx_u, __constant float* falloff_lut,");
    write(" __global StrokeSample* stroke_path, __global float* brush_tex, int brush_tex_w, int brush_tex_h) {\n");
    indent = 1; currentStage = &st;
    if (st.body && st.body->kind == StmtKind::Block) {
      int sl = (int)locals.size();
      for (const auto &c : st.body->stmts) emitStmt(*c);
      while ((int)locals.size() > sl) locals.pop_back();
    }
    currentStage = nullptr; indent = 0;
    ptrParams.clear();
    write("}\n\n");
  }

  void run()
  {
    if (!vertexStage) { emitSkipStub("non-vertex (e.g. face) stage: GPU dispatch not yet implemented"); return; }
    if (vertexStage->params.size() < 1) err("vertex stage must take at least one parameter (the Vertex bundle)");
    usesNeighbors = hasNeighborLoop(vertexStage->body.get());

    emitPrelude();
    for (const auto &td : brush->textures) {
      emitTextureFn(td);
      if (brushUsesGrad()) emitTextureFnDual(td);
    }

    Vector<const Stage *> reduceStages;
    for (const auto &st : brush->stages) if (st.kind == StageKind::Reduce) reduceStages.append(&st);
    for (const auto *st : reduceStages) emitReduceStage(*st);

    string kernelName = brush->attrName.size() > 0 ? brush->attrName : brush->cppName;
    write("__kernel void "); write(kernelName); write("(\n");
    write("    __global float3* co_buf, __global float3* no_buf, __global float* mask_buf,\n");
    write("    __global uint* unique_verts, __global NodeMeta* nodes,\n");
    write("    __constant BrushUniforms* brush_u, __constant CtxUniforms* ctx_u, __constant float* falloff_lut,\n");
    write("    __global float* brush_tex, int brush_tex_w, int brush_tex_h, __global StrokeSample* stroke_path");
    if (usesNeighbors) write(",\n    __global float3* co_prev, __global uint2* vert_nbr_meta, __global uint* nbr_verts");
    write(") {\n");
    write("  NodeMeta sb_node = nodes[get_group_id(0)];\n");
    write("  uint lid = get_local_id(0);\n");
    write("  if (lid >= sb_node.vert_count) return;\n");
    write("  uint sb_vidx = unique_verts[sb_node.vert_offset + lid];\n");
    write("  float3 "); write(vertexParamName); write("_co = co_buf[sb_vidx];\n");
    write("  float3 "); write(vertexParamName); write("_no = no_buf[sb_vidx];\n");
    write("  float ");  write(vertexParamName); write("_mask = mask_buf[sb_vidx];\n");

    for (int pi = 1; pi < (int)vertexStage->params.size(); pi++) {
      const auto &p = vertexStage->params[pi];
      write("  "); write(p.type == TypeKind::Struct ? p.structName.c_str() : clType(p.type)); write(" "); write(p.name); write(";\n");
    }
    for (const auto *st : reduceStages) {
      write("  "); write(st->name); write("(");
      bool first = true;
      for (const auto &rp : st->params) {
        if (!first) write(", "); first = false;
        bool found = false;
        for (int pi = 1; pi < (int)vertexStage->params.size(); pi++) {
          const auto &vp = vertexStage->params[pi];
          if (vp.type != rp.type) continue;
          if (!string(vp.name).operator==(string(rp.name.c_str()))) continue;
          if (rp.type == TypeKind::Struct && !string(vp.structName).operator==(string(rp.structName.c_str()))) continue;
          if (rp.dir == ParamDir::Out || rp.dir == ParamDir::InOut) write("&");
          write(vp.name); found = true; break;
        }
        if (!found) { errf("reduce param '%s' has no matching vertex-stage local of the same type", rp.name.c_str()); write("/*unmatched*/"); }
      }
      write(", brush_u, ctx_u, falloff_lut, stroke_path, brush_tex, brush_tex_w, brush_tex_h);\n");
    }
    write("\n");

    indent = 1; currentStage = vertexStage;
    if (vertexStage->body && vertexStage->body->kind == StmtKind::Block) {
      int sl = (int)locals.size();
      for (const auto &c : vertexStage->body->stmts) emitStmt(*c);
      while ((int)locals.size() > sl) locals.pop_back();
    }
    currentStage = nullptr; indent = 0;

    write("\n  co_buf[sb_vidx] = ");   write(vertexParamName); write("_co;\n");
    write("  no_buf[sb_vidx] = ");     write(vertexParamName); write("_no;\n");
    write("  mask_buf[sb_vidx] = ");   write(vertexParamName); write("_mask;\n");
    write("}\n");
  }
};

} // namespace

EmitResult emitOpencl(const Brush &brush)
{
  Emit em;
  em.brush = &brush;
  for (const auto &st : brush.stages) { if (st.kind == StageKind::Vertex) { em.vertexStage = &st; break; } }
  em.vertexParamName = (em.vertexStage && em.vertexStage->params.size() > 0)
                           ? em.vertexStage->params[0].name : string("v");
  em.run();
  EmitResult r; r.text = std::move(em.out); r.errors = std::move(em.errors); return r;
}

} // namespace sculptcore::brush::sbrush
