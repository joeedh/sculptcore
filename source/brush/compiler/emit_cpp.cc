#include "emit_cpp.h"
#include "../kernels/ir/intrinsics.h"
// The engine-side Brush struct: sbrushc consults Brush::builtinPropNames at
// generation time to split member-backed uniforms from named-store slots.
#include "brush/brush.h"
#include <cctype>
#include <cstdio>
#include <cstring>

namespace sculptcore::brush::sbrush {

using litestl::util::string;
using litestl::util::stringref;
using litestl::util::Vector;

bool isCtxBaseName(const char *n)
{
  return (std::strcmp(n, "mouse") == 0) || (std::strcmp(n, "mousePos") == 0) ||
         (std::strcmp(n, "surfacePos") == 0) || (std::strcmp(n, "surfaceNo") == 0) ||
         (std::strcmp(n, "mouseDir") == 0) || (std::strcmp(n, "renderMatrix") == 0) ||
         (std::strcmp(n, "isFirstOfStep") == 0) || (std::strcmp(n, "meshLog") == 0);
}

bool isMemberBackedName(const char *name)
{
  static Vector<string> names = [] {
    Vector<string> v;
    ::sculptcore::brush::Brush::builtinPropNames(v);
    return v;
  }();
  for (const auto &n : names) {
    if (n == string(name)) {
      return true;
    }
  }
  return false;
}

bool fieldUsesStore(const Field &f)
{
  if (f.kind == FieldKind::Attr) {
    return false;
  }
  if (f.kind == FieldKind::Ctx && isCtxBaseName(f.name.c_str())) {
    return false;
  }
  return !isMemberBackedName(f.name.c_str());
}

namespace {

struct Emit {
  const Brush *brush;
  // Extra (out-of-repo) kernel: store-classified uniforms lower to
  // namedFloats slots instead of erroring (see CppEmitOptions).
  bool extrasMode = false;
  const Stage *vertexStage = nullptr;
  const Stage *faceStage = nullptr;
  string vertexParamName; // e.g. "v"
  string faceParamName;   // e.g. "f"

  // Stage currently being lowered — drives stage-param identifier
  // resolution (so reduce-body `s` and vertex-body `v` route correctly).
  const Stage *currentStage = nullptr;

  // Texture whose eval body is being lowered — routes `param` identifiers to
  // the slab (sb_tex_params), gates mapPoint(), and flags sampler calls.
  const TextureDef *currentTexture = nullptr;

  string out;
  Vector<string> errors;
  int indent = 0;

  // Locals declared in the current body. We track the declared type so a
  // vector-component access (`v.x`) can be lowered to `operator[]` on the C++
  // backend — litestl Vec has no named .x/.y/.z/.w members, unlike WGSL/CUDA/
  // OpenCL vectors. Knowing a name is a local also routes identifier resolution.
  struct LocalVar {
    string name;
    TypeKind type = TypeKind::Unknown;
  };
  Vector<LocalVar> locals;

  // Set when a NeighborLoop is emitted — drives an extra #include in
  // the generated header so EdgeOfVertIter resolves.
  bool neighborLoopUsed = false;
  // Set when grad(expr, var) is used — emits the forward-mode dual prelude.
  bool gradUsed = false;
  // When rewriting a grad body, the float3 variable being differentiated.
  string gradVar;

  // Element-bundle identifiers currently in scope (active for_neighbor
  // bindings) — used together with the vertex param to recognize
  // v.<attr>/nb.<attr> attribute access.
  Vector<string> nbrBundles;

  void err(const char *msg)
  {
    errors.append(string(msg));
  }

  void errf(const char *fmt, const char *arg)
  {
    char buf[256];
    std::snprintf(buf, sizeof(buf), fmt, arg);
    errors.append(string(buf));
  }

  void writeIndent()
  {
    for (int i = 0; i < indent; i++)
      out += "  ";
  }

  void write(const char *s)
  {
    out += s;
  }
  void write(const string &s)
  {
    out += s;
  }

  bool isLocal(stringref name) const
  {
    for (const auto &l : locals) {
      if (string(l.name).operator==(string(name.c_str())))
        return true;
    }
    return false;
  }

  const Field *findField(stringref name) const
  {
    for (const auto &f : brush->fields) {
      if (string(f.name).operator==(string(name.c_str())))
        return &f;
    }
    return nullptr;
  }

  // Find a declared `attr` field by handle name (any domain).
  const Field *findAttrField(stringref name) const
  {
    for (const auto &f : brush->fields) {
      if (f.kind != FieldKind::Attr)
        continue;
      if (string(f.name).operator==(string(name.c_str())))
        return &f;
    }
    return nullptr;
  }

  // If `name` binds a per-element bundle (vertex-stage param, an active
  // for_neighbor binding, or the face-stage param), set its index-field
  // ("v"/"f") and the generated handle-local prefix and return true.
  bool bundleInfo(stringref name, const char *&idxField, const char *&prefix) const
  {
    if (string(vertexParamName).operator==(string(name.c_str()))) {
      idxField = "v";
      prefix = "__attr_";
      return true;
    }
    for (const auto &b : nbrBundles) {
      if (string(b).operator==(string(name.c_str()))) {
        idxField = "v";
        prefix = "__attr_";
        return true;
      }
    }
    if (faceParamName.size() && string(faceParamName).operator==(string(name.c_str()))) {
      idxField = "f";
      prefix = "__fattr_";
      return true;
    }
    return false;
  }

  // Resolve a dotted call name like "Rings.eval" to its texture def.
  const TextureDef *findTextureCall(stringref callName) const
  {
    for (const auto &t : brush->textures) {
      string full = t.name + ".eval";
      if (string(full).operator==(string(callName.c_str())))
        return &t;
    }
    return nullptr;
  }

  // Resolve a `param` name in the texture currently being lowered.
  const TexParam *findTexParam(stringref name) const
  {
    if (!currentTexture)
      return nullptr;
    for (const auto &tp : currentTexture->texParams) {
      if (string(tp.name).operator==(string(name.c_str())))
        return &tp;
    }
    return nullptr;
  }

  bool anyTextureUsesMap() const
  {
    for (const auto &t : brush->textures) {
      if (t.usesMap)
        return true;
    }
    return false;
  }

  static string texDefaultsName(const TextureDef &td)
  {
    return string("tex") + capitalize(td.name) + "ParamDefaults";
  }

  // Format `v` as a C++ float literal (round-trip exact for float values).
  static void appendFloatLit(string &s, double v)
  {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    bool hasDot = false;
    for (const char *p = buf; *p; p++) {
      if (*p == '.' || *p == 'e' || *p == 'E') {
        hasDot = true;
        break;
      }
    }
    s += buf;
    if (!hasDot)
      s += ".0";
    s += "f";
  }

  bool isStageParam(stringref name) const
  {
    if (!currentStage)
      return false;
    for (const auto &p : currentStage->params) {
      if (string(p.name).operator==(string(name.c_str())))
        return true;
    }
    return false;
  }

  static bool isVectorType(TypeKind t)
  {
    return t == TypeKind::Float2 || t == TypeKind::Float3 || t == TypeKind::Float4;
  }

  // Component index for a `.x`/`.y`/`.z`/`.w` swizzle, or -1 otherwise.
  static int swizzleIndex(const string &name)
  {
    if (name.size() != 1) {
      return -1;
    }
    switch (name.c_str()[0]) {
    case 'x':
      return 0;
    case 'y':
      return 1;
    case 'z':
      return 2;
    case 'w':
      return 3;
    default:
      return -1;
    }
  }

