#include "test_util.h"

#include "brush/compiler/emit_c.h"
#include "brush/compiler/emit_cpp.h"
#include "brush/compiler/emit_wgsl.h"
#include "brush/compiler/lexer.h"
#include "brush/compiler/parser.h"

#include <cstring>
#include <utility>

test_init;

using namespace sculptcore::brush::sbrush;

/** Inline-texture coverage for the sbrush compiler.
 *
 * kernels/texdraw.sbrush used to define its Rings texture inline; it now
 * imports it from kernels/rings.stex via `use texture Rings;`. This test
 * keeps the inline form alive: it parses the pre-port source below, parses
 * the same texture as a standalone .stex unit, resolves the import the way
 * sbrushc does (move the TextureDef into the brush), and asserts the two
 * forms emit byte-identical C++ and WGSL. The compiled kernel's numeric
 * behavior is covered separately by the texdraw sbrush-verify golden.
 *
 * It also covers autodiff through textures (texture-scripts T2): a grad()
 * over an imported eval emits the dual-number twin under its own
 * SB_TEX_DEFD_* guard, and an intrinsic without a chain rule inside a
 * grad()'d eval is a reported error. Numerics gate: the texgrad golden. */

// The former inline form of kernels/texdraw.sbrush, verbatim.
static const char *kInlineSrc = R"(
@brush("texdraw")
brush TexDraw {
  ctx float3 surfaceNo;

  texture Rings {
    float eval(float3 p, float3 n) {
      float d = length(p);
      float rings = 0.5 + 0.5 * sin(d * 40.0);
      float bands = fract(d * 6.0);
      float steps = floor(bands * 4.0) * 0.25;
      float tilt = 0.5 + 0.5 * cos(dot(n, p) * 8.0);
      return rings * steps * tilt;
    }
  }

  vertex void apply(inout Vertex v) {
    float s = strength(v.co) * masks();
    s *= Rings.eval(v.co, surfaceNo);
    if (s == 0.0) {
      continue;
    }
    v.co += surfaceNo * s;
  }
}
)";

// The same brush importing Rings from a unit instead.
static const char *kImportSrc = R"(
@brush("texdraw")
brush TexDraw {
  ctx float3 surfaceNo;

  use texture Rings;

  vertex void apply(inout Vertex v) {
    float s = strength(v.co) * masks();
    s *= Rings.eval(v.co, surfaceNo);
    if (s == 0.0) {
      continue;
    }
    v.co += surfaceNo * s;
  }
}
)";

// The texture as a standalone unit (kernels/rings.stex's body).
static const char *kUnitSrc = R"(
texture Rings {
  float eval(float3 p, float3 n) {
    float d = length(p);
    float rings = 0.5 + 0.5 * sin(d * 40.0);
    float bands = fract(d * 6.0);
    float steps = floor(bands * 4.0) * 0.25;
    float tilt = 0.5 + 0.5 * cos(dot(n, p) * 8.0);
    return rings * steps * tilt;
  }
}
)";

// A grad() brush over the imported texture (kernels/texgrad.sbrush's shape):
// the call inside grad() must dispatch to the texture's dual-number twin.
static const char *kGradSrc = R"(
@brush("texgrad")
brush TexGrad {
  ctx float3 surfaceNo;

  use texture Rings;

  vertex void apply(inout Vertex v) {
    float s = strength(v.co) * masks();
    if (s == 0.0) {
      continue;
    }
    float3 g = grad(Rings.eval(v.co, surfaceNo), v.co);
    v.co += g * (s * 0.005);
  }
}
)";

// grad() through a texture whose eval uses an intrinsic with no dual
// chain rule (min): must be a reported emit error, not a wrong derivative.
static const char *kBadGradSrc = R"(
@brush("badgrad")
brush BadGrad {
  ctx float3 surfaceNo;

  texture Chop {
    float eval(float3 p, float3 n) {
      return min(length(p), dot(n, p));
    }
  }

  vertex void apply(inout Vertex v) {
    float3 g = grad(Chop.eval(v.co, surfaceNo), v.co);
    v.co += g;
  }
}
)";

// C99-backend coverage (texture-scripts T3.2): params/ramp/mapPoint lowering.
// Decl order gives scale slab offset 0 and shape offset 1 (@const holds none).
static const char *kParamUnitSrc = R"(
texture Bands {
  param float scale = 2.0 @range(0.5, 8.0);
  param int octaves = 3 @const;
  param ramp shape;

  float eval(float3 p, float3 n) {
    float3 q = mapPoint(p);
    float d = length(q) * scale + octaves;
    return shape.sample(fract(d));
  }
}
)";

// A sampler call without a carried decl must be the runtime-only error.
static const char *kSamplerUnitSrc = R"(
sampler float noisefn(float3 p);

texture Noisy {
  float eval(float3 p, float3 n) {
    return noisefn(p);
  }
}
)";

