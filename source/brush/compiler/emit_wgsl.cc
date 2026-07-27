#include "emit_wgsl.h"
#include "../kernels/ir/intrinsics.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace sculptcore::brush::sbrush {

using litestl::util::string;
using litestl::util::stringref;
using litestl::util::Vector;

namespace {

// WGSL spelling for primitive types. Matches the IR's TypeKind set; the
// engine has no WGSL-specific types yet, so anything unknown lowers to
// `f32` to keep tint moving — typed parsing rejects truly bogus input
// upstream.
const char *wgslType(TypeKind k)
{
  switch (k) {
  case TypeKind::Void: return "void";
  case TypeKind::Bool: return "bool";
  case TypeKind::Int: return "i32";
  case TypeKind::Float: return "f32";
  case TypeKind::Float2: return "vec2<f32>";
  case TypeKind::Float3: return "vec3<f32>";
  case TypeKind::Float4: return "vec4<f32>";
  default: return "f32";
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
  // Wave 1b: per-face GPU dispatch. A brush with a `face` stage and no vertex
  // stage (e.g. polygroup) emits a face kernel instead of the vertex kernel.
  const Stage *faceStage = nullptr;
  string faceParamName;  // e.g. "f"

  // Stage currently being lowered — drives stage-param identifier
  // resolution (so reduce-body `s` and vertex-body `v` route correctly).
  const Stage *currentStage = nullptr;

  string out;
  Vector<string> errors;
  int indent = 0;

  Vector<string> locals;

  // True when the vertex stage uses for_neighbor — gates the extra
  // co_prev / neighbor-CSR bindings (11-13) and the NeighborLoop lowering.
  bool usesNeighbors = false;
  // Set when grad(expr, var) is used — emits the forward-mode dual prelude.
  bool gradUsed = false;
  string gradVar;  // float3 var being differentiated, rendered

  // Active for_neighbor bundles (name + its WGSL neighbor-index variable),
  // pushed while lowering a NeighborLoop body so member access on the
  // bundle (`nb.co`/`nb.no`/`nb.v`) routes to the CSR buffers.
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

  // Resolve the global vertex-index expression a for_neighbor iterates
  // around. Only the vertex param (sb_vidx) or an enclosing neighbor
  // bundle is supported — matching the C++ lowering's `<outer>.v`.
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

  void writeIndent()
  {
    for (int i = 0; i < indent; i++) out += "  ";
  }
  void write(const char *s) { out += s; }
  void write(const string &s) { out += s; }

  bool isLocal(stringref name) const
  {
    for (const auto &l : locals) {
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
    for (int i = 0; i < (int)r.size(); i++) {
      r[i] = (char)std::tolower((unsigned char)r[i]);
    }
    return r;
  }

  // Mangled WGSL name for an inline texture's eval function.
  static string texEvalName(const TextureDef &td)
  {
    return string("tex_") + lower(td.name) + "_eval";
  }

  // Resolve a dotted call name like "Rings.eval" to its texture def.
  const TextureDef *findTextureCall(stringref callName) const
  {
    for (const auto &t : brush->textures) {
      string full = t.name + ".eval";
      if (string(full).operator==(string(callName.c_str()))) return &t;
    }
    return nullptr;
  }

  // Reduce-stage out/inout params (struct or scalar) are lowered to WGSL
  // `ptr<function, T>`, so identifier references to them have to be
  // dereferenced inline — `s.a = x` becomes `(*s).a = x`, `w = 1.0`
  // becomes `(*w) = 1.0`.
  bool isOutPtrParam(stringref name) const
  {
    if (!currentStage) return false;
    for (const auto &p : currentStage->params) {
      if (!string(p.name).operator==(string(name.c_str()))) continue;
      return p.dir == ParamDir::Out || p.dir == ParamDir::InOut;
    }
    return false;
  }

  bool isVertexParam(stringref name) const
  {
    if (!vertexStage) return false;
    // Only the first param of the vertex stage is the Vertex bundle; the
    // rest are struct-typed locals which share the regular ident path.
    if (vertexStage->params.size() == 0) return false;
    const auto &p = vertexStage->params[0];
    return string(p.name).operator==(string(name.c_str()));
  }

  // True when emitting a face kernel (a `face` stage, no vertex stage). Gates
  // the face binding layout + compute entry; the vertex path is untouched.
  bool faceMode() const { return faceStage && !vertexStage; }

  /** Which non-accumulate base source this kernel declares — mutually
   * exclusive. Grab-class reads the absolute stroke-start snapshot (binding
   * 22); everything else accumulable derives its base from the displacement
   * field (binding 25). @grabmode kernels are also non-global/non-paint, so
   * they must be excluded from the disp side explicitly.
   * CLAUDENOTE(M5): wantsOrigCo() disappears when grab moves onto disp. */
  bool wantsOrigCo() const { return !faceMode() && brush->isGrabMode; }
  bool wantsDisp() const
  {
    return !faceMode() && !brush->isGrabMode && !brush->isGlobal && !brush->isPaint;
  }

  // The Face bundle param (`f`) of the face stage — its member access routes to
  // per-thread locals (f_center / f_no / f_group), mirroring the vertex param.
  bool isFaceParam(stringref name) const
  {
    if (!faceStage || faceStage->params.size() == 0) return false;
    const auto &p = faceStage->params[0];
    return string(p.name).operator==(string(name.c_str()));
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
      // No 'f' suffix — older WGSL didn't accept it, and untyped float
      // literals coerce fine in every position we emit.
      break;
    }
    case ExprKind::LitInt: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%lld", e.ivalue);
      out += buf;
      break;
    }
    case ExprKind::LitBool:
      out += e.bvalue ? "true" : "false";
      break;
    case ExprKind::Ident: {
      stringref nm(e.name.c_str());
      if (isOutPtrParam(nm)) {
        out += "(*";
        out += e.name;
        out += ")";
      } else if (isLocal(nm) || isStageParam(nm)) {
        out += e.name;
      } else if (auto *f = findField(nm)) {
        if (f->kind == FieldKind::Uniform) {
          out += "brush_u.";
          out += e.name;
        } else {
          out += "ctx_u.";
          out += e.name;
        }
      } else {
        out += e.name;
      }
      break;
    }
    case ExprKind::Member: {
      // Vertex-param member access (`v.co`, `v.no`, `v.mask`) targets
      // local mutable vars seeded from the per-thread storage loads.
      if (e.lhs && e.lhs->kind == ExprKind::Ident && isVertexParam(stringref(e.lhs->name.c_str()))) {
        out += e.lhs->name;
        out += "_";
        out += e.name;
      } else if (e.lhs && e.lhs->kind == ExprKind::Ident &&
                 isFaceParam(stringref(e.lhs->name.c_str()))) {
        // Face-param member (`f.center`, `f.no`, `f.group`, `f.f`) → the
        // per-thread locals seeded in the face kernel main (same scheme as the
        // vertex param). `f.f` (the face index) maps to its own local.
        out += e.lhs->name;
        out += "_";
        out += e.name;
      } else if (e.lhs && e.lhs->kind == ExprKind::Ident &&
                 findNb(stringref(e.lhs->name.c_str()))) {
        // Neighbor-bundle member: read from the CSR-indexed buffers. co
        // comes from the pre-dab snapshot (Jacobi); no stays live.
        const NbBinding *nb = findNb(stringref(e.lhs->name.c_str()));
        if (std::strcmp(e.name.c_str(), "co") == 0) {
          // Non-accumulate neighbors read the base — the WGSL twin of
          // OrigNbrBase::neighborCo. On the disp path that is the Jacobi
          // snapshot minus what the brush put there (untouched → disp == 0).
          if (wantsDisp()) {
            out += "(co_prev["; out += nb->idxVar;
            out += "] - select(vec3<f32>(0.0), sb_disp["; out += nb->idxVar;
            out += "], brush_u.nonaccum != 0u))";
          } else if (wantsOrigCo()) {
            out += "select(co_prev["; out += nb->idxVar; out += "], orig_co[";
            out += nb->idxVar; out += "], brush_u.nonaccum != 0u)";
          } else {
            out += "co_prev["; out += nb->idxVar; out += "]";
          }
        } else if (std::strcmp(e.name.c_str(), "no") == 0) {
          out += "no_buf["; out += nb->idxVar; out += "]";
        } else if (std::strcmp(e.name.c_str(), "v") == 0) {
          out += nb->idxVar;
        } else if (const Field *af = findField(stringref(e.name.c_str()));
                   af && af->kind == FieldKind::Attr) {
          // Neighbor attribute read (e.g. nb.color / nb.vclass): index the
          // bound storage buffer directly at the neighbor's vertex index,
          // matching the CPU lowering (*__attr_<name>)[nb.v].
          out += "attr_";
          out += e.name;
          out += "[";
          out += nb->idxVar;
          out += "]";
        } else {
          errf("neighbor bundle has no member '%s'", e.name.c_str());
          out += "/*bad-neighbor-member*/";
        }
      } else {
        emitExpr(*e.lhs);
        out += ".";
        out += e.name;
      }
      break;
    }
    case ExprKind::Index:
      emitExpr(*e.lhs);
      out += "[";
      emitExpr(*e.rhs);
      out += "]";
      break;
    case ExprKind::Binary:
      out += "(";
      emitExpr(*e.lhs);
      out += " ";
      out += binOpCSym(e.binop);
      out += " ";
      emitExpr(*e.rhs);
      out += ")";
      break;
    case ExprKind::Unary:
      out += "(";
      out += unaryOpCSym(e.unaryop);
      emitExpr(*e.lhs);
      out += ")";
      break;
    case ExprKind::Paren:
      out += "(";
      emitExpr(*e.lhs);
      out += ")";
      break;
    case ExprKind::Call: {
      // float2/3/4 type-constructor calls in the source become WGSL
      // vec*<f32>() constructors. The IR doesn't tag these specially
      // (parser just sees Ident + LParen), so the emitter has to match
      // by name.
      const char *n = e.name.c_str();
      if (std::strcmp(n, "float2") == 0 || std::strcmp(n, "float3") == 0 ||
          std::strcmp(n, "float4") == 0) {
        out += "vec";
        out += n[5];
        out += "<f32>(";
        for (int i = 0; i < (int)e.args.size(); i++) {
          if (i > 0) out += ", ";
          emitExpr(*e.args[i]);
        }
        out += ")";
        break;
      }

      // grad(expr, var) — forward-mode gradient, dual-number rewrite. WGSL has
      // no operator overloads, so binary ops map to sbd_add/sub/mul/div.
      if (std::strcmp(n, "grad") == 0 && e.args.size() == 2) {
        gradUsed = true;
        string savedVar = gradVar; gradVar = render(*e.args[1]);
        out += "("; emitDual(*e.args[0]); out += ").d";
        gradVar = savedVar;
        break;
      }
      // Dotted call `Tex.eval(args)` -> inline texture's eval function.
      if (const TextureDef *td = findTextureCall(stringref(e.name.c_str()))) {
        out += texEvalName(*td);
        out += "(";
        for (int i = 0; i < (int)e.args.size(); i++) {
          if (i > 0) out += ", ";
          emitExpr(*e.args[i]);
        }
        out += ")";
        break;
      }

      const IntrinsicDef *intr = findIntrinsic(stringref(e.name.c_str()));
      if (intr) {
        const char *pat = intr->emit[(int)BackendKind::Wgsl].pattern;
        if (!pat) {
          string msg = string("intrinsic '") + e.name + "' has no WGSL emit pattern";
          errors.append(msg);
          out += "/*missing-wgsl-intrinsic*/";
          break;
        }
        Vector<string> rendered;
        for (const auto &a : e.args) {
          string saved = out;
          out = string("");
          emitExpr(*a);
          rendered.append(out);
          out = saved;
        }
        for (const char *p = pat; *p; ) {
          if (*p == '$' && std::isdigit((unsigned char)p[1])) {
            int idx = p[1] - '0';
            p += 2;
            if (idx < (int)rendered.size()) {
              out += rendered[idx];
            } else {
              out += "/*bad-arg*/";
            }
          } else if (*p == '$' && p[1] == 'v') {
            // Current loop vertex index — `sb_vidx` in a vertex kernel (keys the
            // cavity automask read in brush_strength). Face kernels have no
            // per-vertex index; emit a dummy `0u` (brush_strength ignores it).
            p += 2;
            out += faceMode() ? "0u" : "sb_vidx";
          } else {
            char tmp[2] = {*p, 0};
            out += tmp;
            p++;
          }
        }
      } else {
        // Unknown name — defer to WGSL's resolver and let tint flag it.
        out += e.name;
        out += "(";
        for (int i = 0; i < (int)e.args.size(); i++) {
          if (i > 0) out += ", ";
          emitExpr(*e.args[i]);
        }
        out += ")";
      }
      break;
    }
    }
  }