  // Best-effort type of an expression — enough to recognize vector-component
  // access. Returns Unknown when it can't resolve cheaply; callers must treat
  // Unknown conservatively (emit the expression unchanged).
  TypeKind resolveExprType(const Expr &e) const
  {
    switch (e.kind) {
    case ExprKind::LitFloat:
      return TypeKind::Float;
    case ExprKind::LitInt:
      return TypeKind::Int;
    case ExprKind::LitBool:
      return TypeKind::Bool;
    case ExprKind::Paren:
      return e.lhs ? resolveExprType(*e.lhs) : TypeKind::Unknown;
    case ExprKind::Ident: {
      for (const auto &l : locals) {
        if (string(l.name).operator==(string(e.name.c_str()))) {
          return l.type;
        }
      }
      if (currentStage) {
        for (const auto &p : currentStage->params) {
          if (string(p.name).operator==(string(e.name.c_str()))) {
            return p.type;
          }
        }
      }
      if (const Field *f = findField(stringref(e.name.c_str()))) {
        return f->type;
      }
      return TypeKind::Unknown;
    }
    case ExprKind::Member: {
      // Attribute-bundle access (v.co) carries the attr field's type.
      if (e.lhs && e.lhs->kind == ExprKind::Ident) {
        const char *bidx = nullptr, *bprefix = nullptr;
        if (bundleInfo(stringref(e.lhs->name.c_str()), bidx, bprefix)) {
          if (const Field *af = findAttrField(stringref(e.name.c_str()))) {
            return af->type;
          }
        }
      }
      // A swizzle of a proven vector yields a scalar.
      if (e.lhs && isVectorType(resolveExprType(*e.lhs)) && swizzleIndex(e.name) >= 0) {
        return TypeKind::Float;
      }
      return TypeKind::Unknown;
    }
    case ExprKind::Index:
      return e.lhs && isVectorType(resolveExprType(*e.lhs)) ? TypeKind::Float : TypeKind::Unknown;
    case ExprKind::Binary: {
      TypeKind a = e.lhs ? resolveExprType(*e.lhs) : TypeKind::Unknown;
      TypeKind b = e.rhs ? resolveExprType(*e.rhs) : TypeKind::Unknown;
      if (isVectorType(a)) {
        return a;
      }
      if (isVectorType(b)) {
        return b;
      }
      return a != TypeKind::Unknown ? a : b;
    }
    default:
      return TypeKind::Unknown;
    }
  }

  // === expression emitter ===