// A brush-context intrinsic has no meaning in a standalone texture script.
static const char *kBrushCtxUnitSrc = R"(
texture Bad {
  float eval(float3 p, float3 n) {
    return strength(p);
  }
}
)";

static ParseResult parseSrc(const char *src, const char *fname)
{
  LexResult lr = lex(src, fname);
  test_assert(lr.errors.size() == 0);
  return parse(lr.tokens, fname);
}

static void runTests()
{
  // Same filename for both brush parses: the emitters put brush->sourceFile
  // in the generated banner, which would otherwise be the only difference.
  ParseResult inl = parseSrc(kInlineSrc, "texdraw_test.sbrush");
  test_assert(inl.errors.size() == 0);
  test_assert(inl.brush != nullptr && inl.unit == nullptr);

  ParseResult unit = parseSrc(kUnitSrc, "rings.stex");
  test_assert(unit.errors.size() == 0);
  test_assert(unit.unit != nullptr && unit.brush == nullptr);

  ParseResult imp = parseSrc(kImportSrc, "texdraw_test.sbrush");
  test_assert(imp.errors.size() == 0);
  test_assert(imp.brush != nullptr);

  if (retval) {
    return;
  }

  test_assert(inl.brush->textures.size() == 1);
  test_assert(unit.unit->textures.size() == 1);
  test_assert(imp.brush->textures.size() == 0);
  test_assert(imp.brush->useTextures.size() == 1);
  test_assert(std::strcmp(imp.brush->useTextures[0].c_str(), "Rings") == 0);

  if (retval) {
    return;
  }

  // Resolve the import as sbrushc's resolveUseTextures does, but leave
  // `imported` false so the emissions must match the inline form exactly.
  imp.brush->textures.append(std::move(unit.unit->textures[0]));

  EmitResult inlCpp = emitCpp(*inl.brush);
  EmitResult impCpp = emitCpp(*imp.brush);
  test_assert(inlCpp.errors.size() == 0);
  test_assert(impCpp.errors.size() == 0);
  test_assert(std::strcmp(inlCpp.text.c_str(), impCpp.text.c_str()) == 0);
  test_assert(std::strstr(inlCpp.text.c_str(), "texRingsEval") != nullptr);
  test_assert(std::strstr(inlCpp.text.c_str(), "SB_TEX_DEF_Rings") == nullptr);

  EmitResult inlWgsl = emitWgsl(*inl.brush);
  EmitResult impWgsl = emitWgsl(*imp.brush);
  test_assert(inlWgsl.errors.size() == 0);
  test_assert(impWgsl.errors.size() == 0);
  test_assert(std::strcmp(inlWgsl.text.c_str(), impWgsl.text.c_str()) == 0);

  // The real import path additionally include-guards the definition so
  // brushes/all.h can aggregate several importers into one TU.
  imp.brush->textures[0].imported = true;
  EmitResult guarded = emitCpp(*imp.brush);
  test_assert(guarded.errors.size() == 0);
  test_assert(std::strstr(guarded.text.c_str(), "#ifndef SB_TEX_DEF_Rings") != nullptr);
  test_assert(std::strstr(guarded.text.c_str(), "#endif  // SB_TEX_DEF_Rings") !=
              nullptr);

  // grad(Rings.eval(...)) dispatches to the EvalD twin, emitted under its
  // own guard (SB_TEX_DEFD_*) so a non-grad importer of the same texture
  // in the aggregate TU doesn't swallow it.
  ParseResult unitD = parseSrc(kUnitSrc, "rings.stex");
  ParseResult grd = parseSrc(kGradSrc, "texgrad_test.sbrush");
  test_assert(unitD.errors.size() == 0 && unitD.unit != nullptr);
  test_assert(grd.errors.size() == 0 && grd.brush != nullptr);

  if (retval) {
    return;
  }

  grd.brush->textures.append(std::move(unitD.unit->textures[0]));
  grd.brush->textures[0].imported = true;

  EmitResult grdCpp = emitCpp(*grd.brush);
  test_assert(grdCpp.errors.size() == 0);
  test_assert(std::strstr(grdCpp.text.c_str(), "texRingsEvalD") != nullptr);
  test_assert(std::strstr(grdCpp.text.c_str(), "#ifndef SB_TEX_DEFD_Rings") != nullptr);
  test_assert(std::strstr(grdCpp.text.c_str(), "#endif  // SB_TEX_DEFD_Rings") !=
              nullptr);

  EmitResult grdWgsl = emitWgsl(*grd.brush);
  test_assert(grdWgsl.errors.size() == 0);
  test_assert(std::strstr(grdWgsl.text.c_str(), "tex_rings_eval_d") != nullptr);

  // An intrinsic without a chain rule inside a grad()'d eval must error.
  ParseResult bad = parseSrc(kBadGradSrc, "badgrad_test.sbrush");
  test_assert(bad.errors.size() == 0 && bad.brush != nullptr);

  if (retval) {
    return;
  }

  EmitResult badCpp = emitCpp(*bad.brush);
  test_assert(badCpp.errors.size() != 0);
  bool sawWhitelistErr = false;
  for (const auto &e : badCpp.errors) {
    if (std::strstr(e.c_str(), "no derivative rule") != nullptr) {
      sawWhitelistErr = true;
    }
  }
  test_assert(sawWhitelistErr);

  // C99 backend (T3.2). Wrap each unit in a scratch brush the way sbrushc's
  // --backend=c path does.
  auto unitToBrush = [](ParseResult &pr, Brush &s) {
    s.sourceFile = pr.unit->sourceFile;
    for (auto &td : pr.unit->textures) {
      s.textures.append(std::move(td));
    }
  };

  // Rings: pointer-ABI entries, the dual twin, no param slab, no ctx helpers.
  ParseResult cRings = parseSrc(kUnitSrc, "rings.stex");
  test_assert(cRings.errors.size() == 0 && cRings.unit != nullptr);
  if (retval) {
    return;
  }

  Brush ringsBrush;
  unitToBrush(cRings, ringsBrush);
  EmitResult ringsC = emitCTextureDefs(ringsBrush);
  test_assert(ringsC.errors.size() == 0);
  test_assert(std::strstr(ringsC.text.c_str(), "float tex_rings_eval(const float *") !=
              nullptr);
  test_assert(std::strstr(ringsC.text.c_str(), "void tex_rings_eval_d(") != nullptr);
  test_assert(std::strstr(ringsC.text.c_str(), "static sbdual sbd_sin(") != nullptr);
  test_assert(std::strstr(ringsC.text.c_str(), "_param_defaults") == nullptr);
  test_assert(std::strstr(ringsC.text.c_str(), "texMapPoint") == nullptr);
  test_assert(std::strstr(ringsC.text.c_str(), "texRampSample") == nullptr);

  // Bands: runtime-param slab (scale at 0, ramp at 1; @const int holds no
  // slot), mapPoint ctx plumbing, and the dual twin omitted — ramp.sample has
  // no derivative rule, but that must not fail the value emission.
  ParseResult cBands = parseSrc(kParamUnitSrc, "bands.stex");
  test_assert(cBands.errors.size() == 0 && cBands.unit != nullptr);
  if (retval) {
    return;
  }

  Brush bandsBrush;
  unitToBrush(cBands, bandsBrush);
  EmitResult bandsC = emitCTextureDefs(bandsBrush);
  test_assert(bandsC.errors.size() == 0);
  test_assert(std::strstr(bandsC.text.c_str(), "const float tex_bands_param_defaults[") !=
              nullptr);
  test_assert(std::strstr(bandsC.text.c_str(), "sb_tex_params[0]") != nullptr);
  test_assert(std::strstr(bandsC.text.c_str(), "texRampSample(sb_tex_params + 1, ") !=
              nullptr);
  test_assert(std::strstr(bandsC.text.c_str(), "texMapPoint(sb_texctx, ") != nullptr);
  test_assert(std::strstr(bandsC.text.c_str(), "tex_bands_eval_d omitted") != nullptr);
  test_assert(std::strstr(bandsC.text.c_str(), "void tex_bands_eval_d(") == nullptr);

  // A sampler call is runtime-only on the precompiled C backend — the test
  // brush carries no sampler decls, the registry-path shape (T4).
  ParseResult cNoisy = parseSrc(kSamplerUnitSrc, "noisy.stex");
  test_assert(cNoisy.errors.size() == 0 && cNoisy.unit != nullptr);
  if (retval) {
    return;
  }

  Brush noisyBrush;
  unitToBrush(cNoisy, noisyBrush);
  EmitResult noisyC = emitCTextureDefs(noisyBrush);
  test_assert(noisyC.errors.size() != 0);
  bool sawSamplerErr = false;
  for (const auto &e : noisyC.errors) {
    if (std::strstr(e.c_str(), "runtime-only") != nullptr) {
      sawSamplerErr = true;
    }
  }
  test_assert(sawSamplerErr);

  // A brush-context intrinsic must be a reported error, not a stray call.
  ParseResult cBad = parseSrc(kBrushCtxUnitSrc, "badctx.stex");
  test_assert(cBad.errors.size() == 0 && cBad.unit != nullptr);
  if (retval) {
    return;
  }

  Brush badBrush;
  unitToBrush(cBad, badBrush);
  EmitResult badC = emitCTextureDefs(badBrush);
  test_assert(badC.errors.size() != 0);
  bool sawCtxErr = false;
  for (const auto &e : badC.errors) {
    if (std::strstr(e.c_str(), "requires brush context") != nullptr) {
      sawCtxErr = true;
    }
  }
  test_assert(sawCtxErr);
}

int main()
{
  runTests();
  return test_end();
}
