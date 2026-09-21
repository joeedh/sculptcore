/** Texture definitions: defaults, the eval and dual-eval functions, the
 * per-texture block and its param manifest. */

#include "emit_cpp_internal.h"

namespace sculptcore::brush::sbrush::cpp_emit {

bool Emit::anyTextureUsesMap() const
{
  for (const auto &t : brush->textures) {
    if (t.usesMap)
      return true;
  }
  return false;
}

string Emit::texDefaultsName(const TextureDef &td)
{
  return string("tex") + capitalize(td.name) + "ParamDefaults";
}

// Format `v` as a C++ float literal (round-trip exact for float values).
void Emit::appendFloatLit(string &s, double v)
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

// Stage-scope snapshot of ctx.renderMatrix for mapPoint textures — the
// texture-scripts plan's TexEvalCtx threading. Call sites pass `sb_texctx`
// for usesMap textures; every stage prologue emits it when any texture in
// the brush needs it, since a texture call can appear in any stage body.
void Emit::emitTexCtxLocal()
{
  if (!anyTextureUsesMap())
    return;
  write("  TexEvalCtx sb_texctx_data = texEvalCtxFrom(ctx.renderMatrix);\n");
  write("  const TexEvalCtx *sb_texctx = &sb_texctx_data; (void)sb_texctx;\n");
}

// Emit one texture's non-@const param defaults as a static slab the brush
// call sites pass when no runtime binding exists (T1: always). Ramps seed
// to the identity ramp.
void Emit::emitTextureDefaults(const TextureDef &td)
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
  std::snprintf(buf,
                sizeof(buf),
                "static_assert(kTexRampSize == %d, \"ramp slab size drifted\");\n\n",
                kTexRampSize);
  write(buf);
}

// Emit one texture's eval as a pure free function. It sees only its own
// parameters, its param slab, the threaded map ctx, and intrinsics — no
// ctx/brush state — so the same text lowers identically on every backend
// and stays runtime-compilable (T3).
void Emit::emitTextureFn(const TextureDef &td)
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

/**
 * Dual-number twin of emitTextureFn: the same eval body re-emitted with
 * float/float3 locals lowered to sbdual/sbdual3 and expressions routed
 * through emitDual, so grad() can differentiate through a texture call.
 * Value-context expressions (conditions, int locals, indices) still go
 * through emitExpr, which projects dual names back to `.v`.
 */
void Emit::emitTextureFnDual(const TextureDef &td)
{
  write("static inline ");
  write(td.returnType == TypeKind::Float ? "sbdual" : "sbdual3");
  write(" tex");
  write(capitalize(td.name));
  write("EvalD(");
  for (int i = 0; i < (int)td.params.size(); i++) {
    if (i > 0)
      write(", ");
    write(td.params[i].type == TypeKind::Float ? "sbdual" : "sbdual3");
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
  Stage scratch;
  scratch.kind = StageKind::Reduce;
  for (const auto &p : td.params)
    scratch.params.append(p);
  currentStage = &scratch;
  currentTexture = &td;
  dualBody = true;
  int savedLocals = (int)locals.size();
  // Params keep their DSL TypeKind but carry duals — the dual flag is what
  // routes their reads through emitDual / `.v` projection.
  for (const auto &p : td.params)
    locals.append(LocalVar{p.name, p.type, /*dual=*/true});
  if (td.body && td.body->kind == StmtKind::Block) {
    for (const auto &c : td.body->stmts)
      emitStmt(*c);
  }
  while ((int)locals.size() > savedLocals)
    locals.pop_back();
  dualBody = false;
  currentTexture = nullptr;
  currentStage = nullptr;
  indent = 0;
  write("}\n\n");
}

// Defaults slab + eval fn for one texture. Imports are include-guarded:
// several brushes in one TU may pull the same texture, and every emission
// of it is byte-identical (same .stex parse), so first-wins is safe.
void Emit::emitTextureBlock(const TextureDef &td)
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
  // EvalD guards separately from Eval: a non-grad brush emitting this
  // texture first must not swallow a later grad brush's EvalD.
  if (brushUsesGrad()) {
    if (td.imported) {
      write("#ifndef SB_TEX_DEFD_");
      write(td.name);
      write("\n#define SB_TEX_DEFD_");
      write(td.name);
      write("\n");
    }
    emitTextureFnDual(td);
    if (td.imported) {
      write("#endif  // SB_TEX_DEFD_");
      write(td.name);
      write("\n\n");
    }
  }
}

// Runtime-param manifest (non-@const params, decl order) — emitted only
// into .tex.gen.h units, where the registry rows point at it. Kept outside
// the SB_TEX_DEF_ guard: the symbol exists only in the unit header.
void Emit::emitTextureManifest(const TextureDef &td)
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
    std::snprintf(buf,
                  sizeof(buf),
                  "    {\"%s\", %s, %s, %s, %s, %s, %d},\n",
                  tp.name.c_str(),
                  tp.kind == TexParamKind::Ramp ? "true" : "false",
                  defLit.c_str(),
                  tp.hasRange ? "true" : "false",
                  minLit.c_str(),
                  maxLit.c_str(),
                  tp.offset);
    write(buf);
  }
  write("};\n\n");
}

} // namespace sculptcore::brush::sbrush::cpp_emit
