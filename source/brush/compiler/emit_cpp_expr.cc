/** Expression lowering: identifier resolution (locals, stage params, attribute
 * bundles, texture params), type inference, emitExpr and the forward-mode
 * dual rewrite behind grad(). */

#include "emit_cpp_internal.h"

namespace sculptcore::brush::sbrush::cpp_emit {

bool Emit::isLocal(stringref name) const
{
  for (const auto &l : locals) {
    if (string(l.name).operator==(string(name.c_str())))
      return true;
  }
  return false;
}

bool Emit::scalarOrVectorValue(TypeKind type)
{
  return type == TypeKind::Float || type == TypeKind::Int || type == TypeKind::Bool ||
         type == TypeKind::Float2 || type == TypeKind::Float3 ||
         type == TypeKind::Float4;
}

bool Emit::preparedAssignmentSafe(const Expr &expression) const
{
  const Expr *root = &expression;
  while (root->kind == ExprKind::Member || root->kind == ExprKind::Index ||
         root->kind == ExprKind::Paren)
  {
    root = root->lhs.get();
  }
  if (root->kind != ExprKind::Ident)
    return false;
  for (const auto &neighbor : nbrBundles)
    if (root->name == neighbor)
      return false;
  // Lexical locals win over fields/parameters, just as identifier emission does.
  for (size_t i = locals.size(); i > 0; i--)
    if (root->name == locals[i - 1].name)
      return scalarOrVectorValue(locals[i - 1].type);
  if (currentTexture && currentStage) {
    for (const auto &param : currentStage->params)
      if (root->name == param.name)
        return scalarOrVectorValue(param.type);
  }
  const bool vertex = currentStage == vertexStage && root->name == vertexParamName;
  const bool face = currentStage == faceStage && root->name == faceParamName;
  if (!vertex && !face)
    return false;
  // Bundles also expose executor/context references. Only saved members and
  // numeric component/index suffixes qualify.
  const Expr *member = &expression;
  while (member->kind == ExprKind::Paren || member->kind == ExprKind::Index ||
         (member->kind == ExprKind::Member &&
          (member->name == string("x") || member->name == string("y") ||
           member->name == string("z") || member->name == string("w"))))
  {
    member = member->lhs.get();
  }
  if (member->kind != ExprKind::Member)
    return false;
  bool saved = false;
  const auto domain = vertex ? AttrDomain::Vertex : AttrDomain::Face;
  if (brush->saves.size() == 0)
    saved |= member->name == string("no") || (vertex && member->name == string("co"));
  for (const auto &save : brush->saves)
    saved |= save.domain == domain && save.name == member->name;
  if (!saved)
    return false;
  const Expr *base = member->lhs.get();
  while (base->kind == ExprKind::Paren)
    base = base->lhs.get();
  return base == root;
}

bool Emit::isDualLocal(stringref name) const
{
  for (const auto &l : locals) {
    if (l.dual && string(l.name).operator==(string(name.c_str())))
      return true;
  }
  return false;
}

const Field *Emit::findField(stringref name) const
{
  for (const auto &f : brush->fields) {
    if (string(f.name).operator==(string(name.c_str())))
      return &f;
  }
  return nullptr;
}

// Find a declared `attr` field by handle name (any domain).
const Field *Emit::findAttrField(stringref name) const
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
bool Emit::bundleInfo(stringref name, const char *&idxField, const char *&prefix) const
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
const TextureDef *Emit::findTextureCall(stringref callName) const
{
  for (const auto &t : brush->textures) {
    string full = t.name + ".eval";
    if (string(full).operator==(string(callName.c_str())))
      return &t;
  }
  return nullptr;
}

// Resolve a `param` name in the texture currently being lowered.
const TexParam *Emit::findTexParam(stringref name) const
{
  if (!currentTexture)
    return nullptr;
  for (const auto &tp : currentTexture->texParams) {
    if (string(tp.name).operator==(string(name.c_str())))
      return &tp;
  }
  return nullptr;
}

bool Emit::isStageParam(stringref name) const
{
  if (!currentStage)
    return false;
  for (const auto &p : currentStage->params) {
    if (string(p.name).operator==(string(name.c_str())))
      return true;
  }
  return false;
}

bool Emit::isVectorType(TypeKind t)
{
  return t == TypeKind::Float2 || t == TypeKind::Float3 || t == TypeKind::Float4;
}

// Component index for a `.x`/`.y`/`.z`/`.w` swizzle, or -1 otherwise.
int Emit::swizzleIndex(const string &name)
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
TypeKind Emit::resolveExprType(const Expr &e) const
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
    return e.lhs && isVectorType(resolveExprType(*e.lhs)) ? TypeKind::Float
                                                          : TypeKind::Unknown;
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

void Emit::emitExpr(const Expr &e)
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
    if (dualBody && isDualLocal(nm)) {
      // Value-context read of a dual name inside an EvalD body.
      out += e.name;
      out += ".v";
      break;
    }
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
      // their registry-assigned typed slot.
      bool isCtxBase = isCtxBaseName(e.name.c_str());
      bool inHost = (currentStage && currentStage->kind == StageKind::Host);
      if (f->kind == FieldKind::Ctx && isCtxBase) {
        out += "ctx.";
        out += e.name;
      } else if (extrasMode && fieldUsesStore(*f)) {
        out += inHost ? "brush.named" : "ctx.brush.named";
        out += storeSuffix(f->type);
        out += "s[kExtraSlot_";
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
      if (dualBody) {
        err("grad() cannot appear inside a texture eval differentiated by grad()");
        out += "float3(0.0f, 0.0f, 0.0f)";
        break;
      }
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
          std::snprintf(
              buf, sizeof(buf), "texRampSample(sb_tex_params + %d, ", tp->offset);
          out += buf;
          emitExpr(*e.args[0]);
          out += ")";
          break;
        }
      }
      // Host samplers resolve only on the runtime JIT path (T4); the
      // precompiled cpp backend has no registry to bind against.
      bool wasSampler = false;
      for (const auto &sm : currentTexture->samplerDeps) {
        if (string(sm).operator==(string(e.name.c_str()))) {
          errf("sampler '%s' is runtime-only; cannot precompile", e.name.c_str());
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
      if (e.name != string("float") && e.name != string("int") &&
          e.name != string("bool") && e.name != string("float2") &&
          e.name != string("float3") && e.name != string("float4"))
      {
        preparedStateUnsafe = true;
      }
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
string Emit::render(const Expr &e)
{
  string saved = out;
  out = string("");
  emitExpr(e);
  string r = out;
  out = saved;
  return r;
}

bool Emit::isGradVar(const Expr &e)
{
  string r = render(e);
  return string(r).operator==(string(gradVar.c_str()));
}

void Emit::emitDual(const Expr &e)
{
  // EvalD-body params/locals are already dual — read them verbatim;
  // reseeding would discard the caller's incoming Jacobian.
  if (dualBody && e.kind == ExprKind::Ident && isDualLocal(stringref(e.name.c_str()))) {
    out += e.name;
    return;
  }
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
    // In an EvalD body the only non-dual idents are texture params —
    // scalars, constant wrt the seed.
    out += dualBody ? "sb_c(" : "sb_c3(";
    emitExpr(e);
    out += ")";
    break; // zero-deriv const
  case ExprKind::Member:
    if (dualBody && e.lhs && e.lhs->kind == ExprKind::Ident &&
        isDualLocal(stringref(e.lhs->name.c_str())))
    {
      // dual3.x/y/z picks a Jacobian row of the already-dual name.
      out += "sb_comp(";
      out += e.lhs->name;
      out += ", ";
      out += (std::strcmp(e.name.c_str(), "x") == 0   ? "0"
              : std::strcmp(e.name.c_str(), "y") == 0 ? "1"
                                                      : "2");
      out += ")";
    } else if (e.lhs && isGradVar(*e.lhs)) { // var.x/y/z picks a Jacobian row
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
    // Texture call under grad() — dispatch to the statement-level dual
    // eval (texture-scripts T2). Args arrive as duals already seeded by
    // the caller; the body propagates without reseeding.
    if (const TextureDef *td = findTextureCall(stringref(n))) {
      if (currentTexture) {
        errf("texture '%s' cannot call another texture", currentTexture->name.c_str());
      }
      out += "tex";
      out += capitalize(td->name);
      out += "EvalD(";
      for (int i = 0; i < (int)e.args.size(); i++) {
        if (i)
          out += ", ";
        emitDual(*e.args[i]);
      }
      out += ", ";
      out += td->slabSize > 0 ? texDefaultsName(*td) : string("nullptr");
      out += ", ";
      out += td->usesMap ? "sb_texctx" : "nullptr";
      out += ")";
      break;
    }
    if (std::strcmp(n, "mapPoint") == 0 && e.args.size() == 1) {
      if (!currentTexture) {
        err("mapPoint() is only valid inside a texture eval");
      }
      out += "sbd_mapPoint(sb_texctx, ";
      emitDual(*e.args[0]);
      out += ")";
      break;
    }
    if (std::strcmp(n, "grad") == 0) {
      err("nested grad() is not supported");
      out += "sb_c(0.0f)";
      break;
    }
    // Chain-rule coverage is exactly the sbd_* prelude. Everything else —
    // ramp .sample, samplers (T4's sbd_hs_* wrappers), unlisted intrinsics
    // — has no derivative rule yet; error instead of emitting a broken call.
    static const char *const kDualIntrinsics[] = {
        "sin", "cos", "sqrt", "abs", "dot", "length", "mix", "floor", "fract"};
    bool known = false;
    for (const char *k : kDualIntrinsics)
      known = known || std::strcmp(n, k) == 0;
    if (!known) {
      errf("'%s' has no derivative rule inside grad()", n);
      out += "sb_c(0.0f)";
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

} // namespace sculptcore::brush::sbrush::cpp_emit
