#pragma once

/** The C++ emitter's working state, shared by the emit_cpp_*.cc translation
 * units (one per concern: expressions, statements, textures, stages, the
 * kernel body). Internal to the compiler; the public surface is emit_cpp.h. */

#include "emit_cpp.h"
#include "../kernels/ir/intrinsics.h"
#include "prepared_preludes.h"
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

namespace cpp_emit {


inline const char *storeSuffix(TypeKind type)
{
  return type == TypeKind::Bool ? "Bool" : type == TypeKind::Int ? "Int" : "Float";
}

inline const char *scalarCppType(TypeKind type)
{
  return type == TypeKind::Bool ? "bool" : type == TypeKind::Int ? "int" : "float";
}

inline string integralLiteral(double value)
{
  char text[32];
  std::snprintf(text, sizeof(text), "%.0f", value);
  return string(text);
}

struct Emit {
  const Brush *brush;
  // Extra (out-of-repo) kernel: store-classified uniforms lower to
  // typed named slots instead of erroring (see CppEmitOptions).
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
  Vector<string> warnings;
  int indent = 0;

  // Locals declared in the current body. We track the declared type so a
  // vector-component access (`v.x`) can be lowered to `operator[]` on the C++
  // backend — litestl Vec has no named .x/.y/.z/.w members, unlike WGSL/CUDA/
  // OpenCL vectors. Knowing a name is a local also routes identifier resolution.
  struct LocalVar {
    string name;
    TypeKind type = TypeKind::Unknown;
    // Lowered to sbdual/sbdual3 inside a texture EvalD body (dualBody).
    bool dual = false;
  };
  Vector<LocalVar> locals;

  // Set when a NeighborLoop is emitted — drives an extra #include in
  // the generated header so EdgeOfVertIter resolves.
  bool neighborLoopUsed = false;
  bool preparedStateUnsafe = false;
  bool certifiedPrelude = false;

  void validateGeneratedName(const string &name);

  void validateGeneratedLocals(const Stmt *stmt);

  void validateGeneratedTextureNames(const TextureDef &texture);
  // Set when grad(expr, var) is used — emits the forward-mode dual prelude.
  bool gradUsed = false;
  // When rewriting a grad body, the float3 variable being differentiated.
  string gradVar;
  // Set while emitting a texture EvalD body (texture-scripts T2): float/
  // float3 locals lower to sbdual/sbdual3, and emitExpr projects dual names
  // through `.v` so conditions/int contexts read primal values.
  bool dualBody = false;

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

  void warnf(const char *fmt, const char *arg)
  {
    char buf[256];
    std::snprintf(buf, sizeof(buf), fmt, arg);
    warnings.append(string(buf));
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

  bool isLocal(stringref name) const;

  static bool scalarOrVectorValue(TypeKind type);

  bool preparedAssignmentSafe(const Expr &expression) const;

  bool isDualLocal(stringref name) const;

  const Field *findField(stringref name) const;

  const Field *findAttrField(stringref name) const;

  bool bundleInfo(stringref name, const char *&idxField, const char *&prefix) const;

  const TextureDef *findTextureCall(stringref callName) const;

  const TexParam *findTexParam(stringref name) const;

  bool anyTextureUsesMap() const;

  static string texDefaultsName(const TextureDef &td);

  static void appendFloatLit(string &s, double v);

  bool isStageParam(stringref name) const;

  static bool isVectorType(TypeKind t);

  static int swizzleIndex(const string &name);

  TypeKind resolveExprType(const Expr &e) const;

  // === expression emitter ===

  void emitExpr(const Expr &e);

  string render(const Expr &e);
  bool isGradVar(const Expr &e);

  void emitDual(const Expr &e);

  // === statement emitter ===

  void emitTypeRef(TypeKind ty, const string &structName);

  void emitStmt(const Stmt &s);

  // === top-level file emitter ===

  static string capitalize(const string &s);

  static string lower(const string &s);

  static string doubleLit(double v);

  static string floatLit(double v);

  static const char *attrCppType(TypeKind t);

  static const char *attrTypeEnum(TypeKind t);

  static const char *attrDomainEnum(AttrDomain d);

  static const char *attrUseEnum(int use);

  void emitHostStage(const Stage &st, const string &lowerBrush);

  void emitReduceStage(const Stage &st, const string &lowerBrush);

  void emitTexCtxLocal();

  void emitTextureDefaults(const TextureDef &td);

  void emitTextureFn(const TextureDef &td);

  void emitTextureFnDual(const TextureDef &td);

  void emitTextureBlock(const TextureDef &td);

  void emitTextureManifest(const TextureDef &td);

  static bool exprUsesGrad(const Expr *e);
  static bool stmtUsesGrad(const Stmt *s);
  bool brushUsesGrad() const;

  bool brushUsesNeighbor() const;

  void emitFaceKernel(const string &lowerName);

  bool kernelWritesGeomOnly() const;

  string saveFlagExpr(const char *nm, int &customIdx);

  void emitSaveDesc(const SaveAttr &sv, AttrDomain dom, int &customIdx);

  void emitPreStage(const string &lowerName);

  void emitGpuPack(const string &camelName);

  void run();
};

} // namespace cpp_emit
} // namespace sculptcore::brush::sbrush