  // Dual-number rewrite for grad(). Same shape as emit_cpp but WGSL ops are
  // sbd_* functions, not overloaded operators. var seeds the Jacobian; others
  // are zero-deriv constants.
  string render(const Expr &e) { string saved = out; out = string(""); emitExpr(e); string r = out; out = saved; return r; }
  bool isGradVar(const Expr &e) { string r = render(e); return string(r).operator==(string(gradVar.c_str())); }
  void emitDual(const Expr &e)
  {
    if (isGradVar(e)) { out += "sb_seed3("; emitExpr(e); out += ")"; return; }
    switch (e.kind) {
    case ExprKind::LitFloat: case ExprKind::LitInt: out += "sb_c("; emitExpr(e); out += ")"; break;
    case ExprKind::Ident: out += "sb_c3("; emitExpr(e); out += ")"; break;
    case ExprKind::Member:
      if (e.lhs && isGradVar(*e.lhs)) { out += "sb_comp(sb_seed3("; emitExpr(*e.lhs); out += "), "; out += (std::strcmp(e.name.c_str(),"x")==0?"0":std::strcmp(e.name.c_str(),"y")==0?"1":"2"); out += ")"; }
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
      out += "sbd_"; out += n; out += "(";
      for (int i = 0; i < (int)e.args.size(); i++) { if (i) out += ", "; emitDual(*e.args[i]); }
      out += ")"; break;
    }
    default: out += "sb_c(0.0)"; break;
    }
  }
  static bool exprUsesGrad(const Expr *e) { if(!e)return false; if(e->kind==ExprKind::Call&&std::strcmp(e->name.c_str(),"grad")==0)return true; if(exprUsesGrad(e->lhs.get())||exprUsesGrad(e->rhs.get()))return true; for(const auto&a:e->args)if(exprUsesGrad(a.get()))return true; return false; }
  static bool stmtUsesGrad(const Stmt *s) { if(!s)return false; if(exprUsesGrad(s->expr.get())||exprUsesGrad(s->cond.get())||exprUsesGrad(s->lvalue.get())||exprUsesGrad(s->rvalue.get()))return true; for(const auto&c:s->stmts)if(stmtUsesGrad(c.get()))return true; return stmtUsesGrad(s->thenBranch.get())||stmtUsesGrad(s->elseBranch.get())||stmtUsesGrad(s->forInit.get())||stmtUsesGrad(s->forStep.get()); }
  bool brushUsesGrad() const { for(const auto&st:brush->stages)if(stmtUsesGrad(st.body.get()))return true; return false; }

  // === statement emitter ===

  // WGSL has no `continue` outside a loop — and our kernel body is the
  // bare per-vertex code, no enclosing for-loop — so a source `continue;`
  // (i.e. "skip this vertex") lowers to `return;`. That also matches the
  // C++ executor's effect: the post-iteration `affected_verts.append`
  // doesn't run for skipped verts there either.
  void emitStmt(const Stmt &s)
  {
    switch (s.kind) {
    case StmtKind::Block: {
      writeIndent(); out += "{\n";
      indent++;
      int savedLocals = (int)locals.size();
      for (const auto &c : s.stmts) emitStmt(*c);
      while ((int)locals.size() > savedLocals) locals.pop_back();
      indent--;
      writeIndent(); out += "}\n";
      break;
    }
    case StmtKind::DeclLocal:
      writeIndent();
      out += "var ";
      out += s.name;
      out += ": ";
      if (s.declType == TypeKind::Struct) out += s.declStructName;
      else out += wgslType(s.declType);
      if (s.expr) {
        out += " = ";
        emitExpr(*s.expr);
      }
      out += ";\n";
      locals.append(s.name);
      break;
    case StmtKind::Assign:
      writeIndent();
      emitExpr(*s.lvalue);
      out += " ";
      out += assignOpCSym(s.assignOp);
      out += " ";
      emitExpr(*s.rvalue);
      out += ";\n";
      break;
    case StmtKind::If: {
      writeIndent();
      out += "if (";
      emitExpr(*s.cond);
      out += ") ";
      if (s.thenBranch && s.thenBranch->kind == StmtKind::Block) {
        out += "{\n";
        indent++;
        int savedLocals = (int)locals.size();
        for (const auto &c : s.thenBranch->stmts) emitStmt(*c);
        while ((int)locals.size() > savedLocals) locals.pop_back();
        indent--;
        writeIndent(); out += "}";
      } else if (s.thenBranch) {
        out += "{\n";
        indent++;
        emitStmt(*s.thenBranch);
        indent--;
        writeIndent(); out += "}";
      }
      if (s.elseBranch) {
        out += " else ";
        if (s.elseBranch->kind == StmtKind::Block) {
          out += "{\n";
          indent++;
          int savedLocals = (int)locals.size();
          for (const auto &c : s.elseBranch->stmts) emitStmt(*c);
          while ((int)locals.size() > savedLocals) locals.pop_back();
          indent--;
          writeIndent(); out += "}\n";
        } else if (s.elseBranch->kind == StmtKind::If) {
          emitStmt(*s.elseBranch);
        } else {
          out += "{\n";
          indent++;
          emitStmt(*s.elseBranch);
          indent--;
          writeIndent(); out += "}\n";
        }
      } else {
        out += "\n";
      }
      break;
    }
    case StmtKind::For: {
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
        while (n > 0 && frag[n - 1] == '\n') n--;
        if (stripSemi && n > 0 && frag[n - 1] == ';') n--;
        for (int i = 0; i < n; i++) {
          char tmp[2] = {frag[i], 0};
          out += tmp;
        }
      };
      if (s.forInit) renderFrag(*s.forInit, /*stripSemi=*/false);
      out += " ";
      emitExpr(*s.cond);
      out += "; ";
      if (s.forStep) renderFrag(*s.forStep, /*stripSemi=*/true);
      out += ") ";
      if (s.thenBranch && s.thenBranch->kind == StmtKind::Block) {
        out += "{\n";
        indent++;
        int savedLocals = (int)locals.size();
        for (const auto &c : s.thenBranch->stmts) emitStmt(*c);
        while ((int)locals.size() > savedLocals) locals.pop_back();
        indent--;
        writeIndent(); out += "}\n";
      } else if (s.thenBranch) {
        out += "{\n";
        indent++;
        emitStmt(*s.thenBranch);
        indent--;
        writeIndent(); out += "}\n";
      }
      break;
    }
    case StmtKind::Continue:
      writeIndent(); out += "return;\n";
      break;
    case StmtKind::Return:
      writeIndent();
      out += "return";
      if (s.expr) { out += " "; emitExpr(*s.expr); }
      out += ";\n";
      break;
    case StmtKind::ExprStmt:
      writeIndent();
      emitExpr(*s.expr);
      out += ";\n";
      break;
    case StmtKind::NeighborLoop: {
      // for_neighbor (nb in <outer>) { body } — walk the CSR neighbor list
      // for <outer>'s vertex index. vert_nbr_meta[i] = (offset, count) into
      // the flat nbr_verts array. Names are suffixed by nesting depth so a
      // (theoretical) nested for_neighbor doesn't collide.
      int depth = (int)nbStack.size();
      char sfx[16];
      std::snprintf(sfx, sizeof(sfx), "%d", depth);
      string outerIdx = resolveVertIndex(*s.lvalue);
      string metaVar = string("sb_nbr_meta") + sfx;
      string niVar = string("sb_ni") + sfx;
      string idxVar = string("sb_nb_v") + sfx;
      writeIndent(); out += "{\n";
      indent++;
      writeIndent();
      out += "let " + metaVar + " = vert_nbr_meta[" + outerIdx + "];\n";
      writeIndent();
      out += "for (var " + niVar + " = 0u; " + niVar + " < " + metaVar + ".y; " +
             niVar + " = " + niVar + " + 1u) {\n";
      indent++;
      writeIndent();
      out += "let " + idxVar + " = nbr_verts[" + metaVar + ".x + " + niVar + "];\n";
      nbStack.append(NbBinding{s.name, idxVar});
      if (s.thenBranch && s.thenBranch->kind == StmtKind::Block) {
        int savedLocals = (int)locals.size();
        for (const auto &c : s.thenBranch->stmts) emitStmt(*c);
        while ((int)locals.size() > savedLocals) locals.pop_back();
      } else if (s.thenBranch) {
        emitStmt(*s.thenBranch);
      }
      nbStack.pop_back();
      indent--;
      writeIndent(); out += "}\n";
      indent--;
      writeIndent(); out += "}\n";
      break;
    }
    }
  }

  // === top-level emitter ===

  void emitPrelude()
  {
    // Fixed schema — every kernel binds the same layout, so the host can
    // share a single bind-group setup across brushes. Vec3 fields land on
    // their natural 16-byte boundaries in uniform address space; no
    // explicit padding needed for this layout.
    write("// AUTO-GENERATED by sbrushc (WGSL backend) — DO NOT EDIT.\n");
    write("// Source: ");
    write(brush->sourceFile);
    write("\n\n");

    // User-defined struct decls. WGSL allows them at module scope and
    // they're nameable from both reduce and vertex functions.
    for (const auto &sd : brush->structs) {
      write("struct ");
      write(sd.name);
      write(" {\n");
      for (const auto &f : sd.fields) {
        write("  ");
        write(f.name);
        write(": ");
        write(wgslType(f.type));
        write(",\n");
      }
      write("};\n\n");
    }

    // Names that are already members of the fixed BrushUniforms /
    // CtxUniforms blocks. DSL fields with these names re-bind to the
    // existing slot rather than getting re-emitted (and tripping tint).
    auto isBuiltinBrushName = [](const char *n) {
      return std::strcmp(n, "strength") == 0 || std::strcmp(n, "radius") == 0 ||
             std::strcmp(n, "spacing") == 0  || std::strcmp(n, "invert") == 0 ||
             std::strcmp(n, "falloff_kind") == 0 ||
             std::strcmp(n, "falloff_shape") == 0 ||
             std::strcmp(n, "falloff_dir") == 0 ||
             std::strcmp(n, "falloff_extent") == 0 ||
             std::strcmp(n, "coord_space") == 0 ||
             std::strcmp(n, "tex_repeat") == 0 ||
             std::strcmp(n, "stroke_path_count") == 0;
    };
    auto isBuiltinCtxName = [](const char *n) {
      return std::strcmp(n, "surfacePos") == 0 || std::strcmp(n, "surfaceNo") == 0 ||
             std::strcmp(n, "render_matrix") == 0;
    };

    // Lowers a DSL field's type to its WGSL uniform-block spelling.
    // Array<T,N> becomes `array<T, N>`; note that `array<vec3<f32>, N>`
    // has stride 16 in the uniform address space (vec3 is padded), so
    // any future C++ marshaling has to match — deferred until a WGSL
    // dispatcher actually consumes these blocks.
    auto writeFieldType = [&](const Field &f) {
      if (f.type == TypeKind::Array) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d", f.arraySize);
        out += "array<";
        out += wgslType(f.arrayElem);
        out += ", ";
        out += buf;
        out += ">";
      } else {
        out += wgslType(f.type);
      }
    };

    write("struct BrushUniforms {\n");
    write("  strength: f32,\n");
    write("  radius: f32,\n");
    write("  spacing: f32,\n");
    write("  invert: u32,\n");
    // Selector for `brush_falloff` — values must match the C++ enum
    // FalloffKind in brush.h. The C++ side packs this as a `u8`; on the
    // GPU it widens to u32 to keep std140 happy (and the host marshaler,
    // when it lands, must pad to u32 to match).
    write("  falloff_kind: u32,\n");
    // Spatial falloff metric (FalloffShape in brush.h), widened to u32.
    write("  falloff_shape: u32,\n");
    // Non-accumulate stroke flag (plans/nonAccumMode.md). Only accumulable
    // kernels read it; mirrors ComputeBrushUniforms::nonaccum at offset 24.
    write("  nonaccum: u32,\n");
    // Grab-class per-dab generation (grab/kelvinlet first-touch stamps; only
    // @grabmode kernels read it). Mirrors ComputeBrushUniforms::grab_dab_gen
    // at offset 28 — the slot every other kernel treats as pad, so declaring
    // it unconditionally shifts nothing (falloff_dir stays at 32).
    write("  grab_dab_gen: u32,\n");
    // Direction for FalloffShape::Linear / primary axis of FalloffShape::Box.
    // vec3 needs 16-byte alignment in the uniform address space; the host
    // marshaler (ComputeBrushUniforms) must match the padding here.
    write("  falloff_dir: vec3<f32>,\n");
    // Per-axis half-extents for FalloffShape::Box (mirrors Brush::falloff_extent
    // and ComputeBrushUniforms::falloff_extent). std140 pads this vec3 to the
    // next 16-byte slot after falloff_dir.
    write("  falloff_extent: vec3<f32>,\n");
    // Brush-texture UV mapping selector (TexCoordSpace in brush.h), widened
    // to u32, plus the tiling factor for ViewRepeat. `brush_sample_tex`
    // branches on coord_space to match CommandCtx::sampleBrushTex.
    write("  coord_space: u32,\n");
    write("  tex_repeat: f32,\n");
    // Live length of the StrokePath storage buffer for STROKE_CURVED. Mirrors
    // Brush::strokePathCount; the (future) dispatcher writes exactly this many
    // StrokeSample entries into the binding-10 buffer.
    write("  stroke_path_count: u32,\n");
    // Spill brush-uniform fields declared by the DSL into the uniform
    // block so reduce/vertex can reference them. Wave 4 slice keeps the
    // packing trivial — scalars and vec3/vec4 align naturally on 16-byte
    // boundaries in the uniform address space.
    for (const auto &f : brush->fields) {
      if (f.kind != FieldKind::Uniform) continue;
      if (isBuiltinBrushName(f.name.c_str())) continue;
      write("  ");
      write(f.name);
      write(": ");
      writeFieldType(f);
      write(",\n");
    }
    write("};\n\n");

    write("struct CtxUniforms {\n");
    write("  surfacePos: vec3<f32>,\n");
    write("  surfaceNo: vec3<f32>,\n");
    // View/render transform consumed by brush_sample_tex for the ViewPlane
    // and ViewRepeat coord spaces. Mirrors CommandCtxBase::renderMatrix.
    write("  render_matrix: mat4x4<f32>,\n");
    // View-normal automask params (ComputeCtxUniforms offsets 96..124) —
    // brush_view_normal evaluates viewNormalFactor's twin dynamically per dab,
    // so each symmetry image's dispatch carries its own reflected ray. Emitted
    // unconditionally so every kernel's DSL ctx tail starts at offset 128.
    write("  view_dir: vec3<f32>,\n");
    write("  vn_enabled: u32,\n");
    write("  vn_limit: f32,\n");
    write("  vn_falloff: f32,\n");
    write("  vn_cull: u32,\n");
    write("  _vn_pad: u32,\n");
    for (const auto &f : brush->fields) {
      if (f.kind != FieldKind::Ctx) continue;
      if (isBuiltinCtxName(f.name.c_str())) continue;
      write("  ");
      write(f.name);
      write(": ");
      writeFieldType(f);
      write(",\n");
    }
    write("};\n\n");

    // NodeMeta indexes a workgroup's slice of the per-domain unique-element
    // array (verts for the vertex kernel, faces for the face kernel).
    if (faceMode()) {
      write("struct NodeMeta {\n");
      write("  face_offset: u32,\n");
      write("  face_count: u32,\n");
      write("};\n\n");
    } else {
      write("struct NodeMeta {\n");
      write("  vert_offset: u32,\n");
      write("  vert_count: u32,\n");
      write("};\n\n");
    }

    // One StrokePath sample — mirrors Brush::StrokeSample (pos, normal,
    // arclen). Consumed by brush_stroke_uv for STROKE_CURVED texture mapping.
    write("struct StrokeSample {\n");
    write("  pos: vec3<f32>,\n");
    write("  normal: vec3<f32>,\n");
    write("  arclen: f32,\n");
    write("};\n\n");

    // Geometry bindings 0-4 are domain-specific. The face kernel reads
    // precomputed face centroids/normals (faces don't move) + a unique_faces
    // index array; the vertex kernel has read_write co/no/mask. Bindings 5+ are
    // shared so one host bind-group layout covers both.
    if (faceMode()) {
      write("@group(0) @binding(0) var<storage, read>       face_centroid: array<vec3<f32>>;\n");
      write("@group(0) @binding(1) var<storage, read>       face_no: array<vec3<f32>>;\n");
      write("@group(0) @binding(3) var<storage, read>       unique_faces: array<u32>;\n");
      write("@group(0) @binding(4) var<storage, read>       nodes: array<NodeMeta>;\n");
    } else {
      write("@group(0) @binding(0) var<storage, read_write> co_buf: array<vec3<f32>>;\n");
      write("@group(0) @binding(1) var<storage, read_write> no_buf: array<vec3<f32>>;\n");
      write("@group(0) @binding(2) var<storage, read_write> mask_buf: array<f32>;\n");
      write("@group(0) @binding(3) var<storage, read>       unique_verts: array<u32>;\n");
      write("@group(0) @binding(4) var<storage, read>       nodes: array<NodeMeta>;\n");
    }
    write("@group(0) @binding(5) var<uniform>             brush_u: BrushUniforms;\n");
    write("@group(0) @binding(6) var<uniform>             ctx_u: CtxUniforms;\n");
    // Curve LUT for FalloffKind::Curve. Sized to match Brush::falloff_curve
    // (kFalloffCurveSize = 256 in brush.h); the host writes exactly 256 f32s
    // (1024 bytes, contiguous — identical bytes as vec4x64). A uniform, not
    // storage, buffer: neighbor kernels + orig_co already use 10 storage slots
    // (Dawn's per-stage limit), and the uniform address space's 16-byte array
    // stride forces the vec4 shape (sb_lut below recovers scalar indexing).
    // Bake-produced from Brush::falloffCurve via bake_curve_lut, so the
    // LUT-fetch is bit-identical to the C++ Curve-branch interpolation.
    write("@group(0) @binding(7) var<uniform>             falloff_lut: array<vec4<f32>, 64>;\n");
    // Brush texture + sampler. When no texture is bound the host binds a 1x1
    // white texel so `brush_sample_tex` returns 1.0 (matching the C++
    // no-texture path). brush_sample_tex does its own clamp-to-edge bilinear
    // via textureLoad (see below), so brush_samp is currently unused — it is
    // kept for descriptor-layout symmetry with the host's 14-binding superset.
    write("@group(0) @binding(8) var                       brush_tex: texture_2d<f32>;\n");
    write("@group(0) @binding(9) var                       brush_samp: sampler;\n");
    // StrokePath ring buffer for STROKE_CURVED — mirrors Brush::strokePath.
    // Uniform-resident on CPU; a storage buffer here so the length can vary.
    write("@group(0) @binding(10) var<storage, read>      stroke_path: array<StrokeSample>;\n");
    // Neighbor (for_neighbor) bindings — only emitted when the kernel needs
    // them, so non-neighbor brushes keep the 11-binding layout. The host's
    // descriptor set layout is a superset, so a single bind-group setup still
    // works across brushes. co_prev is the pre-dab vertex snapshot (Jacobi);
    // vert_nbr_meta[i] = (offset, count) into the flat nbr_verts CSR array.
    if (usesNeighbors) {
      write("@group(0) @binding(11) var<storage, read>      co_prev: array<vec3<f32>>;\n");
      write("@group(0) @binding(12) var<storage, read>      vert_nbr_meta: array<vec2<u32>>;\n");
      write("@group(0) @binding(13) var<storage, read>      nbr_verts: array<u32>;\n");
    }
    // Custom DSL attribute layers — one read_write storage buffer each, at
    // fixed slots >=14 so they never collide with the neighbor bindings
    // (11-13). The host dispatcher binds these from the kernel's attr manifest
    // in this same declaration order.
    {
      AttrDomain wantDomain = faceMode() ? AttrDomain::Face : AttrDomain::Vertex;
      int slot = 14;
      for (const auto &f : brush->fields) {
        if (f.kind != FieldKind::Attr || f.domain != wantDomain) continue;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", slot++);
        write("@group(0) @binding(");
        write(buf);
        write(") var<storage, read_write> attr_");
        write(f.name);
        write(": array<");
        write(wgslType(f.type));
        write(">;\n");
      }
    }
    // The non-accumulate base source (see wantsDisp / wantsOrigCo): the
    // accumulated displacement for accumulable kernels, the legacy absolute
    // stroke-start snapshot for grab-class ones until M5 moves them over.
    if (wantsDisp()) {
      // sb_-prefixed: `disp` is a natural local name and several kernels
      // already declare one, which would shadow a module-scope buffer.
      write("@group(0) @binding(25) var<storage, read_write> sb_disp: array<vec3<f32>>;\n");
    } else if (wantsOrigCo()) {
      write("@group(0) @binding(22) var<storage, read>      orig_co: array<vec3<f32>>;\n");
    }
    // Grab-class first-touch stamps: one u32 per vertex, compared against
    // brush_u.grab_dab_gen in the write-back — the WGSL twin of
    // grabClaimFirstTouch. Fixed slot 23 = kDabStampBinding; the host zero-
    // fills it at beginStroke (gen 0 never matches, dab gens start at 1).
    if (!faceMode() && brush->isGrabMode) {
      write("@group(0) @binding(23) var<storage, read_write> dab_stamp: array<u32>;\n");
    }
    // Per-vertex cavity automask factor (vertex kernels only), fixed slot 24 =
    // kAutomaskBinding. brush_strength multiplies it in; the host uploads 1.0
    // everywhere when cavity masking is off, so the GPU stays bit-identical to
    // the CPU strength() path.
    if (!faceMode()) {
      write("@group(0) @binding(24) var<storage, read>      automask: array<f32>;\n");
    }
    write("\n");

    // Falloff selector — kept in lockstep with Brush::falloffEval in
    // brush.h. Each branch is the same closed form as its C++ twin;
    // changing one without the other is a regression on the WGSL/CPU
    // bit-equality contract that the upcoming backend A/B framework
    // will rely on.
    // Scalar fetch from the vec4-packed uniform LUT (see binding 7 above).
    write("fn sb_lut(i: i32) -> f32 {\n");
    write("  return falloff_lut[i >> 2][i & 3];\n");
    write("}\n\n");
    write("fn brush_falloff(t: f32) -> f32 {\n");
    write("  if (brush_u.falloff_kind == 1u) {\n");
    write("    return t;\n");
    write("  } else if (brush_u.falloff_kind == 2u) {\n");
    write("    let sb_u = 1.0 - t;\n");
    write("    return exp(-9.0 * sb_u * sb_u);\n");
    write("  } else if (brush_u.falloff_kind == 3u) {\n");
    write("    let sb_c = clamp(t, 0.0, 1.0);\n");
    write("    let sb_s = sb_c * 255.0;\n");
    write("    let sb_i = i32(floor(sb_s));\n");
    write("    if (sb_i >= 255) { return sb_lut(255); }\n");
    write("    let sb_f = sb_s - f32(sb_i);\n");
    write("    return sb_lut(sb_i) * (1.0 - sb_f) + sb_lut(sb_i + 1) * sb_f;\n");
    write("  }\n");
    write("  return t * t * (3.0 - 2.0 * t);\n");
    write("}\n\n");
    // Spatial falloff metric — kept in lockstep with Brush::falloffDist.
    // Values must match the C++ enum FalloffShape in brush.h.
    write("fn brush_falloff_dist(delta: vec3<f32>) -> f32 {\n");
    write("  let sb_inv_r = 1.0 / brush_u.radius;\n");
    write("  if (brush_u.falloff_shape == 1u) {\n");
    write("    let sb_a = abs(delta);\n");
    write("    return max(sb_a.x, max(sb_a.y, sb_a.z)) * sb_inv_r;\n");
    write("  } else if (brush_u.falloff_shape == 2u) {\n");
    write("    return abs(dot(delta, brush_u.falloff_dir)) * sb_inv_r;\n");
    write("  } else if (brush_u.falloff_shape == 3u) {\n");
    // Stroke-aligned oriented cuboid — mirrors Brush::falloffDist's Box case
    // bit-for-bit: axis 0 = stroke tangent projected into the surface tangent
    // plane, axis 1 = in-plane perpendicular, axis 2 = surface normal.
    write("    let sb_n = normalize(ctx_u.surfaceNo);\n");
    write("    var sb_tang = brush_u.falloff_dir - sb_n * dot(brush_u.falloff_dir, sb_n);\n");
    write("    var sb_tl = length(sb_tang);\n");
    write("    if (sb_tl < 1e-6) {\n");
    write("      let sb_ref = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 0.0, 1.0), abs(sb_n.z) < 0.999);\n");
    write("      sb_tang = cross(sb_ref, sb_n);\n");
    write("      sb_tl = length(sb_tang);\n");
    write("    }\n");
    write("    sb_tang = sb_tang / sb_tl;\n");
    write("    let sb_lat = cross(sb_n, sb_tang);\n");
    write("    let sb_dn = abs(dot(delta, sb_tang)) / brush_u.falloff_extent.x;\n");
    write("    let sb_d1 = abs(dot(delta, sb_lat)) / brush_u.falloff_extent.y;\n");
    write("    let sb_d2 = abs(dot(delta, sb_n)) / brush_u.falloff_extent.z;\n");
    write("    return max(sb_dn, max(sb_d1, sb_d2)) * sb_inv_r;\n");
    write("  }\n");
    write("  return length(delta) * sb_inv_r;\n");
    write("}\n\n");
    // Dynamic view-normal automask factor for vertex `vid` — kept in lockstep
    // with automask.h's viewNormalFactor (guards included): 1.0 head-on,
    // ramping to 0 at vn_limit; cull keeps the dot's sign so back faces land
    // past the limit. Reads the live no_buf normal, which on the GPU is
    // stroke-static — matching the CPU's dynamic live-normal evaluation.
    if (!faceMode()) {
      write("fn brush_view_normal(vid: u32) -> f32 {\n");
      write("  if (ctx_u.vn_enabled == 0u) { return 1.0; }\n");
      write("  let sb_no = no_buf[vid];\n");
      write("  let sb_nl = length(sb_no);\n");
      write("  let sb_vl = length(ctx_u.view_dir);\n");
      write("  if (sb_nl <= 1e-9 || sb_vl <= 1e-9) { return 1.0; }\n");
      write("  var sb_d = -dot(sb_no, ctx_u.view_dir) / (sb_nl * sb_vl);\n");
      write("  if (ctx_u.vn_cull == 0u) { sb_d = abs(sb_d); }\n");
      write("  sb_d = clamp(sb_d, -1.0, 1.0);\n");
      write("  let sb_ang = acos(sb_d);\n");
      write("  if (sb_ang >= ctx_u.vn_limit) { return 0.0; }\n");
      write("  if (ctx_u.vn_falloff <= 1e-6) { return 1.0; }\n");
      write("  let sb_ramp = ctx_u.vn_limit - ctx_u.vn_falloff;\n");
      write("  if (sb_ang <= sb_ramp) { return 1.0; }\n");
      write("  return (ctx_u.vn_limit - sb_ang) / ctx_u.vn_falloff;\n");
      write("}\n\n");
    }
    // `vid` is the current vertex index (threaded by the `$v` placeholder in the
    // strength intrinsic). Vertex kernels multiply the per-vertex cavity automask
    // (identity 1.0 when off → bit-identical to the falloff-only strength) and
    // the dynamic view-normal factor; face kernels have neither, so `vid` is
    // unused there. Mirrors CommandCtx::strength in brush_command.h.
    write("fn brush_strength(p: vec3<f32>, vid: u32) -> f32 {\n");
    write("  let sb_t = 1.0 - min(brush_falloff_dist(p - ctx_u.surfacePos), 1.0);\n");
    if (!faceMode()) {
      write("  let sb_s = brush_u.strength * brush_falloff(sb_t) * automask[vid] * "
            "brush_view_normal(vid);\n");
    } else {
      write("  let sb_s = brush_u.strength * brush_falloff(sb_t);\n");
    }
    write("  return select(sb_s, -sb_s, brush_u.invert != 0u);\n");
    write("}\n\n");
    // Brush-texture modulation — kept in lockstep with
    // CommandCtx::sampleBrushTex. `no` is part of the DSL signature but
    // currently unused by the matrix-driven coord spaces; the phony
    // assignment keeps tint from flagging it.
    // Curvilinear stroke UV — kept in lockstep with Brush::sampleStrokeUV.
    // Projects `co` onto the StrokePath polyline: uv.x = arc length at the
    // nearest point, uv.y = lateral distance from the centerline.
    write("fn brush_stroke_uv(co: vec3<f32>) -> vec2<f32> {\n");
    write("  if (brush_u.stroke_path_count == 0u) { return vec2<f32>(0.0, 0.0); }\n");
    write("  if (brush_u.stroke_path_count == 1u) {\n");
    write("    return vec2<f32>(stroke_path[0].arclen, length(co - stroke_path[0].pos));\n");
    write("  }\n");
    write("  var sb_best_dist = 3.402823e+38;\n");
    write("  var sb_best_arc = 0.0;\n");
    write("  var sb_best_lat = 0.0;\n");
    write("  for (var i = 0u; i + 1u < brush_u.stroke_path_count; i = i + 1u) {\n");
    write("    let sb_a = stroke_path[i].pos;\n");
    write("    let sb_ab = stroke_path[i + 1u].pos - sb_a;\n");
    write("    let sb_len2 = dot(sb_ab, sb_ab);\n");
    write("    var sb_t = 0.0;\n");
    write("    if (sb_len2 > 0.0) { sb_t = dot(co - sb_a, sb_ab) / sb_len2; }\n");
    write("    sb_t = clamp(sb_t, 0.0, 1.0);\n");
    write("    let sb_d = length(co - (sb_a + sb_ab * sb_t));\n");
    write("    if (sb_d < sb_best_dist) {\n");
    write("      sb_best_dist = sb_d;\n");
    write("      sb_best_arc = stroke_path[i].arclen + (stroke_path[i + 1u].arclen - stroke_path[i].arclen) * sb_t;\n");
    write("      sb_best_lat = sb_d;\n");
    write("    }\n");
    write("  }\n");
    write("  return vec2<f32>(sb_best_arc, sb_best_lat);\n");
    write("}\n\n");
    // View-pinned UV — perspective divide + NDC -> [0,1] remap, lockstep
    // with CommandCtx::sampleViewUv.
    write("fn brush_view_uv(co: vec3<f32>) -> vec2<f32> {\n");
    write("  let sb_p = ctx_u.render_matrix * vec4<f32>(co, 1.0);\n");
    write("  var sb_w = sb_p.w;\n");
    write("  if (abs(sb_w) <= 1e-6) { sb_w = 1.0; }\n");
    write("  return sb_p.xy / sb_w * 0.5 + vec2<f32>(0.5, 0.5);\n");
    write("}\n\n");
    write("fn brush_sample_tex(co: vec3<f32>, no: vec3<f32>) -> f32 {\n");
    write("  _ = no;\n");
    write("  var sb_uv: vec2<f32>;\n");
    write("  if (brush_u.coord_space == 1u) {\n");
    write("    sb_uv = brush_view_uv(co);\n");
    write("  } else if (brush_u.coord_space == 2u) {\n");
    write("    sb_uv = brush_view_uv(co) * brush_u.tex_repeat;\n");
    write("  } else if (brush_u.coord_space == 3u) {\n");
    write("    sb_uv = brush_stroke_uv(co);\n");
    write("  } else if (brush_u.coord_space == 4u) {\n");
    // PROJECTED: tangent-plane projection at the brush center, normalized so
    // the tile spans the brush diameter. Mirrors CommandCtx::sampleBrushTex —
    // same reference-axis flip on |n.z| < 0.999 so the orthonormal basis is
    // identical to the C++ path within tolerance.
    write("    let sb_n = normalize(ctx_u.surfaceNo);\n");
    write("    var sb_ref = vec3<f32>(0.0, 0.0, 1.0);\n");
    write("    if (abs(sb_n.z) >= 0.999) { sb_ref = vec3<f32>(1.0, 0.0, 0.0); }\n");
    write("    let sb_t1 = normalize(cross(sb_ref, sb_n));\n");
    write("    let sb_t2 = cross(sb_n, sb_t1);\n");
    write("    let sb_rel = co - ctx_u.surfacePos;\n");
    write("    var sb_inv_d = 1.0;\n");
    write("    if (brush_u.radius > 1e-6) { sb_inv_d = 1.0 / (2.0 * brush_u.radius); }\n");
    write("    sb_uv = vec2<f32>(dot(sb_rel, sb_t1), dot(sb_rel, sb_t2)) * sb_inv_d + vec2<f32>(0.5, 0.5);\n");
    write("  } else {\n");
    write("    sb_uv = co.xy;\n");
    write("  }\n");
    // Manual clamp-to-edge bilinear with a half-texel offset, lockstep with
    // Brush::sampleTexBilinear. textureLoad fetches exact texels (NEAREST), so
    // the float blend below — not a hardware sampler's reduced-precision
    // subtexel weights — decides the result; that is what keeps the GPU value
    // bit-modulo-fp identical to the CPU path. The 1x1 white dummy still yields
    // 1.0 when no texture is bound.
    write("  let sb_dim = vec2<f32>(textureDimensions(brush_tex));\n");
    write("  let sb_fx = sb_uv.x * sb_dim.x - 0.5;\n");
    write("  let sb_fy = sb_uv.y * sb_dim.y - 0.5;\n");
    write("  let sb_x0 = floor(sb_fx);\n");
    write("  let sb_y0 = floor(sb_fy);\n");
    write("  let sb_tx = sb_fx - sb_x0;\n");
    write("  let sb_ty = sb_fy - sb_y0;\n");
    write("  let sb_w = i32(sb_dim.x);\n");
    write("  let sb_h = i32(sb_dim.y);\n");
    write("  let sb_x0c = clamp(i32(sb_x0), 0, sb_w - 1);\n");
    write("  let sb_y0c = clamp(i32(sb_y0), 0, sb_h - 1);\n");
    write("  let sb_x1c = clamp(i32(sb_x0) + 1, 0, sb_w - 1);\n");
    write("  let sb_y1c = clamp(i32(sb_y0) + 1, 0, sb_h - 1);\n");
    write("  let sb_p00 = textureLoad(brush_tex, vec2<i32>(sb_x0c, sb_y0c), 0).r;\n");
    write("  let sb_p10 = textureLoad(brush_tex, vec2<i32>(sb_x1c, sb_y0c), 0).r;\n");
    write("  let sb_p01 = textureLoad(brush_tex, vec2<i32>(sb_x0c, sb_y1c), 0).r;\n");
    write("  let sb_p11 = textureLoad(brush_tex, vec2<i32>(sb_x1c, sb_y1c), 0).r;\n");
    write("  let sb_a = sb_p00 * (1.0 - sb_tx) + sb_p10 * sb_tx;\n");
    write("  let sb_b = sb_p01 * (1.0 - sb_tx) + sb_p11 * sb_tx;\n");
    write("  return sb_a * (1.0 - sb_ty) + sb_b * sb_ty;\n");
    write("}\n\n");
  }

  void emitSkipStub(const char *reason)
  {
    write("// AUTO-GENERATED by sbrushc (WGSL backend) — DO NOT EDIT.\n");
    write("// Source: ");
    write(brush->sourceFile);
    write("\n");
    write("// SKIPPED: ");
    write(reason);
    write("\n\n");
    // Tint refuses an entirely empty module; this is the minimum valid
    // WGSL so the build's `tint <file>` step still passes.
    write("@compute @workgroup_size(1) fn nop() {}\n");
  }

  // Emit one reduce stage as a WGSL function. out/inout params (struct
  // or scalar) become `ptr<function, T>` so the callee can write back;
  // `in` params pass by value. The body emitter dereferences ptr params
  // automatically — see isOutPtrParam.
  // Emit one inline texture's eval as a pure WGSL function — sees only
  // its params and intrinsics, no uniforms/ctx.
  void emitTextureFn(const TextureDef &td)
  {
    write("fn ");
    write(texEvalName(td));
    write("(");
    bool first = true;
    for (const auto &p : td.params) {
      if (!first) write(", ");
      first = false;
      write(p.name);
      write(": ");
      write(wgslType(p.type));
    }
    write(") -> ");
    write(wgslType(td.returnType));
    write(" {\n");
    indent = 1;
    Stage scratch;
    scratch.kind = StageKind::Reduce;
    for (const auto &p : td.params) scratch.params.append(p);
    currentStage = &scratch;
    if (td.body && td.body->kind == StmtKind::Block) {
      int savedLocals = (int)locals.size();
      for (const auto &c : td.body->stmts) emitStmt(*c);
      while ((int)locals.size() > savedLocals) locals.pop_back();
    }
    currentStage = nullptr;
    indent = 0;
    write("}\n\n");
  }

  void emitReduceStage(const Stage &st)
  {
    write("fn ");
    write(st.name);
    write("(");
    bool first = true;
    for (const auto &p : st.params) {
      if (!first) write(", ");
      first = false;
      const char *typeSpelling = (p.type == TypeKind::Struct)
                                     ? p.structName.c_str()
                                     : wgslType(p.type);
      if (p.dir == ParamDir::Out || p.dir == ParamDir::InOut) {
        write(p.name);
        write(": ptr<function, ");
        write(typeSpelling);
        write(">");
      } else {
        write(p.name);
        write(": ");
        write(typeSpelling);
      }
    }
    write(") {\n");
    indent = 1;
    currentStage = &st;
    if (st.body && st.body->kind == StmtKind::Block) {
      int savedLocals = (int)locals.size();
      for (const auto &c : st.body->stmts) emitStmt(*c);
      while ((int)locals.size() > savedLocals) locals.pop_back();
    }
    currentStage = nullptr;
    indent = 0;
    write("}\n\n");
  }

  // Per-face compute kernel (Wave 1b) — the GPU twin of emit_cpp's
  // emitFaceKernel. One workgroup per node, threads strided over the node's
  // covered faces (unique_faces[face_offset + lid]). Faces don't move, so the
  // centroid/normal are read-only precomputed buffers; only the declared face
  // attrs (e.g. polygroup `group`) are written back. Reduce/host/neighbor on a
  // face stage are unsupported (poly-group needs none).
  void emitFaceKernel()
  {
    if (faceStage->params.size() < 1) {
      err("face stage must take at least one parameter (the Face bundle)");
    }
    emitPrelude();
    for (const auto &td : brush->textures) {
      emitTextureFn(td);
    }

    write("@compute @workgroup_size(64)\n");
    write("fn main(\n");
    write("    @builtin(local_invocation_index) lid: u32,\n");
    write("    @builtin(workgroup_id) gid: vec3<u32>)\n");
    write("{\n");
    write("  let sb_node = nodes[gid.x];\n");
    write("  if (lid >= sb_node.face_count) { return; }\n");
    write("  let sb_fidx = unique_faces[sb_node.face_offset + lid];\n");
    write("  var "); write(faceParamName); write("_center: vec3<f32> = face_centroid[sb_fidx];\n");
    write("  var "); write(faceParamName); write("_no: vec3<f32> = face_no[sb_fidx];\n");
    write("  var "); write(faceParamName); write("_f: i32 = i32(sb_fidx);\n");
    // Seed a mutable local per face attr from its storage buffer; member access
    // (f.<attr>) routes to <param>_<attr>, written back after the body.
    for (const auto &f : brush->fields) {
      if (f.kind != FieldKind::Attr || f.domain != AttrDomain::Face) continue;
      write("  var "); write(faceParamName); write("_"); write(f.name);
      write(": "); write(wgslType(f.type));
      write(" = attr_"); write(f.name); write("[sb_fidx];\n");
    }
    write("\n");

    indent = 1;
    currentStage = faceStage;
    if (faceStage->body && faceStage->body->kind == StmtKind::Block) {
      int savedLocals = (int)locals.size();
      for (const auto &c : faceStage->body->stmts) emitStmt(*c);
      while ((int)locals.size() > savedLocals) locals.pop_back();
    }
    currentStage = nullptr;
    indent = 0;
    write("\n");

    // Write each declared face attr back unconditionally. This is correct even
    // when the kernel body only conditionally assigns it (e.g. polygroup's
    // `if (strength>0)`): the local was seeded from the buffer at entry, so an
    // unwritten attr round-trips its original value rather than being clobbered.
    for (const auto &f : brush->fields) {
      if (f.kind != FieldKind::Attr || f.domain != AttrDomain::Face) continue;
      write("  attr_"); write(f.name); write("[sb_fidx] = ");
      write(faceParamName); write("_"); write(f.name); write(";\n");
    }
    write("}\n");
  }

  void run()
  {
    if (!vertexStage) {
      if (faceStage) {
        emitFaceKernel();
        return;
      }
      // No vertex or face stage — emit a valid stub so tint validation passes
      // (the CPU executor runs these brushes).
      emitSkipStub("non-vertex/face stage: GPU dispatch not yet implemented");
      return;
    }
    if (vertexStage->params.size() < 1) {
      err("vertex stage must take at least one parameter (the Vertex bundle)");
    }

    usesNeighbors = hasNeighborLoop(vertexStage->body.get());

    emitPrelude();

    // Forward-mode dual prelude — scalar (v, ∂v) and float3 (v + 3-col
    // Jacobian); chain rules drive grad()'s rewrite. WGSL needs sbd_* fns.
    if (brushUsesGrad()) {
      write("struct sbdual { v: f32, d: vec3<f32> };\n");
      write("struct sbdual3 { v: vec3<f32>, dx: vec3<f32>, dy: vec3<f32>, dz: vec3<f32> };\n");
      write("fn sb_c(x: f32) -> sbdual { return sbdual(x, vec3<f32>(0.0)); }\n");
      write("fn sb_c3(p: vec3<f32>) -> sbdual3 { return sbdual3(p, vec3<f32>(0.0), vec3<f32>(0.0), vec3<f32>(0.0)); }\n");
      write("fn sb_seed3(p: vec3<f32>) -> sbdual3 { return sbdual3(p, vec3<f32>(1.0,0.0,0.0), vec3<f32>(0.0,1.0,0.0), vec3<f32>(0.0,0.0,1.0)); }\n");
      write("fn sb_comp(a: sbdual3, i: i32) -> sbdual { return sbdual(a.v[i], vec3<f32>(a.dx[i], a.dy[i], a.dz[i])); }\n");
      write("fn sb_v3(x: sbdual, y: sbdual, z: sbdual) -> sbdual3 { return sbdual3(vec3<f32>(x.v,y.v,z.v), vec3<f32>(x.d[0],y.d[0],z.d[0]), vec3<f32>(x.d[1],y.d[1],z.d[1]), vec3<f32>(x.d[2],y.d[2],z.d[2])); }\n");
      write("fn sbd_add(a: sbdual, b: sbdual) -> sbdual { return sbdual(a.v+b.v, a.d+b.d); }\n");
      write("fn sbd_sub(a: sbdual, b: sbdual) -> sbdual { return sbdual(a.v-b.v, a.d-b.d); }\n");
      write("fn sbd_neg(a: sbdual) -> sbdual { return sbdual(-a.v, -a.d); }\n");
      write("fn sbd_mul(a: sbdual, b: sbdual) -> sbdual { return sbdual(a.v*b.v, a.d*b.v + b.d*a.v); }\n");
      write("fn sbd_div(a: sbdual, b: sbdual) -> sbdual { return sbdual(a.v/b.v, (a.d*b.v - b.d*a.v)/(b.v*b.v)); }\n");
      write("fn sbd_sin(a: sbdual) -> sbdual { return sbdual(sin(a.v), a.d*cos(a.v)); }\n");
      write("fn sbd_cos(a: sbdual) -> sbdual { return sbdual(cos(a.v), a.d*(-sin(a.v))); }\n");
      write("fn sbd_sqrt(a: sbdual) -> sbdual { let r = sqrt(a.v); return sbdual(r, select(vec3<f32>(0.0), a.d*(0.5/r), r>0.0)); }\n");
      write("fn sbd_abs(a: sbdual) -> sbdual { return sbdual(abs(a.v), a.d*select(1.0,-1.0,a.v<0.0)); }\n");
      write("fn sbd_dot(a: sbdual3, b: sbdual3) -> sbdual { return sbdual(dot(a.v,b.v), a.dx*b.v.x+b.dx*a.v.x+a.dy*b.v.y+b.dy*a.v.y+a.dz*b.v.z+b.dz*a.v.z); }\n");
      write("fn sbd_length(a: sbdual3) -> sbdual { return sbd_sqrt(sbd_dot(a,a)); }\n");
      write("fn sbd_mix(a: sbdual, b: sbdual, t: sbdual) -> sbdual { return sbd_add(a, sbd_mul(sbd_sub(b,a), t)); }\n\n");
    }

    // Inline texture eval functions — pure, module scope, before the
    // stages that call them.
    for (const auto &td : brush->textures) {
      emitTextureFn(td);
    }

    // Reduce stages — WGSL has no templates, so we just name-mangle by
    // the DSL stage name (which is brush-local in practice).
    Vector<const Stage *> reduceStages;
    for (const auto &st : brush->stages) {
      if (st.kind == StageKind::Reduce) reduceStages.append(&st);
    }
    for (const auto *st : reduceStages) {
      emitReduceStage(*st);
    }

    // Per-thread vertex kernel. Workgroup size 64 is a reasonable WebGPU
    // default; the host dispatches ceil(node.vert_count / 64) workgroups
    // per node. The conventional shape is one workgroup per node, with
    // node-internal threads strided — using gid.x to look up the node
    // entry is fine for tint validation and keeps the kernel readable.
    write("@compute @workgroup_size(64)\n");
    write("fn main(\n");
    write("    @builtin(local_invocation_index) lid: u32,\n");
    write("    @builtin(workgroup_id) gid: vec3<u32>)\n");
    write("{\n");
    write("  let sb_node = nodes[gid.x];\n");
    write("  if (lid >= sb_node.vert_count) { return; }\n");
    write("  let sb_vidx = unique_verts[sb_node.vert_offset + lid];\n");
    // The base this dab measures from — the WGSL twin of CoProxy::base
    // (accum_mode.h), derived rather than stored. In accumulate mode it
    // collapses to the live position, so one seed covers both modes.
    if (wantsDisp()) {
      write("  let sb_base: vec3<f32> = co_buf[sb_vidx] - select(vec3<f32>(0.0), "
            "sb_disp[sb_vidx], brush_u.nonaccum != 0u);\n");
    }
    write("  var ");
    if (brush->isGrabMode) {
      // Grab-class kernels always deform from the stroke-start position — the
      // WGSL twin of CoProxy<AccumOrigGrab>'s reads_base (accum_mode.h).
      write(vertexParamName); write("_co: vec3<f32> = orig_co[sb_vidx];\n");
    } else if (wantsDisp()) {
      write(vertexParamName); write("_co: vec3<f32> = sb_base;\n");
    } else {
      write(vertexParamName); write("_co: vec3<f32> = co_buf[sb_vidx];\n");
    }
    write("  var ");
    write(vertexParamName); write("_no: vec3<f32> = no_buf[sb_vidx];\n");
    write("  var ");
    write(vertexParamName); write("_mask: f32 = mask_buf[sb_vidx];\n");

    // Seed a mutable local for each vertex attribute from its storage buffer;
    // member access (v.<attr>) routes to these (<param>_<attr>), and they're
    // written back after the body.
    for (const auto &f : brush->fields) {
      if (f.kind != FieldKind::Attr || f.domain != AttrDomain::Vertex) continue;
      write("  var ");
      write(vertexParamName); write("_"); write(f.name);
      write(": ");
      write(wgslType(f.type));
      write(" = attr_"); write(f.name); write("[sb_vidx];\n");
    }

    // Declare locals for the vertex stage's extra params (struct or
    // scalar) and call each reduce stage on them. The naive per-thread
    // reduce matches the C++ executor's one-per-node call: both pay
    // O(stages*params) ops up-front before the per-vertex code runs.
    for (int pi = 1; pi < (int)vertexStage->params.size(); pi++) {
      const auto &p = vertexStage->params[pi];
      write("  var ");
      write(p.name);
      write(": ");
      if (p.type == TypeKind::Struct) write(p.structName);
      else write(wgslType(p.type));
      write(";\n");
    }
    for (const auto *st : reduceStages) {
      write("  ");
      write(st->name);
      write("(");
      bool first = true;
      for (const auto &rp : st->params) {
        if (!first) write(", ");
        first = false;
        // Match by name+type to the vertex-stage local declared above.
        bool found = false;
        for (int pi = 1; pi < (int)vertexStage->params.size(); pi++) {
          const auto &vp = vertexStage->params[pi];
          if (vp.type != rp.type) continue;
          if (!string(vp.name).operator==(string(rp.name.c_str()))) continue;
          if (rp.type == TypeKind::Struct &&
              !string(vp.structName).operator==(string(rp.structName.c_str()))) continue;
          if (rp.dir == ParamDir::Out || rp.dir == ParamDir::InOut) {
            out += "&";
          }
          write(vp.name);
          found = true;
          break;
        }
        if (!found) {
          errf("reduce param '%s' has no matching vertex-stage local of the same type",
               rp.name.c_str());
          write("/*unmatched*/");
        }
      }
      write(");\n");
    }
    write("\n");

    indent = 1;
    currentStage = vertexStage;
    if (vertexStage->body && vertexStage->body->kind == StmtKind::Block) {
      int savedLocals = (int)locals.size();
      for (const auto &c : vertexStage->body->stmts) emitStmt(*c);
      while ((int)locals.size() > savedLocals) locals.pop_back();
    }
    currentStage = nullptr;
    indent = 0;

    write("\n");
    // Grab-class write-back — the WGSL twin of CoProxy<AccumOrigGrab>::commit
    // + grabClaimFirstTouch (accum_mode.h): the first image to write a vert
    // this dab re-bases it absolutely from orig (follows the cursor); later
    // images of the same dab add their displacement-from-orig, so shared seam
    // verts sum and mirror-only verts never accumulate across dabs.
    if (brush->isGrabMode) {
      write("  if (dab_stamp[sb_vidx] != brush_u.grab_dab_gen) {\n");
      write("    dab_stamp[sb_vidx] = brush_u.grab_dab_gen;\n");
      write("  } else {\n");
      write("    "); write(vertexParamName);
      write("_co = co_buf[sb_vidx] + ("); write(vertexParamName);
      write("_co - orig_co[sb_vidx]);\n");
      write("  }\n");
    }
    if (wantsDisp()) {
      // Non-accumulate write-back — the WGSL twin of CoProxy<AccumOrig>::commit
      // (accum_mode.h). The same delta lands on co and on disp, so the derived
      // base holds still for the next dab; accumulate mode leaves disp alone.
      write("  let sb_delta = "); write(vertexParamName); write("_co - sb_base;\n");
      write("  if (brush_u.nonaccum != 0u) {\n");
      write("    co_buf[sb_vidx] = co_buf[sb_vidx] + sb_delta;\n");
      write("    sb_disp[sb_vidx] = sb_disp[sb_vidx] + sb_delta;\n");
      write("  } else {\n");
      write("    co_buf[sb_vidx] = "); write(vertexParamName); write("_co;\n");
      write("  }\n");
    } else {
      write("  co_buf[sb_vidx] = ");
      write(vertexParamName); write("_co;\n");
    }
    write("  no_buf[sb_vidx] = ");
    write(vertexParamName); write("_no;\n");
    write("  mask_buf[sb_vidx] = ");
    write(vertexParamName); write("_mask;\n");
    for (const auto &f : brush->fields) {
      if (f.kind != FieldKind::Attr || f.domain != AttrDomain::Vertex) continue;
      write("  attr_"); write(f.name); write("[sb_vidx] = ");
      write(vertexParamName); write("_"); write(f.name); write(";\n");
    }
    write("}\n");
  }
};

} // namespace

EmitResult emitWgsl(const Brush &brush)
{
  Emit em;
  em.brush = &brush;
  for (const auto &st : brush.stages) {
    if (st.kind == StageKind::Vertex) { em.vertexStage = &st; break; }
  }
  for (const auto &st : brush.stages) {
    if (st.kind == StageKind::Face) { em.faceStage = &st; break; }
  }
  if (em.vertexStage && em.vertexStage->params.size() > 0) {
    em.vertexParamName = em.vertexStage->params[0].name;
  } else {
    em.vertexParamName = string("v");
  }
  if (em.faceStage && em.faceStage->params.size() > 0) {
    em.faceParamName = em.faceStage->params[0].name;
  } else {
    em.faceParamName = string("f");
  }
  em.run();
  EmitResult r;
  r.text = std::move(em.out);
  r.errors = std::move(em.errors);
  return r;
}

} // namespace sculptcore::brush::sbrush
