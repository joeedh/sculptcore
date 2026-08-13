#include "test_util.h"

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
 * behavior is covered separately by the texdraw sbrush-verify golden. */

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
  test_assert(std::strstr(guarded.text.c_str(), "#endif  // SB_TEX_DEF_Rings") != nullptr);
}

int main()
{
  runTests();
  return test_end();
}