  void emitExpr(const Expr &e)
  {
    switch (e.kind) {
    case ExprKind::LitFloat: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.17g", e.fvalue);
      // Ensure we look like a float literal — %g can produce "0" or "1e10"
      // without a decimal point, which then makes "0f" / "1e10f" invalid.
      bool hasDot = false;
      for (const char *p = buf; *p; p++) {
        if (*p == '.' || *p == 'e' || *p == 'E') {
          hasDot = true;
          break;
        }
      }
      out += buf;
      if (!hasDot)
        out += ".0";
      out += "f";
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
      if (isLocal(nm) || isStageParam(nm)) {
        out += e.name;
      } else if (const TexParam *tp = findTexParam(nm)) {
        // Texture `param` reads. @const params fold to literals; runtime
        // floats read their slab slot; a ramp has no scalar value — only
        // `<name>.sample(t)` is meaningful (handled in the Call case).
        if (tp->isConst) {
          if (tp->kind == TexParamKind::Int) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", (long long)tp->defaultValue);
            out += buf;
          } else {
            appendFloatLit(out, tp->defaultValue);
          }
        } else if (tp->kind == TexParamKind::Ramp) {
          errf("ramp param '%s' can only be used via .sample(t)", e.name.c_str());
          out += "0.0f";
        } else {
          char buf[64];
          std::snprintf(buf, sizeof(buf), "sb_tex_params[%d]", tp->offset);
          out += buf;
        }
      } else if (auto *f = findField(nm)) {
        // Uniforms live on Brush. Ctx-kind fields default to ctx.brush.X
        // too, so the DSL can name new per-stroke state without having
        // to extend CommandCtxBase. The exception is the hardcoded
        // CommandCtxBase members (surfacePos, surfaceNo, mouse, …),
        // which keep the legacy `ctx.<name>` spelling.
        //
        // Host stages take `(CommandCtxBase &ctx, Brush &brush)` — there
        // is no `ctx.brush`, so uniforms/non-builtin ctx fields resolve
        // to bare `brush.X` instead. Builtin ctx-base fields still go
        // through `ctx.X` either way. Extra-kernel store uniforms read
        // their registry-assigned namedFloats slot.
        bool isCtxBase = isCtxBaseName(e.name.c_str());
        bool inHost = (currentStage && currentStage->kind == StageKind::Host);
        if (f->kind == FieldKind::Ctx && isCtxBase) {
          out += "ctx.";
          out += e.name;
        } else if (extrasMode && fieldUsesStore(*f)) {
          out += inHost ? "brush.namedFloats[kExtraSlot_" : "ctx.brush.namedFloats[kExtraSlot_";
          out += e.name;
          out += "]";
        } else {
          out += inHost ? "brush." : "ctx.brush.";
          out += e.name;
        }
      } else {
        // Could be an intrinsic referenced without a call — treat as bare
        // identifier and let the C++ compiler catch it.
        out += e.name;
      }
      break;
    }
    case ExprKind::Member:
      // Attribute access on an element bundle (v.<attr> / nb.<attr> / f.<attr>)
      // lowers to an indexed read/write of the bound layer.
      if (e.lhs->kind == ExprKind::Ident) {
        const char *bidx = nullptr, *bprefix = nullptr;
        if (bundleInfo(stringref(e.lhs->name.c_str()), bidx, bprefix) &&
            findAttrField(stringref(e.name.c_str())))
        {
          out += "(*";
          out += bprefix;
          out += e.name;
          out += ")[";
          out += e.lhs->name;
          out += ".";
          out += bidx;
          out += "]";
          break;
        }
      }
      // Vector component: litestl Vec has operator[] but no named .x/.y/.z/.w,
      // so lower a proven vector swizzle to [i] (works as lvalue too).
      {
        int si = swizzleIndex(e.name);
        if (si >= 0 && e.lhs && isVectorType(resolveExprType(*e.lhs))) {
          emitExpr(*e.lhs);
          char b[8];
          std::snprintf(b, sizeof(b), "[%d]", si);
          out += b;
          break;
        }
      }
      emitExpr(*e.lhs);
      out += ".";
      out += e.name;
      break;
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
      // grad(expr, var) — forward-mode gradient of scalar `expr` wrt float3
      // `var`. The arg is rewritten inline into dual-number form (emitDual)
      // and `.d` reads back the float3 ∂expr/∂var. Inline (not a lambda) so
      // the same shape lowers to WGSL/OpenCL, which have no closures.
      if (std::strcmp(e.name.c_str(), "grad") == 0 && e.args.size() == 2) {
        gradUsed = true;
        string savedVar = gradVar;
        gradVar = render(*e.args[1]);
        out += "(";
        emitDual(*e.args[0]);
        out += ").d";
        gradVar = savedVar;
        break;
      }
      // Dotted call `Tex.eval(args)` -> the texture's free function. The eval
      // ABI is (p, n, params, texctx): defaults slab (or nullptr) plus the
      // stage-prologue map ctx for mapPoint textures.
      if (const TextureDef *td = findTextureCall(stringref(e.name.c_str()))) {
        if (currentTexture) {
          errf("texture '%s' cannot call another texture", currentTexture->name.c_str());
        }
        out += "tex";
        out += capitalize(td->name);
        out += "Eval(";
        for (int i = 0; i < (int)e.args.size(); i++) {
          if (i > 0)
            out += ", ";
          emitExpr(*e.args[i]);
        }
        out += ", ";
        out += td->slabSize > 0 ? texDefaultsName(*td) : string("nullptr");
        out += ", ";
        out += td->usesMap ? "sb_texctx" : "nullptr";
        out += ")";
        break;
      }
      // mapPoint(p) — texture-scope map-matrix transform (texture-scripts
      // plan). Lowers to the pure texMapPoint helper on the threaded ctx.
      if (std::strcmp(e.name.c_str(), "mapPoint") == 0) {
        if (!currentTexture) {
          err("mapPoint() is only valid inside a texture eval");
        }
        if (e.args.size() != 1) {
          err("mapPoint() takes exactly one float3 argument");
          out += "float3(0.0f, 0.0f, 0.0f)";
          break;
        }
        out += "texMapPoint(sb_texctx, ";
        emitExpr(*e.args[0]);
        out += ")";
        break;
      }
      // Ramp sample `<param>.sample(t)` inside a texture eval.
      if (currentTexture) {
        const char *dot = std::strchr(e.name.c_str(), '.');
        if (dot && std::strcmp(dot, ".sample") == 0) {
          string base = string(e.name.c_str()).substr(0, (int)(dot - e.name.c_str()));
          const TexParam *tp = findTexParam(stringref(base.c_str()));
          if (tp && tp->kind == TexParamKind::Ramp && e.args.size() == 1) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "texRampSample(sb_tex_params + %d, ",
                          tp->offset);
            out += buf;
            emitExpr(*e.args[0]);
            out += ")";
            break;
          }
        }
        // Sampler calls parse in T1 but have no host plumbing until T4.
        bool wasSampler = false;
        for (const auto &sm : currentTexture->samplerDeps) {
          if (string(sm).operator==(string(e.name.c_str()))) {
            errf("sampler '%s' cannot be called yet — host samplers are "
                 "runtime-only (T4)",
                 e.name.c_str());
            out += "0.0f";
            wasSampler = true;
            break;
          }
        }
        if (wasSampler)
          break;
      }
      const IntrinsicDef *intr = findIntrinsic(stringref(e.name.c_str()));
      if (intr) {
        const char *pat = intr->emit[(int)BackendKind::Cpp].pattern;
        if (!pat) {
          errf("intrinsic '%s' has no C++ emit pattern", e.name.c_str());
          out += "/*missing-intrinsic-pattern*/";
          break;
        }
        // Substitute $0..$N — lower each arg into a temporary string, then
        // copy with substitution.
        Vector<string> rendered;
        for (const auto &a : e.args) {
          string saved = out;
          out = string("");
          emitExpr(*a);
          rendered.append(out);
          out = saved;
        }
        for (const char *p = pat; *p;) {
          if (*p == '$' && std::isdigit((unsigned char)p[1])) {
            int idx = p[1] - '0';
            p += 2;
            if (idx < (int)rendered.size()) {
              out += rendered[idx];
            } else {
              out += "/*bad-arg*/";
            }
          } else if (*p == '$' && p[1] == 'v' && p[2] == 'm') {
            // The kernel's live painted mask — `<vertexParam>.mask`. Face stages
            // have none, so emit 0 (an unmasked vertex).
            p += 3;
            if (currentStage && currentStage == vertexStage) {
              out += vertexParamName;
              out += ".mask";
            } else {
              out += "0.0f";
            }
          } else if (*p == '$' && p[1] == 'v') {
            // Current loop vertex index — `<vertexParam>.v` inside a vertex
            // stage, keying the cavity automask. Outside a vertex stage (e.g. a
            // face-stage strength call) there is no per-vertex index, so emit -1;
            // CommandCtx::automasks treats v < 0 as "no automask".
            p += 2;
            if (currentStage && currentStage == vertexStage) {
              out += vertexParamName;
              out += ".v";
            } else {
              out += "-1";
            }
          } else {
            char tmp[2] = {*p, 0};
            out += tmp;
            p++;
          }
        }
      } else {
        // No intrinsic — emit a direct call. Used for built-in math
        // free functions (length, dot, ...) once their intrinsics are
        // registered; for now any unknown name reaches here.
        out += e.name;
        out += "(";
        for (int i = 0; i < (int)e.args.size(); i++) {
          if (i > 0)
            out += ", ";
          emitExpr(*e.args[i]);
        }
        out += ")";
      }
      break;
    }
    }
  }

  // Rewrite a scalar/vec3 expression into dual-number form. Constants and
  // non-`var` identifiers carry zero derivative (sb_c/sb_c3); `var` is the
  // seeded sbdual3; intrinsics map to sbd_* chain-rule overloads. The prelude
  // provides overloaded operators, so binary/unary just emit verbatim.
  string render(const Expr &e)
  {
    string saved = out;
    out = string("");
    emitExpr(e);
    string r = out;
    out = saved;
    return r;
  }
  bool isGradVar(const Expr &e)
  {
    string r = render(e);
    return string(r).operator==(string(gradVar.c_str()));
  }

  void emitDual(const Expr &e)
  {
    if (isGradVar(e)) {
      out += "sb_seed3(";
      emitExpr(e);
      out += ")";
      return;
    } // seeded float3 var
    switch (e.kind) {
    case ExprKind::LitFloat:
    case ExprKind::LitInt:
      out += "sb_c(";
      emitExpr(e);
      out += ")";
      break;
    case ExprKind::Ident:
      out += "sb_c3(";
      emitExpr(e);
      out += ")";
      break; // float3 const (zero deriv)
    case ExprKind::Member:
      if (e.lhs && isGradVar(*e.lhs)) { // var.x/y/z picks a Jacobian row
        out += "sb_comp(sb_seed3(";
        emitExpr(*e.lhs);
        out += "), ";
        out += (std::strcmp(e.name.c_str(), "x") == 0   ? "0"
                : std::strcmp(e.name.c_str(), "y") == 0 ? "1"
                                                        : "2");
        out += ")";
      } else {
        out += "sb_c(";
        emitExpr(e);
        out += ")";
      }
      break;
    case ExprKind::Paren:
      out += "(";
      emitDual(*e.lhs);
      out += ")";
      break;
    case ExprKind::Binary:
      out += "(";
      emitDual(*e.lhs);
      out += " ";
      out += binOpCSym(e.binop);
      out += " ";
      emitDual(*e.rhs);
      out += ")";
      break;
    case ExprKind::Unary:
      out += "(";
      out += unaryOpCSym(e.unaryop);
      emitDual(*e.lhs);
      out += ")";
      break;
    case ExprKind::Call: {
      const char *n = e.name.c_str();
      if (std::strcmp(n, "float3") == 0) {
        out += "sb_v3(";
        for (int i = 0; i < 3; i++) {
          if (i)
            out += ", ";
          emitDual(*e.args[i]);
        }
        out += ")";
        break;
      }
      out += "sbd_";
      out += n;
      out += "(";
      for (int i = 0; i < (int)e.args.size(); i++) {
        if (i)
          out += ", ";
        emitDual(*e.args[i]);
      }
      out += ")";
      break;
    }
    default:
      out += "sb_c(0.0f)";
      break;
    }
  }

  // === statement emitter ===

  // Emit the C++ spelling of an IR type. Struct types use their
  // user-defined name; everything else maps to the engine math types.
  void emitTypeRef(TypeKind ty, const string &structName)
  {
    if (ty == TypeKind::Struct) {
      out += structName;
    } else {
      out += typeKindName(ty);
    }
  }

  void emitStmt(const Stmt &s)
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
        emitExpr(*s.expr);
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

  // === top-level file emitter ===

  static string capitalize(const string &s)
  {
    string r = s;
    if (r.size() > 0)
      r[0] = (char)std::toupper((unsigned char)r[0]);
    return r;
  }

  static string lower(const string &s)
  {
    string r = s;
    for (int i = 0; i < (int)r.size(); i++) {
      r[i] = (char)std::tolower((unsigned char)r[i]);
    }
    return r;
  }

  // Format a double as a valid C++ float literal (mirrors the LitFloat case).
  static string floatLit(double v)
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
    r += "f";
    return r;
  }

  // DSL attr type -> C++ element type. Used inside the vertex fn, which opens
  // `using namespace litestl::math;`, so the bare vector names resolve.
  static const char *attrCppType(TypeKind t)
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
  static const char *attrTypeEnum(TypeKind t)
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

  static const char *attrDomainEnum(AttrDomain d)
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
  static const char *attrUseEnum(int use)
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

  // Emit one host stage as a templated free function. Host runs once per
  // dab on CPU with direct access to ctx (CommandCtxBase) and the Brush
  // — there's intentionally no per-node CommandCtx here, since the per-
  // node loop hasn't started yet. Currently host stages take no params;
  // any param-passing happens via ctx state.
  void emitHostStage(const Stage &st, const string &lowerBrush)
  {
    write("template <CommandTypes TYPES>\n");
    write("static void ");
    write(lowerBrush);
    write(capitalize(st.name));
    write("(CommandCtxBase &ctx, Brush &brush)\n");
    write("{\n");
    write("  (void)ctx; (void)brush;\n");
    emitTexCtxLocal();
    indent = 1;
    currentStage = &st;
    if (st.body && st.body->kind == StmtKind::Block) {
      int savedLocals = (int)locals.size();
      for (const auto &c : st.body->stmts)
        emitStmt(*c);
      while ((int)locals.size() > savedLocals)
        locals.pop_back();
    }
    currentStage = nullptr;
    indent = 0;
    write("}\n\n");
  }

  // Emit one reduce stage as a templated free function. The signature
  // mirrors the DSL source: each struct-typed param becomes a C++
  // reference param, scalars stay by-value (in) or by-reference (out/inout).
  void emitReduceStage(const Stage &st, const string &lowerBrush)
  {
    write("template <CommandTypes TYPES>\n");
    write("static void ");
    write(lowerBrush);
    write(capitalize(st.name));
    write("(CommandCtx<TYPES> &ctx");
    for (const auto &p : st.params) {
      write(", ");
      if (p.type == TypeKind::Struct) {
        // Pass struct params by reference; const for pure-in to express
        // intent (and to allow temporaries down the line).
        if (p.dir == ParamDir::In)
          write("const ");
        write(p.structName);
        write(" &");
      } else {
        // Scalars: by-ref for out/inout, by-value for in.
        write(typeKindName(p.type));
        if (p.dir == ParamDir::Out || p.dir == ParamDir::InOut)
          write(" &");
      }
      write(" ");
      write(p.name);
    }
    write(")\n");
    write("{\n");
    emitTexCtxLocal();
    indent = 1;
    currentStage = &st;
    if (st.body && st.body->kind == StmtKind::Block) {
      int savedLocals = (int)locals.size();
      for (const auto &c : st.body->stmts)
        emitStmt(*c);
      while ((int)locals.size() > savedLocals)
        locals.pop_back();
    }
    currentStage = nullptr;
    indent = 0;
    write("}\n\n");
  }

  // Stage-scope snapshot of ctx.renderMatrix for mapPoint textures — the
  // texture-scripts plan's TexEvalCtx threading. Call sites pass `sb_texctx`
  // for usesMap textures; every stage prologue emits it when any texture in
  // the brush needs it, since a texture call can appear in any stage body.
  void emitTexCtxLocal()
  {
    if (!anyTextureUsesMap())
      return;
    write("  TexEvalCtx sb_texctx_data = texEvalCtxFrom(ctx.renderMatrix);\n");
    write("  const TexEvalCtx *sb_texctx = &sb_texctx_data; (void)sb_texctx;\n");
  }

  // Emit one texture's non-@const param defaults as a static slab the brush
  // call sites pass when no runtime binding exists (T1: always). Ramps seed
  // to the identity ramp.
  void emitTextureDefaults(const TextureDef &td)
  {
    if (td.slabSize <= 0)
      return;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "[%d] = {\n", td.slabSize);
    write("static const float ");
    write(texDefaultsName(td));
    write(buf);
    for (const auto &tp : td.texParams) {
      if (tp.isConst)
        continue;
      write("    // ");
      write(tp.name);
      write("\n    ");
      if (tp.kind == TexParamKind::Ramp) {
        for (int i = 0; i < kTexRampSize; i++) {
          string lit;
          appendFloatLit(lit, (double)((float)i / (float)(kTexRampSize - 1)));
          write(lit);
          write(",");
          write((i % 8 == 7 && i != kTexRampSize - 1) ? "\n    " : " ");
        }
        write("\n");
      } else {
        string lit;
        appendFloatLit(lit, tp.defaultValue);
        write(lit);
        write(",\n");
      }
    }
    write("};\n");
    std::snprintf(buf, sizeof(buf),
                  "static_assert(kTexRampSize == %d, \"ramp slab size drifted\");\n\n",
                  kTexRampSize);
    write(buf);
  }

  // Emit one texture's eval as a pure free function. It sees only its own
  // parameters, its param slab, the threaded map ctx, and intrinsics — no
  // ctx/brush state — so the same text lowers identically on every backend
  // and stays runtime-compilable (T3).
  void emitTextureFn(const TextureDef &td)
  {
    write("static ");
    write(typeKindName(td.returnType));
    write(" tex");
    write(capitalize(td.name));
    write("Eval(");
    for (int i = 0; i < (int)td.params.size(); i++) {
      if (i > 0)
        write(", ");
      write(typeKindName(td.params[i].type));
      write(" ");
      write(td.params[i].name);
    }
    write(", const float *sb_tex_params, const TexEvalCtx *sb_texctx)\n{\n");
    write("  using namespace litestl::math;\n");
    for (const auto &p : td.params) {
      write("  (void)");
      write(p.name);
      write(";\n");
    }
    write("  (void)sb_tex_params; (void)sb_texctx;\n");
    indent = 1;
    // A scratch stage so identifier resolution treats the eval params as
    // stage params (bare names) rather than brush fields.
    Stage scratch;
    scratch.kind = StageKind::Reduce;
    for (const auto &p : td.params)
      scratch.params.append(p);
    currentStage = &scratch;
    currentTexture = &td;
    if (td.body && td.body->kind == StmtKind::Block) {
      int savedLocals = (int)locals.size();
      for (const auto &c : td.body->stmts)
        emitStmt(*c);
      while ((int)locals.size() > savedLocals)
        locals.pop_back();
    }
    currentTexture = nullptr;
    currentStage = nullptr;
    indent = 0;
    write("}\n\n");
  }

  // Defaults slab + eval fn for one texture. Imports are include-guarded:
  // several brushes in one TU may pull the same texture, and every emission
  // of it is byte-identical (same .stex parse), so first-wins is safe.
  void emitTextureBlock(const TextureDef &td)
  {
    if (td.imported) {
      write("#ifndef SB_TEX_DEF_");
      write(td.name);
      write("\n#define SB_TEX_DEF_");
      write(td.name);
      write("\n");
    }
    emitTextureDefaults(td);
    emitTextureFn(td);
    if (td.imported) {
      write("#endif  // SB_TEX_DEF_");
      write(td.name);
      write("\n\n");
    }
  }

  // Runtime-param manifest (non-@const params, decl order) — emitted only
  // into .tex.gen.h units, where the registry rows point at it. Kept outside
  // the SB_TEX_DEF_ guard: the symbol exists only in the unit header.
  void emitTextureManifest(const TextureDef &td)
  {
    bool any = false;
    for (const auto &tp : td.texParams) {
      any = any || !tp.isConst;
    }
    if (!any)
      return;
    write("static const TexParamManifestEntry tex");
    write(capitalize(td.name));
    write("ParamManifest[] = {\n");
    for (const auto &tp : td.texParams) {
      if (tp.isConst)
        continue;
      string defLit, minLit, maxLit;
      appendFloatLit(defLit, tp.kind == TexParamKind::Ramp ? 0.0 : tp.defaultValue);
      appendFloatLit(minLit, tp.hasRange ? tp.rangeMin : 0.0);
      appendFloatLit(maxLit, tp.hasRange ? tp.rangeMax : 0.0);
      char buf[256];
      std::snprintf(buf, sizeof(buf), "    {\"%s\", %s, %s, %s, %s, %s, %d},\n",
                    tp.name.c_str(), tp.kind == TexParamKind::Ramp ? "true" : "false",
                    defLit.c_str(), tp.hasRange ? "true" : "false", minLit.c_str(),
                    maxLit.c_str(), tp.offset);
      write(buf);
    }
    write("};\n\n");
  }

  static bool exprUsesGrad(const Expr *e)
  {
    if (!e)
      return false;
    if (e->kind == ExprKind::Call && std::strcmp(e->name.c_str(), "grad") == 0)
      return true;
    if (exprUsesGrad(e->lhs.get()) || exprUsesGrad(e->rhs.get()))
      return true;
    for (const auto &a : e->args)
      if (exprUsesGrad(a.get()))
        return true;
    return false;
  }
  static bool stmtUsesGrad(const Stmt *s)
  {
    if (!s)
      return false;
    if (exprUsesGrad(s->expr.get()) || exprUsesGrad(s->cond.get()) ||
        exprUsesGrad(s->lvalue.get()) || exprUsesGrad(s->rvalue.get()))
      return true;
    for (const auto &c : s->stmts)
      if (stmtUsesGrad(c.get()))
        return true;
    return stmtUsesGrad(s->thenBranch.get()) || stmtUsesGrad(s->elseBranch.get()) ||
           stmtUsesGrad(s->forInit.get()) || stmtUsesGrad(s->forStep.get());
  }
  bool brushUsesGrad() const
  {
    for (const auto &st : brush->stages)
      if (stmtUsesGrad(st.body.get()))
        return true;
    return false;
  }

  bool brushUsesNeighbor() const
  {
    return brushUsesNeighborLoop(*brush);
  }

  // Emit a `face` stage as the brush's primary kernel: walk the node's faces
  // (BasicFaceIter, which exposes f.center/f.no + bound face attrs) and run the
  // DSL body. Face attribute writes don't move geometry, so the node is flagged
  // for GPU re-upload only. Reduce/host/neighbor are not supported on the face
  // stage yet (poly-group paint needs none).
  void emitFaceKernel(const string &lowerName)
  {
    // AccMode is unused by the face stage (no vertex proxy / for_neighbor) but
    // is part of the signature so the create-fn can stamp def.exec uniformly.
    write("template <CommandTypes TYPES, sculptcore::brush::AccumMode AccMode>\n");
    write("static void ");
    write(lowerName);
    write("(CommandCtx<TYPES> &ctx)\n");
    write("{\n");
    write("  using namespace sculptcore::spatial;\n");
    write("  using namespace litestl::math;\n");
    write("  bool any_changed = false;\n");
    emitTexCtxLocal();
    for (const auto &f : brush->fields) {
      if (f.kind != FieldKind::Attr || f.domain != AttrDomain::Face)
        continue;
      write("  auto *__fattr_");
      write(f.name);
      write(" = ctx.template boundAttr<");
      write(attrCppType(f.type));
      write(">(\"");
      write(f.name);
      write("\"); (void)__fattr_");
      write(f.name);
      write(";\n");
    }
    write("  for (auto &");
    write(faceParamName);
    write(" : ctx.faceIter(ctx.node)) {\n");
    indent = 2;
    currentStage = faceStage;
    if (faceStage->body && faceStage->body->kind == StmtKind::Block) {
      int savedLocals = (int)locals.size();
      for (const auto &c : faceStage->body->stmts)
        emitStmt(*c);
      while ((int)locals.size() > savedLocals)
        locals.pop_back();
    }
    writeIndent();
    write("any_changed = true;\n");
    currentStage = nullptr;
    indent = 0;
    write("  }\n");
    write("  if (any_changed) {\n");
    if (kernelWritesGeomOnly()) {
      write("    ctx.node.update(Spatial_UpdateGPUGeom | Spatial_RegenBounds);\n");
    } else {
      write("    ctx.node.update(Spatial_UpdateGPU | Spatial_RegenBounds);\n");
    }
    write("  }\n");
    write("}\n\n");
  }

  // True when the kernel's write set (its `save` declarations; empty defaults
  // to {v.co, v.no, f.no}) touches only geometry — such kernels flag the
  // narrower Spatial_UpdateGPUGeom so the slice update skips attr-stream fills.
  // @paint brushes write attribute streams by definition (they may not `save`
  // them, e.g. polygroup) and always take the full-refresh flag.
  bool kernelWritesGeomOnly() const
  {
    if (brush->isPaint) {
      return false;
    }
    if (brush->saves.size() == 0) {
      return true;
    }
    for (const auto &s : brush->saves) {
      const char *n = s.name.c_str();
      const bool geom = (s.domain == AttrDomain::Vertex &&
                         (std::strcmp(n, "co") == 0 || std::strcmp(n, "no") == 0)) ||
                        (s.domain == AttrDomain::Face && std::strcmp(n, "no") == 0);
      if (!geom) {
        return false;
      }
    }
    return true;
  }

  // Map a `save` name to its undo-flag bit expression. co/no/mask/color are
  // standard categories; anything else gets a fresh custom bit. customIdx is
  // bumped for each custom attr so bits stay distinct within the 16-bit field.
  string saveFlagExpr(const char *nm, int &customIdx)
  {
    if (std::strcmp(nm, "co") == 0)
      return string("sculptcore::meshlog::CO");
    if (std::strcmp(nm, "no") == 0)
      return string("sculptcore::meshlog::NO");
    if (std::strcmp(nm, "mask") == 0)
      return string("sculptcore::meshlog::MASK");
    if (std::strcmp(nm, "color") == 0)
      return string("sculptcore::meshlog::COLOR");
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "(1 << (sculptcore::meshlog::CUSTOM_START + %d))", customIdx++);
    return string(buf);
  }

  // Emit one CaptureSaveDesc initializer for a `save`: its field kind, the attr
  // handle (declared attrs only), and the codegen-fixed undo-flag bits. The
  // resolve/capture work itself lives in TYPES::capture_policy (the domain
  // seam) — see brush/capture_policy.h.
  void emitSaveDesc(const SaveAttr &sv, AttrDomain dom, int &customIdx)
  {
    const char *nm = sv.name.c_str();
    string flag = saveFlagExpr(nm, customIdx);

    write("    {sculptcore::brush::CaptureField::");
    if (std::strcmp(nm, "co") == 0) {
      if (dom != AttrDomain::Vertex)
        err("save: 'co' is only valid on the vertex domain");
      write("Co, nullptr, ");
    } else if (std::strcmp(nm, "no") == 0) {
      write("No, nullptr, ");
    } else if (std::strcmp(nm, "mask") == 0) {
      write("Mask, nullptr, ");
    } else {
      if (!findAttrField(stringref(nm)))
        err("save: unknown attribute (not builtin co/no/mask, nor a declared attr)");
      write("Attr, \"");
      write(nm);
      write("\", ");
    }
    write("int(");
    write(flag);
    write(")},\n");
  }

  // Pre-stage: undo capture through TYPES::capture_policy. The `save` set
  // lowers to per-domain CaptureSaveDesc arrays; the policy resolves them and
  // captures (the mesh policy is the historical AttrSaver + parallelCapture
  // block). An empty `save` set defaults to {vertex co, vertex no, face no}.
  void emitPreStage(const string &lowerName)
  {
    Vector<SaveAttr> saves;
    if (brush->saves.size() == 0) {
      saves.append(SaveAttr{AttrDomain::Vertex, string("co")});
      saves.append(SaveAttr{AttrDomain::Vertex, string("no")});
      saves.append(SaveAttr{AttrDomain::Face, string("no")});
    } else {
      for (const auto &s : brush->saves)
        saves.append(s);
    }

    write("template <CommandTypes TYPES>\n");
    write("static void ");
    write(lowerName);
    write("Pre(CommandCtxBase &ctx, std::span<typename TYPES::node_type *> nodes)\n");
    write("{\n");

    int customIdx = 0;
    for (AttrDomain dom : {AttrDomain::Vertex, AttrDomain::Face}) {
      Vector<const SaveAttr *> domSaves;
      for (const auto &s : saves) {
        if (s.domain == dom)
          domSaves.append(&s);
      }
      if (domSaves.size() == 0)
        continue;

      const char *domEnum = (dom == AttrDomain::Vertex) ? "VERTEX" : "FACE";
      const char *arr = (dom == AttrDomain::Vertex) ? "__vsaves" : "__fsaves";

      write("  static const sculptcore::brush::CaptureSaveDesc ");
      write(arr);
      write("[] = {\n");
      for (const SaveAttr *s : domSaves)
        emitSaveDesc(*s, dom, customIdx);
      write("  };\n");
      write("  TYPES::capture_policy::template capture<sculptcore::mesh::ElemType::");
      write(domEnum);
      write(">(\n      ctx, nodes, std::span<const sculptcore::brush::CaptureSaveDesc>(");
      write(arr);
      write("));\n");
    }
    for (const auto &s : saves) {
      if (s.domain != AttrDomain::Vertex && s.domain != AttrDomain::Face)
        err("save: only vertex and face domains are supported");
    }
    write("}\n\n");
  }

  void run()
  {
    string lowerName =
        lower(string(brush->attrName.size() > 0 ? brush->attrName : brush->cppName));
    string camelName = capitalize(lowerName);

    // for_neighbor brushes are templated on a NbrSource policy (live disk walk
    // vs cached CSR), chosen once at brush-command creation. Detect it up front
    // so the includes and function signatures can be emitted accordingly.
    const bool usesNbr = brushUsesNeighbor();

    write("// AUTO-GENERATED by sbrushc — DO NOT EDIT.\n");
    write("// Source: ");
    write(brush->sourceFile);
    write("\n");
    write("#pragma once\n");
    write("#include \"brush/brush_command.h\"\n");
    write("#include \"brush/capture_policy.h\"\n");
    write("#include \"spatial/spatial_enums.h\"\n");
    write("#include \"mesh/mesh_iter.h\"\n");
    if (brush->textures.size() > 0)
      write("#include \"brush/texture_eval.h\"\n");
    if (usesNbr)
      write("#include \"brush/neighbor_source.h\"\n");
    write("\n");
    write("namespace sculptcore::brush::command {\n\n");

    // Struct decls — at namespace scope so reduce/vertex functions can
    // both name them. Skip if the brush declared none.
    for (const auto &sd : brush->structs) {
      write("struct ");
      write(sd.name);
      write(" {\n");
      for (const auto &f : sd.fields) {
        write("  ");
        write(typeKindName(f.type));
        write(" ");
        write(f.name);
        write(";\n");
      }
      write("};\n\n");
    }

    // Forward-mode dual prelude — emitted before bodies so the inlined grad()
    // rewrite resolves. A scalar dual is (v, ∂v); a float3 dual carries a 3-col
    // Jacobian. Overloads + sbd_* chain rules drive the rewrite in emitDual.
    if (brushUsesGrad()) {
      write("struct sbdual { float v; float3 d; };\n");
      write("struct sbdual3 { float3 v; float3 dx, dy, dz; };\n");
      write("inline sbdual sb_c(float x){return {x,float3(0,0,0)};}\n");
      write("inline sbdual3 sb_c3(float3 p){return "
            "{p,float3(0,0,0),float3(0,0,0),float3(0,0,0)};}\n");
      write("inline sbdual3 sb_seed3(float3 p){return "
            "{p,float3(1,0,0),float3(0,1,0),float3(0,0,1)};}\n");
      write("inline sbdual sb_comp(sbdual3 a,int i){return "
            "{a.v[i],float3(a.dx[i],a.dy[i],a.dz[i])};}\n");
      write("inline sbdual3 sb_v3(sbdual x,sbdual y,sbdual z){return "
            "{float3(x.v,y.v,z.v),float3(x.d[0],y.d[0],z.d[0]),float3(x.d[1],y.d[1],z.d["
            "1]),float3(x.d[2],y.d[2],z.d[2])};}\n");
      write("inline sbdual operator+(sbdual a,sbdual b){return {a.v+b.v,a.d+b.d};}\n");
      write("inline sbdual operator-(sbdual a,sbdual b){return {a.v-b.v,a.d-b.d};}\n");
      write("inline sbdual operator-(sbdual a){return {-a.v,-a.d};}\n");
      write("inline sbdual operator*(sbdual a,sbdual b){return "
            "{a.v*b.v,a.d*b.v+b.d*a.v};}\n");
      write("inline sbdual operator/(sbdual a,sbdual b){return "
            "{a.v/b.v,(a.d*b.v-b.d*a.v)/(b.v*b.v)};}\n");
      write(
          "inline sbdual sbd_sin(sbdual a){return {std::sin(a.v),a.d*std::cos(a.v)};}\n");
      write("inline sbdual sbd_cos(sbdual a){return "
            "{std::cos(a.v),a.d*(-std::sin(a.v))};}\n");
      write("inline sbdual sbd_sqrt(sbdual a){float r=std::sqrt(a.v);return "
            "{r,a.d*(r>0?0.5f/r:0.0f)};}\n");
      write("inline sbdual sbd_abs(sbdual a){return "
            "{std::abs(a.v),a.d*(a.v<0?-1.0f:1.0f)};}\n");
      write("inline sbdual sbd_dot(sbdual3 a,sbdual3 b){return "
            "{a.v.dot(b.v),a.dx*b.v.x+b.dx*a.v.x+a.dy*b.v.y+b.dy*a.v.y+a.dz*b.v.z+b.dz*a."
            "v.z};}\n");
      write("inline sbdual sbd_length(sbdual3 a){return sbd_sqrt(sbd_dot(a,a));}\n");
      write("inline sbdual sbd_mix(sbdual a,sbdual b,sbdual t){return a+(b-a)*t;}\n\n");
    }

    // Texture eval functions (inline + `use texture` imports) — pure, at
    // namespace scope so the vertex/reduce bodies can call them. Each is
    // preceded by its param-defaults slab when it declares runtime params.
    for (const auto &td : brush->textures) {
      emitTextureBlock(td);
    }

    // pre-stage: AttrSaver-gated undo capture, driven by the brush's `save` set.
    emitPreStage(lowerName);

    // Host stages — CPU-only setup that runs before any per-node work
    // for a dab. Emitted first so reduce/vertex (which may read ctx
    // fields the host populated) can rely on its side effects.
    Vector<const Stage *> hostStages;
    for (const auto &st : brush->stages) {
      if (st.kind == StageKind::Host)
        hostStages.append(&st);
    }
    for (const auto *st : hostStages) {
      emitHostStage(*st, lowerName);
    }

    // Reduce stages — emitted before the vertex stage so the vertex
    // function can call them by name.
    Vector<const Stage *> reduceStages;
    for (const auto &st : brush->stages) {
      if (st.kind == StageKind::Reduce)
        reduceStages.append(&st);
    }
    for (const auto *st : reduceStages) {
      emitReduceStage(*st, lowerName);
    }

    // Primary stage: a vertex kernel (walks the node's verts) or — for face
    // brushes like poly-group paint — a face kernel (walks the node's faces).
    if (!vertexStage && !faceStage) {
      err("brush has no vertex or face stage");
      return;
    }
    if (faceStage && !vertexStage) {
      emitFaceKernel(lowerName);
    } else {
      if (vertexStage->params.size() < 1) {
        err("vertex stage must take at least one parameter (the Vertex bundle)");
      }

      if (usesNbr) {
        write("template <CommandTypes TYPES, sculptcore::brush::NbrSource NbrSrc, "
              "sculptcore::brush::AccumMode AccMode>\n");
      } else {
        write("template <CommandTypes TYPES, sculptcore::brush::AccumMode AccMode>\n");
      }
      write("static void ");
      write(lowerName);
      write("(CommandCtx<TYPES> &ctx)\n");
      write("{\n");
      write("  using namespace sculptcore::spatial;\n");
      write("  using namespace litestl::math;\n");
      write("  bool any_moved = false;\n");
      emitTexCtxLocal();

      // Bound attribute handles (resolved per-dab in the executor). A handle is
      // null only for an optional layer that was absent; write kernels declare
      // their target attr so it's always present here.
      for (const auto &f : brush->fields) {
        if (f.kind != FieldKind::Attr)
          continue;
        if (f.domain != AttrDomain::Vertex)
          continue; // vertex stage: vertex attrs
        write("  auto *__attr_");
        write(f.name);
        write(" = ctx.template boundAttr<");
        write(attrCppType(f.type));
        write(">(\"");
        write(f.name);
        write("\"); (void)__attr_");
        write(f.name);
        write(";\n");
      }

      // Non-Vertex vertex params get declared as locals and seeded by the
      // matching reduce-stage output (matched by param name). The vertex
      // body then sees them as ordinary stage params — struct, scalar,
      // or vector. Struct locals are default-constructed; scalars stay
      // uninitialized until the reduce call runs (the executor always
      // calls every reduce stage before the per-vertex loop).
      for (int pi = 1; pi < (int)vertexStage->params.size(); pi++) {
        const auto &p = vertexStage->params[pi];
        write("  ");
        emitTypeRef(p.type, p.structName);
        write(" ");
        write(p.name);
        write(";\n");
      }
      // Call each reduce stage in source order. Argument matching is
      // by-name to a vertex-stage local declared above; both struct
      // and scalar params are passed by reference at the C++ level
      // (the reduce signature already declares scalars as `T &` for
      // out/inout, by value for in — the call site looks the same).
      for (const auto *st : reduceStages) {
        write("  ");
        write(lowerName);
        write(capitalize(st->name));
        write("<TYPES>(ctx");
        for (const auto &rp : st->params) {
          write(", ");
          bool found = false;
          for (int pi = 1; pi < (int)vertexStage->params.size(); pi++) {
            const auto &vp = vertexStage->params[pi];
            if (vp.type != rp.type)
              continue;
            if (!string(vp.name).operator==(string(rp.name.c_str())))
              continue;
            if (rp.type == TypeKind::Struct &&
                !string(vp.structName).operator==(string(rp.structName.c_str())))
              continue;
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

      write("  for (auto &");
      write(vertexParamName);
      write(" : ctx.template vertexIter<AccMode>(ctx.node)) {\n");
      indent = 2;
      currentStage = vertexStage;

      // user body
      if (vertexStage->body && vertexStage->body->kind == StmtKind::Block) {
        int savedLocals = (int)locals.size();
        for (const auto &c : vertexStage->body->stmts)
          emitStmt(*c);
        while ((int)locals.size() > savedLocals)
          locals.pop_back();
      }

      // post-iteration side effects: ran only when body did not continue/return.
      writeIndent();
      write("ctx.node.affected_verts.append(");
      write(vertexParamName);
      write(".v);\n");
      writeIndent();
      write("any_moved = true;\n");

      currentStage = nullptr;
      indent = 0;
      write("  }\n");
      write("  if (any_moved) {\n");
      if (kernelWritesGeomOnly()) {
        write("    ctx.node.update(Spatial_UpdateNormals | Spatial_UpdateGPUGeom | "
              "Spatial_RegenBounds);\n");
      } else {
        write("    ctx.node.update(Spatial_UpdateNormals | Spatial_UpdateGPU | "
              "Spatial_RegenBounds);\n");
      }
      write("  }\n");
      write("}\n\n");
    } // end vertex/face primary-kernel branch

    // post-stage: empty for Wave 1.
    write("template <CommandTypes TYPES>\n");
    write("static void ");
    write(lowerName);
    write("Post(CommandCtxBase &ctx, std::span<typename TYPES::node_type *> nodes)\n");
    write("{\n");
    write("  (void)ctx; (void)nodes;\n");
    write("}\n\n");

    // wire-up function: identical shape to existing createDrawBrush. For
    // for_neighbor brushes it carries the NbrSource policy (default: live disk
    // walk) through to the kernel instantiation assigned to def.exec.
    if (usesNbr) {
      write("template <CommandTypes TYPES, sculptcore::brush::NbrSource NbrSrc = "
            "sculptcore::brush::LiveDiskNbr, "
            "sculptcore::brush::AccumMode AccMode = sculptcore::brush::AccumLive>\n");
    } else {
      write("template <CommandTypes TYPES, "
            "sculptcore::brush::AccumMode AccMode = sculptcore::brush::AccumLive>\n");
    }
    write("static void create");
    write(camelName);
    write("Brush(BrushCommandDef<CommandCtx<TYPES>> &def)\n");
    write("{\n");
    // Host stages, if any, are composed into a single lambda so multiple
    // hosts on one brush still flow through one execHost slot.
    if (hostStages.size() > 0) {
      write("  def.execHost = [](CommandCtxBase &ctx, Brush &brush) {\n");
      for (const auto *st : hostStages) {
        write("    ");
        write(lowerName);
        write(capitalize(st->name));
        write("<TYPES>(ctx, brush);\n");
      }
      write("  };\n");
    }
    write("  def.execPre  = ");
    write(lowerName);
    write("Pre<TYPES>;\n");
    write("  def.exec     = ");
    write(lowerName);
    if (usesNbr)
      write("<TYPES, NbrSrc, AccMode>;\n");
    else
      write("<TYPES, AccMode>;\n");
    write("  def.execPost = ");
    write(lowerName);
    write("Post<TYPES>;\n");
    // for_neighbor reads ctx.co_prev — tell the executor to snapshot it.
    if (neighborLoopUsed) {
      write("  def.needsCoPrev = true;\n");
    }
    // Non-accumulate eligibility (see plans/nonAccumMode.md): accumulable is the
    // default; the executor runs the AccumOrig instantiation when non-accumulate
    // mode is on for the stroke. @paint writes attributes and @unbounded fields
    // are anchored, so from-base re-derivation is meaningless for both; an
    // @incremental kernel is driven by a per-dab delta, so it has no base.
    write("  def.accumulable = ");
    write((!brush->isPaint && !brush->isUnbounded && !brush->isIncremental) ? "true" : "false");
    write(";\n");
    // `@relaxation`: relaxes the surface rather than displacing it, so it stays
    // on AccumLive and never accumulates into `.brush.disp.vec`.
    if (brush->isRelaxation) {
      write("  def.relaxesBase = true;\n");
    }
    // `@grabmode`: eligible for the from-orig grab policy. The executor decides
    // whether the stroke actually uses it (CommandExecutor::anchoredGrab).
    if (brush->isGrabMode) {
      write("  def.grabModeCapable = true;\n");
    }
    // `@unbounded`: the field is live out to radius * unboundedExtent, which the
    // executor uses as the node-filter floor so no leaf seam falls inside it.
    if (brush->isUnbounded) {
      write("  def.unbounded = true;\n");
    }
    // `@incremental`: the host must feed the per-dab delta this kernel expects
    // (accumulable is already false above — this says *why*, which is what the
    // host needs to shape the dab).
    if (brush->isIncremental) {
      write("  def.incremental = true;\n");
    }
    // A `face` stage means the kernel is dispatched per-face, not per-vertex.
    if (brushHasFaceStage(*brush)) {
      write("  def.faceMode = true;\n");
    }
    // `v.mask` is a builtin Vertex field, so unlike a painted attr it leaves no
    // trace in def.attrs — scan the bodies for the write instead.
    if (brushWritesMember(*brush, "mask")) {
      write("  def.writesMask = true;\n");
    }
    // Declared attribute layers — resolved + bound per dab by the executor.
    for (const auto &f : brush->fields) {
      if (f.kind != FieldKind::Attr)
        continue;
      write("  def.attrs.append(sculptcore::brush::BrushAttrManifestEntry{\"");
      write(f.name);
      write("\", \"");
      write(f.boundName);
      write("\", ");
      write(attrTypeEnum(f.type));
      write(", ");
      write(attrDomainEnum(f.domain));
      write(", true, ");
      write(attrUseEnum(f.use));
      write("});\n");
    }
    // Declared uniforms — the executor registers these as props and applies
    // device dynamics each dab (see sbrush-dynamic-uniforms plan). Carries the
    // DSL `= <n>` default, `@range(a,b)`, and `@static` opt-out.
    for (const auto &f : brush->fields) {
      if (f.kind != FieldKind::Uniform)
        continue;
      bool isFloat = f.type == TypeKind::Float;
      bool dynamic = isFloat && f.dynamicCapable;
      double def = f.hasDefault ? f.defaultValue : 0.0;
      write("  def.uniforms.append(sculptcore::brush::BrushUniformManifestEntry{\"");
      write(f.name);
      write("\", ");
      write(isFloat ? "true" : "false"); // isFloat
      write(", ");
      write(dynamic ? "true" : "false"); // dynamic
      write(", ");
      write(floatLit(def)); // def
      write(", ");
      write(f.hasRange ? "true" : "false"); // hasRange
      write(", ");
      write(floatLit(f.hasRange ? f.rangeMin : 0.0));
      write(", ");
      write(floatLit(f.hasRange ? f.rangeMax : 0.0));
      if (extrasMode && fieldUsesStore(f)) {
        write(", kExtraSlot_");
        write(f.name); // storeSlot (-1 default for member-backed entries)
      }
      write("});\n");
    }
    // Generated prop wiring: register this kernel's scalar-float uniforms as
    // props (idempotent, authored default seeded) and resolve them each dab
    // applying device dynamics into the cached Brush members the kernel reads.
    // Replaces the hand-written structDef_/loadProps lists for these fields; the
    // fixed common props (strength/radius/...) stay on Brush. Non-float uniforms
    // remain plain host-set members (no prop).
    write("  def.registerProps = [](sculptcore::props::StructDef &sd) {\n");
    for (const auto &f : brush->fields) {
      if (f.kind != FieldKind::Uniform || f.type != TypeKind::Float || !f.dynamicCapable)
        continue;
      double def = f.hasDefault ? f.defaultValue : 0.0;
      write("    if (!sd.has(\"");
      write(f.name);
      write("\")) sd.Float32(\"");
      write(f.name);
      write("\", \"");
      write(f.name);
      write("\").Default(");
      write(floatLit(def));
      write(");\n");
    }
    write("  };\n");
    write("  def.loadUniformProps = [](sculptcore::brush::Brush &brush, "
          "sculptcore::props::DeviceInputCtx *ctx) {\n");
    for (const auto &f : brush->fields) {
      if (f.kind != FieldKind::Uniform || f.type != TypeKind::Float || !f.dynamicCapable)
        continue;
      double def = f.hasDefault ? f.defaultValue : 0.0;
      if (extrasMode && fieldUsesStore(f)) {
        // Store uniforms resolve dynamics into their slot (setNamedFloat
        // sizes defensively; ensureExtraUniformDefaults ran at creation).
        write("    brush.setNamedFloat(kExtraSlot_");
        write(f.name);
        write(", brush.props.lookupValue<float>(\"");
        write(f.name);
        write("\", ");
        write(floatLit(def));
        write(", ctx));\n");
        continue;
      }
      write("    brush.");
      write(f.name);
      write(" = brush.props.lookupValue<float>(\"");
      write(f.name);
      write("\", ");
      write(floatLit(def));
      write(", ctx);\n");
    }
    write("  };\n");
    write("}\n\n");

    write("} // namespace sculptcore::brush::command\n");
  }
};

} // namespace

EmitResult emitCpp(const Brush &brush, const CppEmitOptions &opts)
{
  Emit em;
  em.brush = &brush;
  em.extrasMode = opts.extras;

  // Uniform resolution gate. Built-in kernels: every uniform / non-builtin
  // ctx field must be member-backed (Brush::builtinPropNames) — the honesty
  // tripwire that keeps the list in sync with what kernels read. Extra
  // kernels: unlisted scalar floats fall through to the named store; any
  // other unlisted type still needs an engine-side member.
  for (const auto &f : brush.fields) {
    if (!fieldUsesStore(f)) {
      continue;
    }
    char buf[256];
    if (!opts.extras) {
      std::snprintf(buf, sizeof(buf),
                    "uniform '%s' does not resolve to a Brush member — add the member "
                    "and list it in Brush::builtinPropNames (brush.h)",
                    f.name.c_str());
      em.errors.append(string(buf));
    } else if (f.type != TypeKind::Float) {
      std::snprintf(buf, sizeof(buf),
                    "extra kernel uniform '%s': only scalar floats can use the named "
                    "store; a %s uniform needs an existing Brush member",
                    f.name.c_str(), typeKindName(f.type));
      em.errors.append(string(buf));
    }
  }
  if (em.errors.size() > 0) {
    EmitResult r;
    r.errors = std::move(em.errors);
    return r;
  }

  for (const auto &st : brush.stages) {
    if (st.kind == StageKind::Vertex) {
      em.vertexStage = &st;
      break;
    }
  }
  for (const auto &st : brush.stages) {
    if (st.kind == StageKind::Face) {
      em.faceStage = &st;
      break;
    }
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

EmitResult emitCppTextureDefs(const Brush &brush)
{
  Emit em;
  em.brush = &brush;
  for (const auto &td : brush.textures) {
    em.emitTextureBlock(td);
    em.emitTextureManifest(td);
  }
  EmitResult r;
  r.text = std::move(em.out);
  r.errors = std::move(em.errors);
  return r;
}

} // namespace sculptcore::brush::sbrush
