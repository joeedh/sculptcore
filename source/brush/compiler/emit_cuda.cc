#include "emit_cuda.h"
#include "../kernels/ir/intrinsics.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace sculptcore::brush::sbrush {

using litestl::util::string;
using litestl::util::stringref;
using litestl::util::Vector;

namespace {

// CUDA/HIP spelling for primitive types. The device prelude defines the
// float2/3/4 structs (no vector builtins under -nogpuinc), so these names
// resolve there. Anything unknown lowers to `float` to keep the syntactic
// gate moving — typed parsing rejects truly bogus input upstream.
const char *cudaType(TypeKind k)
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
  BackendKind target = BackendKind::Cuda;
  const Stage *vertexStage = nullptr;
  string vertexParamName;  // e.g. "v"

  const Stage *currentStage = nullptr;

  string out;
  Vector<string> errors;
  int indent = 0;

  Vector<string> locals;

  // True when the vertex stage uses for_neighbor — gates the extra
  // co_prev / neighbor-CSR globals and the NeighborLoop lowering.
  bool usesNeighbors = false;
  bool gradUsed = false;
  string gradVar;

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
  // around — the vertex param (sb_vidx) or an enclosing neighbor bundle.
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

  // Mangled device name for an inline texture's eval function.
  static string texEvalName(const TextureDef &td)
  {
    return string("tex_") + lower(td.name) + "_eval";
  }

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
    if (!vertexStage) return false;
    if (vertexStage->params.size() == 0) return false;
    const auto &p = vertexStage->params[0];
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
      out += "f";  // single-precision device literal
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
      // Reduce out/inout params lower to C++ references, so an identifier
      // reference is just the name (no deref, unlike the WGSL ptr form).
      if (isLocal(nm) || isStageParam(nm)) {
        out += e.name;
      } else if (auto *f = findField(nm)) {
        out += (f->kind == FieldKind::Uniform) ? "brush_u." : "ctx_u.";
        out += e.name;
      } else {
        out += e.name;
      }
      break;
    }
    case ExprKind::Member: {
      // Vertex-param member access (`v.co`, `v.no`, `v.mask`) targets the
      // per-thread mutable locals seeded from the storage loads.
      if (e.lhs && e.lhs->kind == ExprKind::Ident && isVertexParam(stringref(e.lhs->name.c_str()))) {
        out += e.lhs->name;
        out += "_";
        out += e.name;
      } else if (e.lhs && e.lhs->kind == ExprKind::Ident &&
                 findNb(stringref(e.lhs->name.c_str()))) {
        // Neighbor-bundle member: read from the CSR-indexed buffers. co
        // comes from the pre-dab snapshot (Jacobi); no stays live.
        const NbBinding *nb = findNb(stringref(e.lhs->name.c_str()));
        if (std::strcmp(e.name.c_str(), "co") == 0) {
          out += "co_prev["; out += nb->idxVar; out += "]";
        } else if (std::strcmp(e.name.c_str(), "no") == 0) {
          out += "no_buf["; out += nb->idxVar; out += "]";
        } else if (std::strcmp(e.name.c_str(), "v") == 0) {
          out += nb->idxVar;
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
      // grad(expr, var) — forward-mode gradient; rewrite the arg inline into
      // dual form and read back `.d`. CUDA has overloaded sbdual operators.
      if (std::strcmp(e.name.c_str(), "grad") == 0 && e.args.size() == 2) {
        gradUsed = true;
        string savedVar = gradVar; gradVar = render(*e.args[1]);
        out += "("; emitDual(*e.args[0]); out += ").d";
        gradVar = savedVar;
        break;
      }
      // float2/3/4 type-constructor calls -> sb_make_float* prelude helpers
      // (the prelude has no implicit aggregate constructors).
      const char *n = e.name.c_str();
      if (std::strcmp(n, "float2") == 0 || std::strcmp(n, "float3") == 0 ||
          std::strcmp(n, "float4") == 0) {
        out += "sb_make_float";
        out += n[5];
        out += "(";
        for (int i = 0; i < (int)e.args.size(); i++) {
          if (i > 0) out += ", ";
          emitExpr(*e.args[i]);
        }
        out += ")";
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
        const char *pat = intr->emit[(int)target].pattern;
        if (!pat) {
          string msg = string("intrinsic '") + e.name + "' has no CUDA/HIP emit pattern";
          errors.append(msg);
          out += "/*missing-gpu-intrinsic*/";
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
          } else {
            char tmp[2] = {*p, 0};
            out += tmp;
            p++;
          }
        }
      } else {
        // Unknown name — emit verbatim and let the device compiler flag it.
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

  // grad dual rewrite — CUDA prelude provides overloaded sbdual operators, so
  // binary/unary emit verbatim; sbd_* use device math + sc_*/float3 helpers.
  string render(const Expr &e) { string s = out; out = string(""); emitExpr(e); string r = out; out = s; return r; }
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
    case ExprKind::Binary: out += "("; emitDual(*e.lhs); out += " "; out += binOpCSym(e.binop); out += " "; emitDual(*e.rhs); out += ")"; break;
    case ExprKind::Unary: out += "("; out += unaryOpCSym(e.unaryop); emitDual(*e.lhs); out += ")"; break;
    case ExprKind::Call: {
      const char *n = e.name.c_str();
      if (std::strcmp(n,"float3")==0) { out += "sb_v3("; for (int i=0;i<3;i++){if(i)out+=", ";emitDual(*e.args[i]);} out += ")"; break; }
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

  // The kernel body is the bare per-vertex code (no enclosing loop), so a
  // source `continue;` ("skip this vertex") lowers to `return;` — the
  // writeback at the kernel tail is then skipped and the buffer keeps its
  // unmodified value, matching the C++/WGSL executors.
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
      if (s.declType == TypeKind::Struct) out += s.declStructName;
      else out += cudaType(s.declType);
      out += " ";
      out += s.name;
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
      // the flat nbr_verts array. Names are suffixed by nesting depth.
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
      out += "sb_uint2 " + metaVar + " = vert_nbr_meta[" + outerIdx + "];\n";
      writeIndent();
      out += "for (unsigned int " + niVar + " = 0u; " + niVar + " < " + metaVar + ".y; " +
             niVar + " = " + niVar + " + 1u) {\n";
      indent++;
      writeIndent();
      out += "unsigned int " + idxVar + " = nbr_verts[" + metaVar + ".x + " + niVar + "];\n";
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
    const bool cuda = target == BackendKind::Cuda;
    write("// AUTO-GENERATED by sbrushc (");
    write(cuda ? "CUDA" : "HIP");
    write(" backend) — DO NOT EDIT.\n");
    write("// Source: ");
    write(brush->sourceFile);
    write("\n\n");

    // Self-contained attribute spellings so the file compiles under
    // `clang -x cuda/hip -nogpuinc` (which doesn't pull in the runtime
    // wrapper that normally defines these), and is inert under a full
    // toolchain that already defines them.
    write("#ifndef __device__\n#define __device__ __attribute__((device))\n#endif\n");
    write("#ifndef __global__\n#define __global__ __attribute__((global))\n#endif\n");
    write("#ifndef __forceinline__\n#define __forceinline__ __attribute__((always_inline)) inline\n#endif\n\n");

    // Block/thread index — the only per-target difference in the lowering.
    if (cuda) {
      write("#define SB_NODE_IDX  (__nvvm_read_ptx_sreg_ctaid_x())\n");
      write("#define SB_LOCAL_IDX (__nvvm_read_ptx_sreg_tid_x())\n\n");
    } else {
      write("#define SB_NODE_IDX  (__builtin_amdgcn_workgroup_id_x())\n");
      write("#define SB_LOCAL_IDX (__builtin_amdgcn_workitem_id_x())\n\n");
    }

    // Scalar device math — declared only (the syntactic gate emits PTX/GCN
    // and never links, so the library definitions aren't needed here).
    write("extern \"C\" __device__ float sinf(float);\n");
    write("extern \"C\" __device__ float cosf(float);\n");
    write("extern \"C\" __device__ float floorf(float);\n");
    write("extern \"C\" __device__ float sqrtf(float);\n");
    write("extern \"C\" __device__ float fabsf(float);\n");
    write("extern \"C\" __device__ float fminf(float, float);\n");
    write("extern \"C\" __device__ float fmaxf(float, float);\n");
    write("extern \"C\" __device__ float expf(float);\n\n");
    write("__device__ __forceinline__ float sb_clampf(float x, float lo, float hi) {\n");
    write("  return fminf(fmaxf(x, lo), hi);\n");
    write("}\n\n");

    // Vector types + algebra (no vector builtins under -nogpuinc).
    write("struct float2 { float x, y; };\n");
    write("struct float3 { float x, y, z; };\n");
    write("struct float4 { float x, y, z, w; };\n\n");
    write("__device__ __forceinline__ float2 sb_make_float2(float x, float y) { float2 r; r.x=x; r.y=y; return r; }\n");
    write("__device__ __forceinline__ float3 sb_make_float3(float x, float y, float z) { float3 r; r.x=x; r.y=y; r.z=z; return r; }\n");
    write("__device__ __forceinline__ float4 sb_make_float4(float x, float y, float z, float w) { float4 r; r.x=x; r.y=y; r.z=z; r.w=w; return r; }\n\n");
    write("__device__ __forceinline__ float3 operator+(float3 a, float3 b) { return sb_make_float3(a.x+b.x, a.y+b.y, a.z+b.z); }\n");
    write("__device__ __forceinline__ float3 operator-(float3 a, float3 b) { return sb_make_float3(a.x-b.x, a.y-b.y, a.z-b.z); }\n");
    write("__device__ __forceinline__ float3 operator-(float3 a) { return sb_make_float3(-a.x, -a.y, -a.z); }\n");
    write("__device__ __forceinline__ float3 operator*(float3 a, float s) { return sb_make_float3(a.x*s, a.y*s, a.z*s); }\n");
    write("__device__ __forceinline__ float3 operator*(float s, float3 a) { return sb_make_float3(a.x*s, a.y*s, a.z*s); }\n");
    write("__device__ __forceinline__ float3 operator/(float3 a, float s) { return sb_make_float3(a.x/s, a.y/s, a.z/s); }\n");
    write("__device__ __forceinline__ float3 &operator+=(float3 &a, float3 b) { a = a + b; return a; }\n");
    write("__device__ __forceinline__ float3 &operator-=(float3 &a, float3 b) { a = a - b; return a; }\n");
    write("__device__ __forceinline__ float3 &operator*=(float3 &a, float s) { a = a * s; return a; }\n");
    write("__device__ __forceinline__ float3 &operator/=(float3 &a, float s) { a = a / s; return a; }\n\n");
    write("__device__ __forceinline__ float sc_dot(float3 a, float3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }\n");
    write("__device__ __forceinline__ float sc_length(float3 a) { return sqrtf(sc_dot(a, a)); }\n");
    write("__device__ __forceinline__ float3 sc_normalize(float3 a) { float l = sc_length(a); return (l > 0.0f) ? a / l : a; }\n");
    write("__device__ __forceinline__ float3 sc_cross(float3 a, float3 b) {\n");
    write("  return sb_make_float3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);\n");
    write("}\n\n");
    write("struct sb_uint2 { unsigned int x, y; };\n");
    // Column-major 4x4, mirrors CommandCtxBase::renderMatrix for the
    // matrix-driven texture coord spaces.
    write("struct sb_mat4 { float m[16]; };\n");
    write("__device__ __forceinline__ float3 sb_mat4_mul_point(sb_mat4 t, float3 p) {\n");
    write("  float x = t.m[0]*p.x + t.m[4]*p.y + t.m[8]*p.z  + t.m[12];\n");
    write("  float y = t.m[1]*p.x + t.m[5]*p.y + t.m[9]*p.z  + t.m[13];\n");
    write("  float z = t.m[2]*p.x + t.m[6]*p.y + t.m[10]*p.z + t.m[14];\n");
    write("  return sb_make_float3(x, y, z);\n");
    write("}\n\n");
    write("struct NodeMeta { unsigned int vert_offset; unsigned int vert_count; };\n");
    write("struct StrokeSample { float3 pos; float3 normal; float arclen; };\n\n");

    // Forward-mode dual prelude — emitted only when grad() is used. Mirrors the
    // cpp/wgsl duals so all backends produce the same gradient; CUDA float3 has
    // no operator[], so sb_idx picks components explicitly.
    if (brushUsesGrad()) {
      write("struct sbdual { float v; float3 d; };\n");
      write("struct sbdual3 { float3 v; float3 dx, dy, dz; };\n");
      write("__device__ __forceinline__ float sb_idx(float3 a, int i) { return (i==0)?a.x:(i==1)?a.y:a.z; }\n");
      write("__device__ __forceinline__ sbdual sb_c(float x) { return {x, sb_make_float3(0,0,0)}; }\n");
      write("__device__ __forceinline__ sbdual3 sb_c3(float3 p) { return {p, sb_make_float3(0,0,0), sb_make_float3(0,0,0), sb_make_float3(0,0,0)}; }\n");
      write("__device__ __forceinline__ sbdual3 sb_seed3(float3 p) { return {p, sb_make_float3(1,0,0), sb_make_float3(0,1,0), sb_make_float3(0,0,1)}; }\n");
      write("__device__ __forceinline__ sbdual sb_comp(sbdual3 a, int i) { return {sb_idx(a.v,i), sb_make_float3(sb_idx(a.dx,i), sb_idx(a.dy,i), sb_idx(a.dz,i))}; }\n");
      write("__device__ __forceinline__ sbdual3 sb_v3(sbdual x, sbdual y, sbdual z) { return {sb_make_float3(x.v,y.v,z.v), sb_make_float3(x.d.x,y.d.x,z.d.x), sb_make_float3(x.d.y,y.d.y,z.d.y), sb_make_float3(x.d.z,y.d.z,z.d.z)}; }\n");
      write("__device__ __forceinline__ sbdual operator+(sbdual a, sbdual b) { return {a.v+b.v, a.d+b.d}; }\n");
      write("__device__ __forceinline__ sbdual operator-(sbdual a, sbdual b) { return {a.v-b.v, a.d-b.d}; }\n");
      write("__device__ __forceinline__ sbdual operator-(sbdual a) { return {-a.v, -a.d}; }\n");
      write("__device__ __forceinline__ sbdual operator*(sbdual a, sbdual b) { return {a.v*b.v, a.d*b.v + b.d*a.v}; }\n");
      write("__device__ __forceinline__ sbdual operator/(sbdual a, sbdual b) { return {a.v/b.v, (a.d*b.v - b.d*a.v)/(b.v*b.v)}; }\n");
      write("__device__ __forceinline__ sbdual sbd_sin(sbdual a) { return {sinf(a.v), a.d*cosf(a.v)}; }\n");
      write("__device__ __forceinline__ sbdual sbd_cos(sbdual a) { return {cosf(a.v), a.d*(-sinf(a.v))}; }\n");
      write("__device__ __forceinline__ sbdual sbd_sqrt(sbdual a) { float r=sqrtf(a.v); return {r, a.d*(r>0?0.5f/r:0.0f)}; }\n");
      write("__device__ __forceinline__ sbdual sbd_abs(sbdual a) { return {fabsf(a.v), a.d*(a.v<0?-1.0f:1.0f)}; }\n");
      write("__device__ __forceinline__ sbdual sbd_dot(sbdual3 a, sbdual3 b) { return {sc_dot(a.v,b.v), a.dx*b.v.x+b.dx*a.v.x+a.dy*b.v.y+b.dy*a.v.y+a.dz*b.v.z+b.dz*a.v.z}; }\n");
      write("__device__ __forceinline__ sbdual sbd_length(sbdual3 a) { return sbd_sqrt(sbd_dot(a,a)); }\n");
      write("__device__ __forceinline__ sbdual sbd_mix(sbdual a, sbdual b, sbdual t) { return a+(b-a)*t; }\n\n");
    }

    // User-defined struct decls.
    for (const auto &sd : brush->structs) {
      write("struct ");
      write(sd.name);
      write(" {\n");
      for (const auto &f : sd.fields) {
        write("  ");
        write(cudaType(f.type));
        write(" ");
        write(f.name);
        write(";\n");
      }
      write("};\n\n");
    }

    auto isBuiltinBrushName = [](const char *n) {
      return std::strcmp(n, "strength") == 0 || std::strcmp(n, "radius") == 0 ||
             std::strcmp(n, "spacing") == 0  || std::strcmp(n, "invert") == 0 ||
             std::strcmp(n, "falloff_kind") == 0 ||
             std::strcmp(n, "falloff_shape") == 0 ||
             std::strcmp(n, "falloff_dir") == 0 ||
             std::strcmp(n, "coord_space") == 0 ||
             std::strcmp(n, "tex_repeat") == 0 ||
             std::strcmp(n, "stroke_path_count") == 0;
    };
    auto isBuiltinCtxName = [](const char *n) {
      return std::strcmp(n, "surfacePos") == 0 || std::strcmp(n, "surfaceNo") == 0 ||
             std::strcmp(n, "render_matrix") == 0;
    };

    // DSL-declared field -> C++ member. Array<T,N> puts the dimension after
    // the name (C++ array declarator), unlike WGSL's array<T,N>.
    auto writeFieldMember = [&](const Field &f) {
      out += "  ";
      if (f.type == TypeKind::Array) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d", f.arraySize);
        out += cudaType(f.arrayElem);
        out += " ";
        out += f.name;
        out += "[";
        out += buf;
        out += "]";
      } else {
        out += cudaType(f.type);
        out += " ";
        out += f.name;
      }
      out += ";\n";
    };

    write("struct BrushUniforms {\n");
    write("  float strength;\n");
    write("  float radius;\n");
    write("  float spacing;\n");
    write("  unsigned int invert;\n");
    write("  unsigned int falloff_kind;\n");
    write("  unsigned int falloff_shape;\n");
    write("  float3 falloff_dir;\n");
    write("  unsigned int coord_space;\n");
    write("  float tex_repeat;\n");
    write("  unsigned int stroke_path_count;\n");
    for (const auto &f : brush->fields) {
      if (f.kind != FieldKind::Uniform) continue;
      if (isBuiltinBrushName(f.name.c_str())) continue;
      writeFieldMember(f);
    }
    write("};\n\n");

    write("struct CtxUniforms {\n");
    write("  float3 surfacePos;\n");
    write("  float3 surfaceNo;\n");
    write("  sb_mat4 render_matrix;\n");
    for (const auto &f : brush->fields) {
      if (f.kind != FieldKind::Ctx) continue;
      if (isBuiltinCtxName(f.name.c_str())) continue;
      writeFieldMember(f);
    }
    write("};\n\n");

    // Device-resident bindings. A host launcher would populate these (e.g.
    // cudaMemcpyToSymbol); the syntactic gate just needs them declared. The
    // brush texture is a flat row-major luminance buffer (1x1 white when no
    // texture is bound, so brush_sample_tex returns 1.0).
    write("__device__ float3* co_buf;\n");
    write("__device__ float3* no_buf;\n");
    write("__device__ float* mask_buf;\n");
    write("__device__ unsigned int* unique_verts;\n");
    write("__device__ NodeMeta* nodes;\n");
    write("__device__ BrushUniforms brush_u;\n");
    write("__device__ CtxUniforms ctx_u;\n");
    write("__device__ float falloff_lut[256];\n");
    write("__device__ float* brush_tex;\n");
    write("__device__ int brush_tex_w;\n");
    write("__device__ int brush_tex_h;\n");
    write("__device__ StrokeSample* stroke_path;\n");
    if (usesNeighbors) {
      write("__device__ float3* co_prev;\n");
      write("__device__ sb_uint2* vert_nbr_meta;\n");
      write("__device__ unsigned int* nbr_verts;\n");
    }
    write("\n");

    // Falloff selector — kept in lockstep with Brush::falloffEval (brush.h),
    // identical closed forms to the WGSL/CPU paths.
    write("__device__ float brush_falloff(float t) {\n");
    write("  if (brush_u.falloff_kind == 1u) {\n");
    write("    return t;\n");
    write("  } else if (brush_u.falloff_kind == 2u) {\n");
    write("    float sb_u = 1.0f - t;\n");
    write("    return expf(-9.0f * sb_u * sb_u);\n");
    write("  } else if (brush_u.falloff_kind == 3u) {\n");
    write("    float sb_c = sb_clampf(t, 0.0f, 1.0f);\n");
    write("    float sb_s = sb_c * 255.0f;\n");
    write("    int sb_i = (int)floorf(sb_s);\n");
    write("    if (sb_i >= 255) { return falloff_lut[255]; }\n");
    write("    float sb_f = sb_s - (float)sb_i;\n");
    write("    return falloff_lut[sb_i] * (1.0f - sb_f) + falloff_lut[sb_i + 1] * sb_f;\n");
    write("  }\n");
    write("  return t * t * (3.0f - 2.0f * t);\n");
    write("}\n\n");
    write("__device__ float brush_falloff_dist(float3 delta) {\n");
    write("  float sb_inv_r = 1.0f / brush_u.radius;\n");
    write("  if (brush_u.falloff_shape == 1u) {\n");
    write("    float3 sb_a = sb_make_float3(fabsf(delta.x), fabsf(delta.y), fabsf(delta.z));\n");
    write("    return fmaxf(sb_a.x, fmaxf(sb_a.y, sb_a.z)) * sb_inv_r;\n");
    write("  } else if (brush_u.falloff_shape == 2u) {\n");
    write("    return fabsf(sc_dot(delta, brush_u.falloff_dir)) * sb_inv_r;\n");
    write("  }\n");
    write("  return sc_length(delta) * sb_inv_r;\n");
    write("}\n\n");
    write("__device__ float brush_strength(float3 p) {\n");
    write("  float sb_t = 1.0f - fminf(brush_falloff_dist(p - ctx_u.surfacePos), 1.0f);\n");
    write("  return brush_u.strength * brush_falloff(sb_t);\n");
    write("}\n\n");
    write("__device__ float2 brush_stroke_uv(float3 co) {\n");
    write("  if (brush_u.stroke_path_count == 0u) { return sb_make_float2(0.0f, 0.0f); }\n");
    write("  if (brush_u.stroke_path_count == 1u) {\n");
    write("    return sb_make_float2(stroke_path[0].arclen, sc_length(co - stroke_path[0].pos));\n");
    write("  }\n");
    write("  float sb_best_dist = 3.402823e+38f;\n");
    write("  float sb_best_arc = 0.0f;\n");
    write("  float sb_best_lat = 0.0f;\n");
    write("  for (unsigned int i = 0u; i + 1u < brush_u.stroke_path_count; i = i + 1u) {\n");
    write("    float3 sb_a = stroke_path[i].pos;\n");
    write("    float3 sb_ab = stroke_path[i + 1u].pos - sb_a;\n");
    write("    float sb_len2 = sc_dot(sb_ab, sb_ab);\n");
    write("    float sb_t = 0.0f;\n");
    write("    if (sb_len2 > 0.0f) { sb_t = sc_dot(co - sb_a, sb_ab) / sb_len2; }\n");
    write("    sb_t = sb_clampf(sb_t, 0.0f, 1.0f);\n");
    write("    float sb_d = sc_length(co - (sb_a + sb_ab * sb_t));\n");
    write("    if (sb_d < sb_best_dist) {\n");
    write("      sb_best_dist = sb_d;\n");
    write("      sb_best_arc = stroke_path[i].arclen + (stroke_path[i + 1u].arclen - stroke_path[i].arclen) * sb_t;\n");
    write("      sb_best_lat = sb_d;\n");
    write("    }\n");
    write("  }\n");
    write("  return sb_make_float2(sb_best_arc, sb_best_lat);\n");
    write("}\n\n");
    // Brush-texture modulation — kept in lockstep with
    // CommandCtx::sampleBrushTex. Manual clamp-to-edge bilinear over the flat
    // row-major buffer (NEAREST texel fetch + float blend) so the result is
    // bit-modulo-fp identical to the CPU path.
    write("__device__ float brush_sample_tex(float3 co, float3 no) {\n");
    write("  (void)no;\n");
    write("  float2 sb_uv;\n");
    write("  if (brush_u.coord_space == 1u) {\n");
    write("    float3 sb_p = sb_mat4_mul_point(ctx_u.render_matrix, co);\n");
    write("    sb_uv = sb_make_float2(sb_p.x, sb_p.y);\n");
    write("  } else if (brush_u.coord_space == 2u) {\n");
    write("    float3 sb_p = sb_mat4_mul_point(ctx_u.render_matrix, co);\n");
    write("    sb_uv = sb_make_float2(sb_p.x * brush_u.tex_repeat, sb_p.y * brush_u.tex_repeat);\n");
    write("  } else if (brush_u.coord_space == 3u) {\n");
    write("    sb_uv = brush_stroke_uv(co);\n");
    write("  } else if (brush_u.coord_space == 4u) {\n");
    write("    float3 sb_n = sc_normalize(ctx_u.surfaceNo);\n");
    write("    float3 sb_ref = sb_make_float3(0.0f, 0.0f, 1.0f);\n");
    write("    if (fabsf(sb_n.z) >= 0.999f) { sb_ref = sb_make_float3(1.0f, 0.0f, 0.0f); }\n");
    write("    float3 sb_t1 = sc_normalize(sc_cross(sb_ref, sb_n));\n");
    write("    float3 sb_t2 = sc_cross(sb_n, sb_t1);\n");
    write("    float3 sb_rel = co - ctx_u.surfacePos;\n");
    write("    sb_uv = sb_make_float2(sc_dot(sb_rel, sb_t1), sc_dot(sb_rel, sb_t2));\n");
    write("  } else {\n");
    write("    sb_uv = sb_make_float2(co.x, co.y);\n");
    write("  }\n");
    write("  float sb_dimx = (float)brush_tex_w;\n");
    write("  float sb_dimy = (float)brush_tex_h;\n");
    write("  float sb_fx = sb_uv.x * sb_dimx - 0.5f;\n");
    write("  float sb_fy = sb_uv.y * sb_dimy - 0.5f;\n");
    write("  float sb_x0 = floorf(sb_fx);\n");
    write("  float sb_y0 = floorf(sb_fy);\n");
    write("  float sb_tx = sb_fx - sb_x0;\n");
    write("  float sb_ty = sb_fy - sb_y0;\n");
    write("  int sb_w = brush_tex_w;\n");
    write("  int sb_h = brush_tex_h;\n");
    write("  int sb_x0c = (int)sb_clampf(sb_x0, 0.0f, (float)(sb_w - 1));\n");
    write("  int sb_y0c = (int)sb_clampf(sb_y0, 0.0f, (float)(sb_h - 1));\n");
    write("  int sb_x1c = (int)sb_clampf(sb_x0 + 1.0f, 0.0f, (float)(sb_w - 1));\n");
    write("  int sb_y1c = (int)sb_clampf(sb_y0 + 1.0f, 0.0f, (float)(sb_h - 1));\n");
    write("  float sb_p00 = brush_tex[sb_y0c * sb_w + sb_x0c];\n");
    write("  float sb_p10 = brush_tex[sb_y0c * sb_w + sb_x1c];\n");
    write("  float sb_p01 = brush_tex[sb_y1c * sb_w + sb_x0c];\n");
    write("  float sb_p11 = brush_tex[sb_y1c * sb_w + sb_x1c];\n");
    write("  float sb_a = sb_p00 * (1.0f - sb_tx) + sb_p10 * sb_tx;\n");
    write("  float sb_b = sb_p01 * (1.0f - sb_tx) + sb_p11 * sb_tx;\n");
    write("  return sb_a * (1.0f - sb_ty) + sb_b * sb_ty;\n");
    write("}\n\n");
  }

  void emitSkipStub(const char *reason)
  {
    write("// AUTO-GENERATED by sbrushc (");
    write(target == BackendKind::Cuda ? "CUDA" : "HIP");
    write(" backend) — DO NOT EDIT.\n");
    write("// Source: ");
    write(brush->sourceFile);
    write("\n// SKIPPED: ");
    write(reason);
    write("\n\n");
    write("#ifndef __global__\n#define __global__ __attribute__((global))\n#endif\n");
    write("extern \"C\" __global__ void nop() {}\n");
  }

  // Inline texture eval — a pure device function over its params + intrinsics.
  void emitTextureFn(const TextureDef &td)
  {
    write("__device__ ");
    write(cudaType(td.returnType));
    write(" ");
    write(texEvalName(td));
    write("(");
    bool first = true;
    for (const auto &p : td.params) {
      if (!first) write(", ");
      first = false;
      write(cudaType(p.type));
      write(" ");
      write(p.name);
    }
    write(") {\n");
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

  // Reduce stage -> device function. out/inout params are C++ references so
  // the callee writes back; `in` params pass by value.
  void emitReduceStage(const Stage &st)
  {
    write("__device__ void ");
    write(st.name);
    write("(");
    bool first = true;
    for (const auto &p : st.params) {
      if (!first) write(", ");
      first = false;
      const char *typeSpelling = (p.type == TypeKind::Struct)
                                     ? p.structName.c_str()
                                     : cudaType(p.type);
      write(typeSpelling);
      if (p.dir == ParamDir::Out || p.dir == ParamDir::InOut) write(" &");
      else write(" ");
      write(p.name);
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

  void run()
  {
    if (!vertexStage) {
      err("brush has no vertex stage");
      return;
    }
    if (vertexStage->params.size() < 1) {
      err("vertex stage must take at least one parameter (the Vertex bundle)");
    }

    usesNeighbors = hasNeighborLoop(vertexStage->body.get());

    emitPrelude();

    for (const auto &td : brush->textures) {
      emitTextureFn(td);
    }

    Vector<const Stage *> reduceStages;
    for (const auto &st : brush->stages) {
      if (st.kind == StageKind::Reduce) reduceStages.append(&st);
    }
    for (const auto *st : reduceStages) {
      emitReduceStage(*st);
    }

    // Per-thread vertex kernel: one workgroup per spatial node (SB_NODE_IDX),
    // one thread per vertex (SB_LOCAL_IDX). The kernel name is the brush's
    // @brush attribute so each .cu/.hip has a distinct, readable entry point.
    string kernelName = brush->attrName.size() > 0 ? brush->attrName : brush->cppName;
    write("extern \"C\" __global__ void ");
    write(kernelName);
    write("() {\n");
    write("  NodeMeta sb_node = nodes[SB_NODE_IDX];\n");
    write("  unsigned int lid = SB_LOCAL_IDX;\n");
    write("  if (lid >= sb_node.vert_count) { return; }\n");
    write("  unsigned int sb_vidx = unique_verts[sb_node.vert_offset + lid];\n");
    write("  float3 ");
    write(vertexParamName); write("_co = co_buf[sb_vidx];\n");
    write("  float3 ");
    write(vertexParamName); write("_no = no_buf[sb_vidx];\n");
    write("  float ");
    write(vertexParamName); write("_mask = mask_buf[sb_vidx];\n");

    // Declare locals for the vertex stage's extra (reduce-output) params and
    // call each reduce stage on them — matches the C++ executor's one call
    // per node.
    for (int pi = 1; pi < (int)vertexStage->params.size(); pi++) {
      const auto &p = vertexStage->params[pi];
      write("  ");
      if (p.type == TypeKind::Struct) write(p.structName);
      else write(cudaType(p.type));
      write(" ");
      write(p.name);
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
        bool found = false;
        for (int pi = 1; pi < (int)vertexStage->params.size(); pi++) {
          const auto &vp = vertexStage->params[pi];
          if (vp.type != rp.type) continue;
          if (!string(vp.name).operator==(string(rp.name.c_str()))) continue;
          if (rp.type == TypeKind::Struct &&
              !string(vp.structName).operator==(string(rp.structName.c_str()))) continue;
          write(vp.name);  // out/inout pass by reference — no address-of needed
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
    write("  co_buf[sb_vidx] = ");
    write(vertexParamName); write("_co;\n");
    write("  no_buf[sb_vidx] = ");
    write(vertexParamName); write("_no;\n");
    write("  mask_buf[sb_vidx] = ");
    write(vertexParamName); write("_mask;\n");
    write("}\n");
  }
};

} // namespace

EmitResult emitCuda(const Brush &brush, BackendKind target)
{
  Emit em;
  em.brush = &brush;
  em.target = target;
  for (const auto &st : brush.stages) {
    if (st.kind == StageKind::Vertex) { em.vertexStage = &st; break; }
  }
  if (em.vertexStage && em.vertexStage->params.size() > 0) {
    em.vertexParamName = em.vertexStage->params[0].name;
  } else {
    em.vertexParamName = string("v");
  }
  em.run();
  EmitResult r;
  r.text = std::move(em.out);
  r.errors = std::move(em.errors);
  return r;
}

} // namespace sculptcore::brush::sbrush
