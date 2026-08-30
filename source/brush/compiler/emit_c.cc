#include "emit_c.h"
#include "../kernels/ir/intrinsics.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace sculptcore::brush::sbrush {

using litestl::util::string;
using litestl::util::stringref;
using litestl::util::Vector;

namespace {

// C99 has no vector types or operator overloads: float3 lowers to a named
// struct and arithmetic routes through sb3_* helpers by resolved type.
const char *cTypeName(TypeKind k)
{
  switch (k) {
  case TypeKind::Void:
    return "void";
  case TypeKind::Bool:
    return "int";
  case TypeKind::Int:
    return "int";
  case TypeKind::Float:
    return "float";
  case TypeKind::Float3:
    return "float3";
  default:
    return nullptr;
  }
}

bool isVectorType(TypeKind k)
{
  return k == TypeKind::Float2 || k == TypeKind::Float3 || k == TypeKind::Float4;
}

int swizzleIndex(const char *n)
{
  if (!n[0] || n[1])
    return -1;
  switch (n[0]) {
  case 'x':
    return 0;
  case 'y':
    return 1;
  case 'z':
    return 2;
  case 'w':
    return 3;
  }
  return -1;
}

// The texture-safe intrinsic subset with C99 emit patterns. A shared-table
// intrinsic missing here needs brush context and errors out.
struct CIntrinsic {
  const char *name;
  const char *pattern;
};
const CIntrinsic kCIntrinsics[] = {
    {"length", "sc_length($0)"},
    {"dot", "sc_dot($0, $1)"},
    {"normalize", "sc_normalize($0)"},
    {"cross", "sc_cross($0, $1)"},
    {"distance", "sc_length(sb3_sub($1, $0))"},
    {"mix", "(($0) + (($1) - ($0)) * ($2))"},
    {"min", "sb_minf($0, $1)"},
    {"max", "sb_maxf($0, $1)"},
    {"clamp", "sb_clampf($0, $1, $2)"},
    {"abs", "fabsf($0)"},
    {"sqrt", "sqrtf($0)"},
    {"sin", "sinf($0)"},
    {"cos", "cosf($0)"},
    {"floor", "floorf($0)"},
    {"fract", "(($0) - floorf($0))"},
    {"pow", "powf($0, $1)"},
    {"atan2", "atan2f($0, $1)"},
    {"exp", "expf($0)"},
    {"log", "logf($0)"},
    // Helpers, not inline expansions: tcc optimizes nothing, so a pattern
    // that repeats `$0` repeats the work that computed it.
    {"mod", "sb_modf($0, $1)"},
    {"step", "sb_stepf($0, $1)"},
    {"smoothstep", "sb_smoothstepf($0, $1, $2)"},
};

const CIntrinsic *findCIntrinsic(const char *name)
{
  for (const auto &ci : kCIntrinsics) {
    if (std::strcmp(ci.name, name) == 0)
      return &ci;
  }
  return nullptr;
}

void appendFloatLit(string &s, double v)
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

struct Emit {
  const Brush *brush;
  const Stage *currentStage = nullptr;

  string out;
  Vector<string> errors;
  int indent = 0;

  struct LocalVar {
    string name;
    TypeKind type = TypeKind::Unknown;
    // Lowered to sbdual/sbdual3 inside a texture EvalD body (dualBody).
    bool dual = false;
  };
  Vector<LocalVar> locals;

  // Set while emitting a texture eval body — gates texture-calls-texture,
  // texParam lookup and ramp/sampler call handling.
  const TextureDef *currentTexture = nullptr;

  bool gradUsed = false;
  string gradVar; // float3 var being differentiated, rendered
  // True while emitting a texture EvalD body: float/float3 locals lower to
  // sbdual/sbdual3, dual contexts route through emitDual, and emitExpr
  // projects dual names back to `.v`.
  bool dualBody = false;

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
  bool isDualLocal(stringref name) const
  {
    for (const auto &l : locals) {
      if (l.dual && string(l.name).operator==(string(name.c_str())))
        return true;
    }
    return false;
  }
  TypeKind localType(stringref name) const
  {
    for (const auto &l : locals) {
      if (string(l.name).operator==(string(name.c_str())))
        return l.type;
    }
    return TypeKind::Unknown;
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
  TypeKind stageParamType(stringref name) const
  {
    if (!currentStage)
      return TypeKind::Unknown;
    for (const auto &p : currentStage->params) {
      if (string(p.name).operator==(string(name.c_str())))
        return p.type;
    }
    return TypeKind::Unknown;
  }

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
  bool anyTextureUsesSampler() const
  {
    for (const auto &t : brush->textures) {
      if (t.samplerDeps.size() > 0)
        return true;
    }
    return false;
  }

  const SamplerDecl *findSamplerDecl(const char *name) const
  {
    for (const auto &sd : brush->samplers) {
      if (std::strcmp(sd.name.c_str(), name) == 0)
        return &sd;
    }
    return nullptr;
  }
  bool anyTextureUsesRamp() const
  {
    for (const auto &t : brush->textures) {
      for (const auto &tp : t.texParams) {
        if (tp.kind == TexParamKind::Ramp)
          return true;
      }
    }
    return false;
  }

  static string lower(const string &s)
  {
    string r = s;
    for (int i = 0; i < (int)r.size(); i++)
      r[i] = (char)std::tolower((unsigned char)r[i]);
    return r;
  }
  static string texEvalName(const TextureDef &td)
  {
    return string("tex_") + lower(td.name) + "_eval";
  }

  const TextureDef *findTextureCall(stringref callName) const
  {
    for (const auto &t : brush->textures) {
      string full = t.name + ".eval";
      if (string(full).operator==(string(callName.c_str())))
        return &t;
    }
    return nullptr;
  }

  // === type resolution (drives float3-vs-scalar lowering) ===

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
      stringref nm(e.name.c_str());
      for (const auto &l : locals) {
        if (string(l.name).operator==(string(nm.c_str())))
          return l.type;
      }
      if (isStageParam(nm))
        return stageParamType(nm);
      if (const TexParam *tp = findTexParam(nm)) {
        return tp->kind == TexParamKind::Int ? TypeKind::Int : TypeKind::Float;
      }
      return TypeKind::Unknown;
    }
    case ExprKind::Member: {
      if (!e.lhs)
        return TypeKind::Unknown;
      TypeKind bt = resolveExprType(*e.lhs);
      if (isVectorType(bt) && swizzleIndex(e.name.c_str()) >= 0)
        return TypeKind::Float;
      return TypeKind::Unknown;
    }
    case ExprKind::Index:
      return (e.lhs && isVectorType(resolveExprType(*e.lhs))) ? TypeKind::Float
                                                              : TypeKind::Unknown;
    case ExprKind::Unary:
      return e.lhs ? resolveExprType(*e.lhs) : TypeKind::Unknown;
    case ExprKind::Binary: {
      TypeKind a = e.lhs ? resolveExprType(*e.lhs) : TypeKind::Unknown;
      TypeKind b = e.rhs ? resolveExprType(*e.rhs) : TypeKind::Unknown;
      if (isVectorType(a))
        return a;
      if (isVectorType(b))
        return b;
      return a != TypeKind::Unknown ? a : b;
    }
    case ExprKind::Call: {
      const char *n = e.name.c_str();
      if (std::strcmp(n, "float3") == 0)
        return TypeKind::Float3;
      if (const TextureDef *td = findTextureCall(stringref(n)))
        return td->returnType;
      if (std::strcmp(n, "mapPoint") == 0)
        return TypeKind::Float3;
      if (std::strcmp(n, "grad") == 0)
        return TypeKind::Float3;
      const char *dot = std::strchr(n, '.');
      if (dot && std::strcmp(dot, ".sample") == 0)
        return TypeKind::Float;
      if (currentTexture && findSamplerDecl(n))
        return TypeKind::Float;
      if (const IntrinsicDef *intr = findIntrinsic(stringref(n)))
        return intr->returnType;
      return TypeKind::Unknown;
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
      out += e.bvalue ? "1" : "0";
      break;
    case ExprKind::Ident: {
      stringref nm(e.name.c_str());
      if (dualBody && isDualLocal(nm)) {
        // Value context inside an EvalD body — project the dual's value.
        out += e.name;
        out += ".v";
        break;
      }
      if (isLocal(nm) || isStageParam(nm)) {
        out += e.name;
        break;
      }
      if (const TexParam *tp = findTexParam(nm)) {
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
        break;
      }
      out += e.name;
      break;
    }
    case ExprKind::Member: {
      if (e.lhs && isVectorType(resolveExprType(*e.lhs)) &&
          swizzleIndex(e.name.c_str()) == 3)
      {
        err(".w is not supported on the C texture backend (no float4)");
        out += "0.0f";
        break;
      }
      emitExpr(*e.lhs);
      out += ".";
      out += e.name;
      break;
    }
    case ExprKind::Index:
      if (e.lhs && resolveExprType(*e.lhs) == TypeKind::Float3) {
        out += "sb3_idx(";
        emitExpr(*e.lhs);
        out += ", ";
        emitExpr(*e.rhs);
        out += ")";
      } else {
        emitExpr(*e.lhs);
        out += "[";
        emitExpr(*e.rhs);
        out += "]";
      }
      break;
    case ExprKind::Binary: {
      TypeKind at = e.lhs ? resolveExprType(*e.lhs) : TypeKind::Unknown;
      TypeKind bt = e.rhs ? resolveExprType(*e.rhs) : TypeKind::Unknown;
      bool av = at == TypeKind::Float3, bv = bt == TypeKind::Float3;
      bool arith = e.binop == BinOp::Add || e.binop == BinOp::Sub ||
                   e.binop == BinOp::Mul || e.binop == BinOp::Div;
      if ((av || bv) && arith) {
        if (av && bv) {
          const char *f = e.binop == BinOp::Add   ? "sb3_add"
                          : e.binop == BinOp::Sub ? "sb3_sub"
                          : e.binop == BinOp::Mul ? "sb3_mul"
                                                  : "sb3_div";
          out += f;
          out += "(";
          emitExpr(*e.lhs);
          out += ", ";
          emitExpr(*e.rhs);
          out += ")";
        } else if (e.binop == BinOp::Mul) {
          out += "sb3_scale(";
          if (av) {
            emitExpr(*e.lhs);
            out += ", ";
            emitExpr(*e.rhs);
          } else {
            emitExpr(*e.rhs);
            out += ", ";
            emitExpr(*e.lhs);
          }
          out += ")";
        } else if (e.binop == BinOp::Div && av) {
          out += "sb3_divs(";
          emitExpr(*e.lhs);
          out += ", ";
          emitExpr(*e.rhs);
          out += ")";
        } else {
          err("mixed float3/scalar '+', '-' or scalar/float3 '/' is not supported");
          out += "sb_f3(0.0f, 0.0f, 0.0f)";
        }
        break;
      }
      if (av || bv) {
        err("comparison/logical operators on float3 are not supported");
        out += "0";
        break;
      }
      out += "(";
      emitExpr(*e.lhs);
      out += " ";
      out += binOpCSym(e.binop);
      out += " ";
      emitExpr(*e.rhs);
      out += ")";
      break;
    }
    case ExprKind::Unary:
      if (e.unaryop == UnaryOp::Neg && e.lhs &&
          resolveExprType(*e.lhs) == TypeKind::Float3)
      {
        err("unary minus on float3 is not supported (use v * -1.0)");
        out += "sb_f3(0.0f, 0.0f, 0.0f)";
        break;
      }
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
      const char *n = e.name.c_str();
      if (std::strcmp(n, "float3") == 0) {
        if (e.args.size() != 3) {
          err("float3() takes exactly three arguments");
          out += "sb_f3(0.0f, 0.0f, 0.0f)";
          break;
        }
        out += "sb_f3(";
        for (int i = 0; i < 3; i++) {
          if (i)
            out += ", ";
          emitExpr(*e.args[i]);
        }
        out += ")";
        break;
      }
      if (std::strcmp(n, "float2") == 0 || std::strcmp(n, "float4") == 0) {
        errf("%s is not supported on the C texture backend", n);
        out += "0.0f";
        break;
      }
      // grad(expr, var) — forward-mode gradient, dual-number rewrite. C has no
      // operator overloads, so binary ops map to sbd_add/sub/mul/div.
      if (std::strcmp(n, "grad") == 0 && e.args.size() == 2) {
        if (dualBody) {
          err("grad() cannot appear inside a texture eval differentiated by grad()");
          out += "sb_f3(0.0f, 0.0f, 0.0f)";
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
      if (findTextureCall(stringref(e.name.c_str()))) {
        if (currentTexture)
          errf("texture '%s' cannot call another texture", currentTexture->name.c_str());
        else
          err("texture calls are only valid inside a texture eval on the C backend");
        out += "0.0f";
        break;
      }
      if (std::strcmp(n, "mapPoint") == 0) {
        if (!currentTexture)
          err("mapPoint() is only valid inside a texture eval");
        if (e.args.size() != 1) {
          err("mapPoint() takes exactly one float3 argument");
          out += "sb_f3(0.0f, 0.0f, 0.0f)";
          break;
        }
        out += "texMapPoint(sb_texctx, ";
        emitExpr(*e.args[0]);
        out += ")";
        break;
      }
      if (currentTexture) {
        const char *dot = std::strchr(n, '.');
        if (dot && std::strcmp(dot, ".sample") == 0) {
          string base = string(n).substr(0, (int)(dot - n));
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
        if (const SamplerDecl *sd = findSamplerDecl(n)) {
          if (e.args.size() != sd->params.size()) {
            errf("sampler '%s' called with the wrong number of arguments", n);
            out += "0.0f";
            break;
          }
          out += "sb_hs_call(sb_hs_";
          out += n;
          out += ", ";
          emitExpr(*e.args[0]);
          out += ", ";
          if (e.args.size() == 2)
            emitExpr(*e.args[1]);
          else
            out += "sb_f3(0.0f, 0.0f, 0.0f)";
          out += ")";
          break;
        }
        // No carried decl (Brush::samplers) means this is a precompiled
        // path, where host samplers cannot resolve.
        bool isSamplerDep = false;
        for (const auto &dep : currentTexture->samplerDeps) {
          if (std::strcmp(dep.c_str(), n) == 0) {
            isSamplerDep = true;
          }
        }
        if (isSamplerDep) {
          errf("sampler '%s' is runtime-only; cannot precompile", n);
          out += "0.0f";
          break;
        }
      }
      if (std::strcmp(n, "mix") == 0 && e.args.size() == 3 &&
          resolveExprType(*e.args[0]) == TypeKind::Float3)
      {
        out += "sb3_mix(";
        for (int i = 0; i < 3; i++) {
          if (i)
            out += ", ";
          emitExpr(*e.args[i]);
        }
        out += ")";
        break;
      }
      if (const CIntrinsic *ci = findCIntrinsic(n)) {
        Vector<string> rendered;
        for (const auto &a : e.args) {
          rendered.append(render(*a));
        }
        for (const char *p = ci->pattern; *p;) {
          if (*p == '$' && std::isdigit((unsigned char)p[1])) {
            int idx = p[1] - '0';
            p += 2;
            out += (idx < (int)rendered.size()) ? rendered[idx] : string("/*bad-arg*/");
          } else {
            char tmp[2] = {*p, 0};
            out += tmp;
            p++;
          }
        }
        break;
      }
      if (findIntrinsic(stringref(n))) {
        errf("intrinsic '%s' requires brush context and is not available in a texture "
             "script",
             n);
        out += "0.0f";
        break;
      }
      out += e.name;
      out += "(";
      for (int i = 0; i < (int)e.args.size(); i++) {
        if (i > 0)
          out += ", ";
        emitExpr(*e.args[i]);
      }
      out += ")";
      break;
    }
    }
  }

  // Dual-number rewrite for grad(). Same shape as emit_opencl — C lacks
  // operator overloads, so binary ops map to sbd_* functions; var seeds the
  // Jacobian and other terms are zero-deriv constants.
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
    // Inside an EvalD body a dual local already carries its derivative.
    if (dualBody && e.kind == ExprKind::Ident && isDualLocal(stringref(e.name.c_str()))) {
      out += e.name;
      return;
    }
    if (isGradVar(e)) {
      out += "sb_seed3(";
      emitExpr(e);
      out += ")";
      return;
    }
    switch (e.kind) {
    case ExprKind::LitFloat:
    case ExprKind::LitInt:
      out += "sb_c(";
      emitExpr(e);
      out += ")";
      break;
    case ExprKind::Ident:
      out += dualBody ? "sb_c(" : "sb_c3(";
      emitExpr(e);
      out += ")";
      break;
    case ExprKind::Member:
      if (e.lhs && isGradVar(*e.lhs)) {
        out += "sb_comp(sb_seed3(";
        emitExpr(*e.lhs);
        out += "), ";
        out += (std::strcmp(e.name.c_str(), "x") == 0   ? "0"
                : std::strcmp(e.name.c_str(), "y") == 0 ? "1"
                                                        : "2");
        out += ")";
      } else if (dualBody && e.lhs && e.lhs->kind == ExprKind::Ident &&
                 isDualLocal(stringref(e.lhs->name.c_str())))
      {
        out += "sb_comp(";
        out += e.lhs->name;
        out += ", ";
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
    case ExprKind::Binary: {
      bool arith = e.binop == BinOp::Add || e.binop == BinOp::Sub ||
                   e.binop == BinOp::Mul || e.binop == BinOp::Div;
      if (!arith) {
        err("comparison/logical operators are not differentiable inside grad()");
        out += "sb_c(0.0f)";
        break;
      }
      const char *f = e.binop == BinOp::Add   ? "sbd_add"
                      : e.binop == BinOp::Sub ? "sbd_sub"
                      : e.binop == BinOp::Mul ? "sbd_mul"
                                              : "sbd_div";
      out += f;
      out += "(";
      emitDual(*e.lhs);
      out += ", ";
      emitDual(*e.rhs);
      out += ")";
      break;
    }
    case ExprKind::Unary:
      out += "sbd_neg(";
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
      if (findTextureCall(stringref(e.name.c_str()))) {
        if (currentTexture)
          errf("texture '%s' cannot call another texture", currentTexture->name.c_str());
        else
          err("texture calls are only valid inside a texture eval on the C backend");
        out += "sb_c(0.0f)";
        break;
      }
      if (std::strcmp(n, "mapPoint") == 0 && e.args.size() == 1) {
        if (!currentTexture)
          err("mapPoint() is only valid inside a texture eval");
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
      // Host samplers chain through sb_hs_grad — the p-gradient dotted with
      // p's Jacobian rows; any dependence of the sampler on n is not tracked.
      if (currentTexture && findSamplerDecl(n)) {
        const SamplerDecl *sd = findSamplerDecl(n);
        if (e.args.size() != sd->params.size()) {
          errf("sampler '%s' called with the wrong number of arguments", n);
          out += "sb_c(0.0f)";
          break;
        }
        out += "sb_hs_call_d(sb_hs_";
        out += n;
        out += ", ";
        emitDual(*e.args[0]);
        out += ", ";
        if (e.args.size() == 2)
          emitDual(*e.args[1]);
        else
          out += "sb_c3(sb_f3(0.0f, 0.0f, 0.0f))";
        out += ")";
        break;
      }
      // Only intrinsics with an sbd_* chain rule in the prelude may appear
      // in a differentiated expression.
      static const char *kDualIntrinsics[] = {"sin",
                                              "cos",
                                              "sqrt",
                                              "abs",
                                              "dot",
                                              "length",
                                              "mix",
                                              "floor",
                                              "fract",
                                              "pow",
                                              "exp",
                                              "log",
                                              "atan2",
                                              "mod",
                                              "step",
                                              "smoothstep"};
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

  // === statement emitter ===

  void emitStmt(const Stmt &s)
  {
    switch (s.kind) {
    case StmtKind::Block: {
      writeIndent();
      out += "{\n";
      indent++;
      int sl = (int)locals.size();
      for (const auto &c : s.stmts)
        emitStmt(*c);
      while ((int)locals.size() > sl)
        locals.pop_back();
      indent--;
      writeIndent();
      out += "}\n";
      break;
    }
    case StmtKind::DeclLocal:
      if (dualBody && (s.declType == TypeKind::Float || s.declType == TypeKind::Float3)) {
        writeIndent();
        out += (s.declType == TypeKind::Float) ? "sbdual" : "sbdual3";
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
      if (!cTypeName(s.declType) && s.declType != TypeKind::Unknown) {
        errf("local '%s' has a type unsupported on the C texture backend",
             s.name.c_str());
        break;
      }
      writeIndent();
      out += cTypeName(s.declType) ? cTypeName(s.declType) : "float";
      out += " ";
      out += s.name;
      if (s.expr) {
        out += " = ";
        emitExpr(*s.expr);
      }
      out += ";\n";
      locals.append(LocalVar{s.name, s.declType, /*dual=*/false});
      break;
    case StmtKind::Assign: {
      if (dualBody && s.lvalue->kind == ExprKind::Ident &&
          isDualLocal(stringref(s.lvalue->name.c_str())))
      {
        // C has no operator overloads: compound assigns on duals expand
        // through the sbd_* function forms.
        writeIndent();
        out += s.lvalue->name;
        out += " = ";
        char opc = assignOpCSym(s.assignOp)[0];
        if (opc != '=') {
          const char *f = opc == '+'   ? "sbd_add"
                          : opc == '-' ? "sbd_sub"
                          : opc == '*' ? "sbd_mul"
                                       : "sbd_div";
          out += f;
          out += "(";
          out += s.lvalue->name;
          out += ", ";
          emitDual(*s.rvalue);
          out += ")";
        } else
          emitDual(*s.rvalue);
        out += ";\n";
        break;
      }
      if (dualBody && s.lvalue->kind == ExprKind::Member && s.lvalue->lhs &&
          s.lvalue->lhs->kind == ExprKind::Ident &&
          isDualLocal(stringref(s.lvalue->lhs->name.c_str())))
      {
        errf("component assignment to dual '%s' is not differentiable",
             s.lvalue->lhs->name.c_str());
        break;
      }
      if (s.lvalue->kind == ExprKind::Index && s.lvalue->lhs &&
          resolveExprType(*s.lvalue->lhs) == TypeKind::Float3)
      {
        err("indexed assignment into a float3 is not supported on the C texture backend");
        break;
      }
      if (resolveExprType(*s.lvalue) == TypeKind::Float3) {
        // float3 compound assigns expand through the sb3_* helpers, with the
        // rhs type picking the vec-vec vs vec-scalar form.
        writeIndent();
        string lv = render(*s.lvalue);
        char opc = assignOpCSym(s.assignOp)[0];
        out += lv;
        out += " = ";
        if (opc != '=') {
          TypeKind rt = resolveExprType(*s.rvalue);
          const char *f = opc == '+'   ? "sb3_add"
                          : opc == '-' ? "sb3_sub"
                          : opc == '*'
                              ? (rt == TypeKind::Float3 ? "sb3_mul" : "sb3_scale")
                              : (rt == TypeKind::Float3 ? "sb3_div" : "sb3_divs");
          out += f;
          out += "(";
          out += lv;
          out += ", ";
          emitExpr(*s.rvalue);
          out += ")";
        } else
          emitExpr(*s.rvalue);
        out += ";\n";
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
        int sl = (int)locals.size();
        for (const auto &c : s.thenBranch->stmts)
          emitStmt(*c);
        while ((int)locals.size() > sl)
          locals.pop_back();
        indent--;
        writeIndent();
        out += "}";
      } else if (s.thenBranch) {
        out += "{\n";
        indent++;
        emitStmt(*s.thenBranch);
        indent--;
        writeIndent();
        out += "}";
      }
      if (s.elseBranch) {
        out += " else ";
        if (s.elseBranch->kind == StmtKind::Block) {
          out += "{\n";
          indent++;
          int sl = (int)locals.size();
          for (const auto &c : s.elseBranch->stmts)
            emitStmt(*c);
          while ((int)locals.size() > sl)
            locals.pop_back();
          indent--;
          writeIndent();
          out += "}\n";
        } else if (s.elseBranch->kind == StmtKind::If)
          emitStmt(*s.elseBranch);
        else {
          out += "{\n";
          indent++;
          emitStmt(*s.elseBranch);
          indent--;
          writeIndent();
          out += "}\n";
        }
      } else
        out += "\n";
      break;
    }
    case StmtKind::For: {
      writeIndent();
      out += "for (";
      auto renderFrag = [&](const Stmt &child, bool stripSemi) {
        string saved = out;
        out = string("");
        int si = indent;
        indent = 0;
        emitStmt(child);
        indent = si;
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
        renderFrag(*s.forInit, false);
      out += " ";
      emitExpr(*s.cond);
      out += "; ";
      if (s.forStep)
        renderFrag(*s.forStep, true);
      out += ") ";
      if (s.thenBranch && s.thenBranch->kind == StmtKind::Block) {
        out += "{\n";
        indent++;
        int sl = (int)locals.size();
        for (const auto &c : s.thenBranch->stmts)
          emitStmt(*c);
        while ((int)locals.size() > sl)
          locals.pop_back();
        indent--;
        writeIndent();
        out += "}\n";
      } else if (s.thenBranch) {
        out += "{\n";
        indent++;
        emitStmt(*s.thenBranch);
        indent--;
        writeIndent();
        out += "}\n";
      }
      break;
    }
    // Texture bodies are plain functions, not per-vertex kernel bodies, so
    // continue keeps its C meaning (unlike the opencl kernel lowering).
    case StmtKind::Continue:
      writeIndent();
      out += "continue;\n";
      break;
    case StmtKind::Return:
      writeIndent();
      out += "return";
      if (s.expr) {
        out += " ";
        if (dualBody)
          emitDual(*s.expr);
        else
          emitExpr(*s.expr);
      }
      out += ";\n";
      break;
    case StmtKind::ExprStmt:
      writeIndent();
      emitExpr(*s.expr);
      out += ";\n";
      break;
    case StmtKind::NeighborLoop:
      err("for_neighbor is not available in a texture script");
      break;
    }
  }

  // === top-level emitter ===

  void emitPrelude()
  {
    write("// AUTO-GENERATED by sbrushc (C99 texture backend) — DO NOT EDIT.\n");
    write("// Source: ");
    write(brush->sourceFile);
    write("\n");
    write("// Freestanding TU: the only external symbols are these libm functions,\n");
    write("// bound by the host via tcc_add_symbol (or -lm when built standalone).\n\n");

    write("extern float sinf(float);\n");
    write("extern float cosf(float);\n");
    write("extern float sqrtf(float);\n");
    write("extern float floorf(float);\n");
    write("extern float fabsf(float);\n");
    write("extern float powf(float, float);\n");
    write("extern float atan2f(float, float);\n");
    write("extern float expf(float);\n");
    write("extern float logf(float);\n\n");

    write("typedef struct { float x, y, z; } float3;\n");
    write("typedef struct { float map_matrix[16]; } TexEvalCtx;\n\n");

    write("static float3 sb_f3(float x, float y, float z) { float3 r; r.x = x; r.y = y; "
          "r.z = z; return r; }\n");
    write("static float sb3_idx(float3 a, int i) { return i == 0 ? a.x : i == 1 ? a.y : "
          "a.z; }\n");
    write("static float3 sb3_add(float3 a, float3 b) { return sb_f3(a.x + b.x, a.y + "
          "b.y, a.z + b.z); }\n");
    write("static float3 sb3_sub(float3 a, float3 b) { return sb_f3(a.x - b.x, a.y - "
          "b.y, a.z - b.z); }\n");
    write("static float3 sb3_mul(float3 a, float3 b) { return sb_f3(a.x * b.x, a.y * "
          "b.y, a.z * b.z); }\n");
    write("static float3 sb3_div(float3 a, float3 b) { return sb_f3(a.x / b.x, a.y / "
          "b.y, a.z / b.z); }\n");
    write("static float3 sb3_scale(float3 a, float s) { return sb_f3(a.x * s, a.y * s, "
          "a.z * s); }\n");
    write("static float3 sb3_divs(float3 a, float s) { return sb_f3(a.x / s, a.y / s, "
          "a.z / s); }\n");
    write("static float3 sb3_mix(float3 a, float3 b, float t) { return sb3_add(a, "
          "sb3_scale(sb3_sub(b, a), t)); }\n");
    // std::min/max/clamp semantics (single-comparison ternaries), NOT
    // fminf/fmaxf — the NaN behavior differs.
    write("static float sb_minf(float a, float b) { return b < a ? b : a; }\n");
    write("static float sb_maxf(float a, float b) { return a < b ? b : a; }\n");
    write("static float sb_clampf(float x, float lo, float hi) { return x < lo ? lo : hi "
          "< x ? hi : x; }\n");
    // This computes GLSL/WGSL floored modulus rather than C's truncated fmod.
    // For example, mod(-0.25, 1) equals 0.75.
    write("static float sb_modf(float a, float b) { return a - b * floorf(a / b); }\n");
    write("static float sb_stepf(float edge, float x) { return x < edge ? 0.0f : 1.0f; "
          "}\n");
    write("static float sb_smoothstepf(float e0, float e1, float x) {\n");
    write("  float t = sb_clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);\n");
    write("  return t * t * (3.0f - 2.0f * t);\n");
    write("}\n");
    write("static float sc_dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z "
          "* b.z; }\n");
    write("static float sc_length(float3 a) { return sqrtf(sc_dot(a, a)); }\n");
    // The double literal + double round-trip mirror litestl's normalize()
    // exactly — required for bit parity with the cpp backend.
    write("static float3 sc_normalize(float3 a) {\n");
    write("  float len = sc_length(a);\n");
    write("  if (!(len > 0.00000001)) return sb_f3(0.0f, 0.0f, 0.0f);\n");
    write("  double mul = 1.0 / (double)len;\n");
    write("  return sb_f3((float)((double)a.x * mul), (float)((double)a.y * mul), "
          "(float)((double)a.z * mul));\n");
    write("}\n");
    write("static float3 sc_cross(float3 a, float3 b) {\n");
    write("  return sb_f3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y "
          "* b.x);\n");
    write("}\n\n");

    if (anyTextureUsesRamp()) {
      char buf[128];
      std::snprintf(buf,
                    sizeof(buf),
                    "enum { kTexRampSize = %d }; // must match brush/texture_eval.h\n",
                    kTexRampSize);
      write(buf);
      write("static float texRampSample(const float *ramp, float t) {\n");
      write("  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);\n");
      write("  float x = t * (float)(kTexRampSize - 1);\n");
      write("  int i = (int)x;\n");
      write("  if (i >= kTexRampSize - 1) return ramp[kTexRampSize - 1];\n");
      write("  float f = x - (float)i;\n");
      write("  return ramp[i] + (ramp[i + 1] - ramp[i]) * f;\n");
      write("}\n\n");
    }

    if (anyTextureUsesMap()) {
      write("static float3 texMapPoint(const TexEvalCtx *ctx, float3 p) {\n");
      write("  if (!ctx) return p;\n");
      write("  const float *m = ctx->map_matrix;\n");
      write("  float x = m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3];\n");
      write("  float y = m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7];\n");
      write("  float z = m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11];\n");
      write("  float w = m[12] * p.x + m[13] * p.y + m[14] * p.z + m[15];\n");
      write("  float d = fabsf(w) > 1e-6f ? w : 1.0f;\n");
      write("  return sb_f3(x / d, y / d, z / d);\n");
      write("}\n\n");
    }

    // Forward-mode dual prelude — always emitted: TextureProgram carries
    // evalDual unconditionally, so grad availability never needs a re-JIT.
    write("typedef struct { float v; float3 d; } sbdual;\n");
    write("typedef struct { float3 v; float3 dx, dy, dz; } sbdual3;\n");
    write("static sbdual sb_dual(float v, float3 d) { sbdual r; r.v = v; r.d = d; return "
          "r; }\n");
    write("static sbdual3 sb_dual3(float3 v, float3 dx, float3 dy, float3 dz) { sbdual3 "
          "r; r.v = v; r.dx = dx; r.dy = dy; r.dz = dz; return r; }\n");
    write(
        "static sbdual sb_c(float x) { return sb_dual(x, sb_f3(0.0f, 0.0f, 0.0f)); }\n");
    write("static sbdual3 sb_c3(float3 p) { float3 z = sb_f3(0.0f, 0.0f, 0.0f); return "
          "sb_dual3(p, z, z, z); }\n");
    write("static sbdual3 sb_seed3(float3 p) { return sb_dual3(p, sb_f3(1.0f, 0.0f, "
          "0.0f), sb_f3(0.0f, 1.0f, 0.0f), sb_f3(0.0f, 0.0f, 1.0f)); }\n");
    write("static sbdual sb_comp(sbdual3 a, int i) { return sb_dual(sb3_idx(a.v, i), "
          "sb_f3(sb3_idx(a.dx, i), sb3_idx(a.dy, i), sb3_idx(a.dz, i))); }\n");
    write("static sbdual3 sb_v3(sbdual x, sbdual y, sbdual z) { return "
          "sb_dual3(sb_f3(x.v, y.v, z.v), sb_f3(x.d.x, y.d.x, z.d.x), sb_f3(x.d.y, "
          "y.d.y, z.d.y), sb_f3(x.d.z, y.d.z, z.d.z)); }\n");
    write("static sbdual sbd_add(sbdual a, sbdual b) { return sb_dual(a.v + b.v, "
          "sb3_add(a.d, b.d)); }\n");
    write("static sbdual sbd_sub(sbdual a, sbdual b) { return sb_dual(a.v - b.v, "
          "sb3_sub(a.d, b.d)); }\n");
    write("static sbdual sbd_neg(sbdual a) { return sb_dual(-a.v, sb3_scale(a.d, "
          "-1.0f)); }\n");
    write("static sbdual sbd_mul(sbdual a, sbdual b) { return sb_dual(a.v * b.v, "
          "sb3_add(sb3_scale(a.d, b.v), sb3_scale(b.d, a.v))); }\n");
    write("static sbdual sbd_div(sbdual a, sbdual b) { return sb_dual(a.v / b.v, "
          "sb3_divs(sb3_sub(sb3_scale(a.d, b.v), sb3_scale(b.d, a.v)), b.v * b.v)); }\n");
    write("static sbdual sbd_sin(sbdual a) { return sb_dual(sinf(a.v), sb3_scale(a.d, "
          "cosf(a.v))); }\n");
    write("static sbdual sbd_cos(sbdual a) { return sb_dual(cosf(a.v), sb3_scale(a.d, "
          "-sinf(a.v))); }\n");
    write("static sbdual sbd_sqrt(sbdual a) { float r = sqrtf(a.v); return sb_dual(r, "
          "sb3_scale(a.d, r > 0.0f ? 0.5f / r : 0.0f)); }\n");
    write("static sbdual sbd_abs(sbdual a) { return sb_dual(fabsf(a.v), sb3_scale(a.d, "
          "a.v < 0.0f ? -1.0f : 1.0f)); }\n");
    write("static sbdual sbd_dot(sbdual3 a, sbdual3 b) {\n");
    write("  float3 d = sb3_add(sb3_add(sb3_add(sb3_add(sb3_add(sb3_scale(a.dx, b.v.x), "
          "sb3_scale(b.dx, a.v.x)),\n");
    write("      sb3_scale(a.dy, b.v.y)), sb3_scale(b.dy, a.v.y)), sb3_scale(a.dz, "
          "b.v.z)), sb3_scale(b.dz, a.v.z));\n");
    write("  return sb_dual(sc_dot(a.v, b.v), d);\n");
    write("}\n");
    write("static sbdual sbd_length(sbdual3 a) { return sbd_sqrt(sbd_dot(a, a)); }\n");
    write("static sbdual sbd_mix(sbdual a, sbdual b, sbdual t) { return sbd_add(a, "
          "sbd_mul(sbd_sub(b, a), t)); }\n");
    write("static sbdual sbd_floor(sbdual a) { return sb_dual(floorf(a.v), sb_f3(0.0f, "
          "0.0f, 0.0f)); }\n");
    write("static sbdual sbd_fract(sbdual a) { return sb_dual(a.v - floorf(a.v), a.d); "
          "}\n");
    write("static sbdual sbd_exp(sbdual a) { float r = expf(a.v); return sb_dual(r, "
          "sb3_scale(a.d, r)); }\n");
    write("static sbdual sbd_log(sbdual a) { return sb_dual(logf(a.v), sb3_divs(a.d, "
          "a.v)); }\n");
    // d(a^b) = a^b * (b*a'/a + ln(a)*b'); the log term is dropped for a <= 0,
    // where it is undefined and b is in practice a constant exponent anyway.
    write("static sbdual sbd_pow(sbdual a, sbdual b) {\n");
    write("  float r = powf(a.v, b.v);\n");
    write("  float3 d = sb3_scale(a.d, b.v * powf(a.v, b.v - 1.0f));\n");
    write("  if (a.v > 0.0f) d = sb3_add(d, sb3_scale(b.d, r * logf(a.v)));\n");
    write("  return sb_dual(r, d);\n");
    write("}\n");
    write("static sbdual sbd_atan2(sbdual y, sbdual x) {\n");
    write("  float den = x.v * x.v + y.v * y.v;\n");
    write("  float3 d = sb3_sub(sb3_scale(y.d, x.v), sb3_scale(x.d, y.v));\n");
    write("  return sb_dual(atan2f(y.v, x.v), den > 0.0f ? sb3_divs(d, den) : "
          "sb_f3(0.0f, 0.0f, 0.0f));\n");
    write("}\n");
    // floor() contributes nothing away from its steps, so mod differentiates
    // to a' - floor(a/b)*b'; step and smoothstep follow the same reasoning.
    write("static sbdual sbd_mod(sbdual a, sbdual b) {\n");
    write("  float q = floorf(a.v / b.v);\n");
    write("  return sb_dual(a.v - b.v * q, sb3_sub(a.d, sb3_scale(b.d, q)));\n");
    write("}\n");
    write("static sbdual sbd_step(sbdual edge, sbdual x) {\n");
    write("  return sb_dual(x.v < edge.v ? 0.0f : 1.0f, sb_f3(0.0f, 0.0f, 0.0f));\n");
    write("}\n");
    write("static sbdual sbd_smoothstep(sbdual e0, sbdual e1, sbdual x) {\n");
    write("  float w = e1.v - e0.v;\n");
    write("  float u = (x.v - e0.v) / w;\n");
    write("  float t = sb_clampf(u, 0.0f, 1.0f);\n");
    write("  float dt = (u > 0.0f && u < 1.0f) ? 6.0f * t * (1.0f - t) / w : 0.0f;\n");
    write("  float3 du = sb3_sub(x.d, sb3_add(sb3_scale(e0.d, 1.0f - t), sb3_scale(e1.d, "
          "t)));\n");
    write("  return sb_dual(t * t * (3.0f - 2.0f * t), sb3_scale(du, dt));\n");
    write("}\n");

    if (anyTextureUsesMap()) {
      write("static float3 sbd_mapCol(const float *m, float3 c, float3 q, float d, float "
            "g) {\n");
      write("  float qx = m[0] * c.x + m[1] * c.y + m[2] * c.z;\n");
      write("  float qy = m[4] * c.x + m[5] * c.y + m[6] * c.z;\n");
      write("  float qz = m[8] * c.x + m[9] * c.y + m[10] * c.z;\n");
      write("  float qw = (m[12] * c.x + m[13] * c.y + m[14] * c.z) * g;\n");
      write("  return sb_f3((qx * d - q.x * qw) / (d * d), (qy * d - q.y * qw) / (d * "
            "d), (qz * d - q.z * qw) / (d * d));\n");
      write("}\n");
      write("static sbdual3 sbd_mapPoint(const TexEvalCtx *ctx, sbdual3 p) {\n");
      write("  if (!ctx) return p;\n");
      write("  const float *m = ctx->map_matrix;\n");
      write("  float x = m[0] * p.v.x + m[1] * p.v.y + m[2] * p.v.z + m[3];\n");
      write("  float y = m[4] * p.v.x + m[5] * p.v.y + m[6] * p.v.z + m[7];\n");
      write("  float z = m[8] * p.v.x + m[9] * p.v.y + m[10] * p.v.z + m[11];\n");
      write("  float w = m[12] * p.v.x + m[13] * p.v.y + m[14] * p.v.z + m[15];\n");
      write("  float g = fabsf(w) > 1e-6f ? 1.0f : 0.0f;\n");
      write("  float d = g > 0.0f ? w : 1.0f;\n");
      write("  float3 q = sb_f3(x, y, z);\n");
      write("  sbdual3 r;\n");
      write("  r.v = sb_f3(x / d, y / d, z / d);\n");
      write("  r.dx = sbd_mapCol(m, p.dx, q, d, g);\n");
      write("  r.dy = sbd_mapCol(m, p.dy, q, d, g);\n");
      write("  r.dz = sbd_mapCol(m, p.dz, q, d, g);\n");
      write("  return r;\n");
      write("}\n");
    }

    // Host samplers (T4): the TU reaches the registry only through the two
    // scalar-arg bridges. Each sb_hs_<name> handle is a TU-defined slot the
    // host fills with the registry entry's address after relocation — an
    // extern data symbol would need dllimport under tcc's PE backend, and a
    // function symbol's address resolves to a local jump thunk, not the
    // bound target.
    if (anyTextureUsesSampler()) {
      write("extern float sb_hs_value(const void *s, float px, float py, float pz, "
            "float nx, float ny, float nz);\n");
      write("extern void sb_hs_grad(const void *s, float px, float py, float pz, "
            "float nx, float ny, float nz, float *out4);\n");
      Vector<string> handles;
      for (const auto &t : brush->textures) {
        for (const auto &dep : t.samplerDeps) {
          bool seen = false;
          for (const auto &h : handles) {
            if (string(h).operator==(string(dep.c_str())))
              seen = true;
          }
          if (seen)
            continue;
          handles.append(dep);
          write("void *sb_hs_");
          write(dep);
          write(" = 0;\n");
        }
      }
      write("static float sb_hs_call(const void *s, float3 p, float3 n) "
            "{ return sb_hs_value(s, p.x, p.y, p.z, n.x, n.y, n.z); }\n");
      write("static sbdual sb_hs_call_d(const void *s, sbdual3 p, sbdual3 n) {\n");
      write("  float o[4];\n");
      write("  sb_hs_grad(s, p.v.x, p.v.y, p.v.z, n.v.x, n.v.y, n.v.z, o);\n");
      write("  float3 g = sb_f3(o[1], o[2], o[3]);\n");
      write("  return sb_dual(o[0], sb_f3(sc_dot(g, p.dx), sc_dot(g, p.dy), sc_dot(g, "
            "p.dz)));\n");
      write("}\n");
    }
    write("\n");
  }

  // Runtime param slab — non-static so tcc_get_symbol can resolve it.
  void emitTextureDefaults(const TextureDef &td)
  {
    if (td.slabSize <= 0)
      return;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "_param_defaults[%d] = {\n", td.slabSize);
    write("const float tex_");
    write(lower(td.name));
    write(buf);
    for (const auto &tp : td.texParams) {
      if (tp.isConst)
        continue;
      out += "    // ";
      out += tp.name;
      out += "\n    ";
      if (tp.kind == TexParamKind::Ramp) {
        for (int i = 0; i < kTexRampSize; i++) {
          appendFloatLit(out, (double)((float)i / (float)(kTexRampSize - 1)));
          out += ",";
          out += (i % 8 == 7 && i != kTexRampSize - 1) ? "\n    " : " ";
        }
        out += "\n";
      } else {
        appendFloatLit(out, tp.defaultValue);
        out += ",\n";
      }
    }
    write("};\n\n");
  }

  bool checkEvalSignature(const TextureDef &td)
  {
    if (td.returnType != TypeKind::Float) {
      errf("texture '%s' must return float for the C backend (runtime eval ABI)",
           td.name.c_str());
      return false;
    }
    for (const auto &p : td.params) {
      if (p.type != TypeKind::Float && p.type != TypeKind::Float3) {
        errf("texture eval param '%s' must be float or float3 on the C backend",
             p.name.c_str());
        return false;
      }
    }
    return true;
  }

  void emitBody(const TextureDef &td)
  {
    indent = 1;
    Stage scratch;
    scratch.kind = StageKind::Reduce;
    for (const auto &p : td.params)
      scratch.params.append(p);
    currentStage = &scratch;
    currentTexture = &td;
    if (td.body && td.body->kind == StmtKind::Block) {
      int sl = (int)locals.size();
      for (const auto &c : td.body->stmts)
        emitStmt(*c);
      while ((int)locals.size() > sl)
        locals.pop_back();
    }
    currentTexture = nullptr;
    currentStage = nullptr;
    indent = 0;
  }

  // By-value struct passing is safe within the single tcc TU; the exported
  // wrapper flattens float3 params to const float* for a stable ctypes-style
  // ABI at the JIT boundary.
  void emitTextureFn(const TextureDef &td)
  {
    if (!checkEvalSignature(td))
      return;
    string name = texEvalName(td);

    write("static float ");
    write(name);
    write("_impl(");
    for (const auto &p : td.params) {
      write(p.type == TypeKind::Float3 ? "float3 " : "float ");
      write(p.name);
      write(", ");
    }
    write("const float *sb_tex_params, const TexEvalCtx *sb_texctx)\n{\n");
    for (const auto &p : td.params) {
      write("  (void)");
      write(p.name);
      write(";\n");
    }
    write("  (void)sb_tex_params;\n  (void)sb_texctx;\n");
    emitBody(td);
    write("}\n\n");

    write("float ");
    write(name);
    write("(");
    for (const auto &p : td.params) {
      write(p.type == TypeKind::Float3 ? "const float *" : "float ");
      write(p.name);
      write(", ");
    }
    write("const float *sb_tex_params, const TexEvalCtx *sb_texctx)\n{\n");
    write("  return ");
    write(name);
    write("_impl(");
    for (const auto &p : td.params) {
      if (p.type == TypeKind::Float3) {
        write("sb_f3(");
        write(p.name);
        write("[0], ");
        write(p.name);
        write("[1], ");
        write(p.name);
        write("[2])");
      } else {
        write(p.name);
      }
      write(", ");
    }
    write("sb_tex_params, sb_texctx);\n}\n\n");
  }

  void emitTextureFnDual(const TextureDef &td)
  {
    if (td.returnType != TypeKind::Float)
      return;
    int outMark = (int)out.size();
    int errMark = (int)errors.size();
    string name = texEvalName(td);

    write("static sbdual ");
    write(name);
    write("_d_impl(");
    for (const auto &p : td.params) {
      write(p.type == TypeKind::Float3 ? "sbdual3 " : "sbdual ");
      write(p.name);
      write(", ");
    }
    write("const float *sb_tex_params, const TexEvalCtx *sb_texctx)\n{\n");
    for (const auto &p : td.params) {
      write("  (void)");
      write(p.name);
      write(";\n");
    }
    write("  (void)sb_tex_params;\n  (void)sb_texctx;\n");
    int sl = (int)locals.size();
    for (const auto &p : td.params)
      locals.append(LocalVar{p.name, p.type, /*dual=*/true});
    dualBody = true;
    emitBody(td);
    dualBody = false;
    while ((int)locals.size() > sl)
      locals.pop_back();
    write("}\n\n");

    write("void ");
    write(name);
    write("_d(");
    for (const auto &p : td.params) {
      write(p.type == TypeKind::Float3 ? "const sbdual3 *" : "const sbdual *");
      write(p.name);
      write(", ");
    }
    write(
        "const float *sb_tex_params, const TexEvalCtx *sb_texctx, sbdual *sb_out)\n{\n");
    write("  *sb_out = ");
    write(name);
    write("_d_impl(");
    for (const auto &p : td.params) {
      write("*");
      write(p.name);
      write(", ");
    }
    write("sb_tex_params, sb_texctx);\n}\n\n");

    // A body with no derivative rule (ramp.sample) is still a valid value
    // texture: drop the twin + its errors so the JIT resolves eval alone and
    // the host leaves evalDual null. Sampler calls chain through sb_hs_grad.
    if ((int)errors.size() > errMark) {
      out = out.substr(0, outMark);
      while ((int)errors.size() > errMark)
        errors.pop_back();
      write("// ");
      write(name);
      write("_d omitted: eval is not differentiable\n\n");
    }
  }
};

} // namespace

EmitResult emitCTextureDefs(const Brush &brush)
{
  Emit em;
  em.brush = &brush;
  em.emitPrelude();
  for (const auto &td : brush.textures) {
    em.emitTextureDefaults(td);
    em.emitTextureFn(td);
    em.emitTextureFnDual(td);
  }
  EmitResult r;
  r.text = em.out;
  r.errors = em.errors;
  return r;
}

} // namespace sculptcore::brush::sbrush
