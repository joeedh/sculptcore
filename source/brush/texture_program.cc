#include "texture_program.h"

#include "texture_jit.h"

#include "litestl/util/alloc.h"

#ifdef SCULPTCORE_HAVE_TCC
#include <tinycc/libtcc.h>
#endif

#if defined(SCULPTCORE_HAVE_TCC) && defined(SCULPTCORE_HAVE_TEXTURE_COMPILER)
#define SCULPTCORE_TEXTURE_PROGRAMS 1
#endif

#ifdef SCULPTCORE_TEXTURE_PROGRAMS
#include "compiler/emit_c.h"
#include "compiler/emit_wgsl.h"
#include "compiler/lexer.h"
#include "compiler/parser.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#endif

namespace sculptcore::brush {

using litestl::util::string;
using litestl::util::stringref;
using litestl::util::Vector;

TextureProgram::~TextureProgram()
{
#ifdef SCULPTCORE_HAVE_TCC
  if (jit_state) {
    tcc_delete((TCCState *)jit_state);
  }
#endif
}

void freeTextureProgram(TextureProgram *program)
{
  if (program) {
    litestl::alloc::Delete(program);
  }
}

#ifdef SCULPTCORE_TEXTURE_PROGRAMS

// The compiler IR assigns ramp slab offsets with its own copy of the runtime
// ramp size; a mismatch would put every ramp read off-slab.
static_assert(sbrush::kTexRampSize == kTexRampSize,
              "compiler/ir.h kTexRampSize out of sync with texture_eval.h");

static void appendErrors(string &error, const Vector<string> &errs)
{
  for (const auto &e : errs) {
    error += e;
    error += "\n";
  }
}

// Lex/parse errors carry line/col separately from the message.
template <typename ErrT> static void appendLineErrors(string &error, const Vector<ErrT> &errs)
{
  char buf[32];
  for (const auto &e : errs) {
    std::snprintf(buf, sizeof(buf), "%d:%d: ", e.line, e.col);
    error += buf;
    error += e.message;
    error += "\n";
  }
}

static void tccErrorSink(void *user, const char *msg)
{
  string *error = (string *)user;
  *error += msg;
  *error += "\n";
}

// The same std:: calls the precompiled cpp path makes, so a JIT'd program and
// the registry entry for the same script differ only by FP contraction.
static float jitSinf(float x)
{
  return std::sin(x);
}
static float jitCosf(float x)
{
  return std::cos(x);
}
static float jitSqrtf(float x)
{
  return std::sqrt(x);
}
static float jitFloorf(float x)
{
  return std::floor(x);
}
static float jitFabsf(float x)
{
  return std::abs(x);
}

static string lowerName(const string &s)
{
  string r = s;
  for (int i = 0; i < (int)r.size(); i++) {
    r[i] = (char)std::tolower((unsigned char)r[i]);
  }
  return r;
}

TextureProgram *compileTextureScript(stringref source, stringref filename, string &error)
{
  error = string("");

  if (!textureScriptCpuAvailable()) {
    error = string("runtime texture JIT is unavailable in this process");
    return nullptr;
  }

  sbrush::LexResult lr = sbrush::lex(source, filename);
  if (lr.errors.size() > 0) {
    appendLineErrors(error, lr.errors);
    return nullptr;
  }

  sbrush::ParseResult pr = sbrush::parse(lr.tokens, filename);
  if (pr.errors.size() > 0) {
    appendLineErrors(error, pr.errors);
    return nullptr;
  }
  if (!pr.unit) {
    error = string("not a texture unit (expected 'texture' or 'sampler' at file scope)");
    return nullptr;
  }
  if (pr.unit->textures.size() != 1) {
    error = string("a runtime texture script must define exactly one texture");
    return nullptr;
  }

  sbrush::Brush scratch;
  scratch.sourceFile = pr.unit->sourceFile;
  for (auto &td : pr.unit->textures) {
    scratch.textures.append(std::move(td));
  }
  const sbrush::TextureDef &td = scratch.textures[0];

  sbrush::EmitResult cr = sbrush::emitCTextureDefs(scratch);
  if (cr.errors.size() > 0) {
    appendErrors(error, cr.errors);
    return nullptr;
  }

  TCCState *s = tcc_new();
  if (!s) {
    error = string("tcc_new failed");
    return nullptr;
  }

  tcc_set_error_func(s, &error, tccErrorSink);
  tcc_set_options(s, "-nostdlib");
  bool ok = tcc_set_output_type(s, TCC_OUTPUT_MEMORY) == 0 &&
            tcc_add_symbol(s, "sinf", (const void *)jitSinf) == 0 &&
            tcc_add_symbol(s, "cosf", (const void *)jitCosf) == 0 &&
            tcc_add_symbol(s, "sqrtf", (const void *)jitSqrtf) == 0 &&
            tcc_add_symbol(s, "floorf", (const void *)jitFloorf) == 0 &&
            tcc_add_symbol(s, "fabsf", (const void *)jitFabsf) == 0 &&
            tcc_compile_string(s, cr.text.c_str()) == 0 && tcc_relocate(s) == 0;
  if (!ok) {
    if (error.size() == 0) {
      error = string("tcc failed to compile the emitted texture TU");
    }
    tcc_delete(s);
    return nullptr;
  }

  string lower = lowerName(td.name);
  auto *eval = (float (*)(const float *, const float *, const float *, const TexEvalCtx *))
      tcc_get_symbol(s, (string("tex_") + lower + "_eval").c_str());
  if (!eval) {
    error = string("JIT'd TU is missing tex_") + lower + "_eval";
    tcc_delete(s);
    return nullptr;
  }

  const float *slab = nullptr;
  if (td.slabSize > 0) {
    slab = (const float *)tcc_get_symbol(s, (string("tex_") + lower + "_param_defaults").c_str());
    if (!slab) {
      error = string("JIT'd TU is missing tex_") + lower + "_param_defaults";
      tcc_delete(s);
      return nullptr;
    }
  }

  TextureProgram *p = litestl::alloc::New<TextureProgram>("TextureProgram");
  p->jit_state = s;
  p->name = td.name;
  p->source = string(source);
  p->eval = eval;
  p->evalDual = (void (*)(const TexDual3 *, const TexDual3 *, const float *, const TexEvalCtx *,
                          TexDual *))tcc_get_symbol(s, (string("tex_") + lower + "_eval_d").c_str());
  p->usesMap = td.usesMap;
  p->paramSlabSize = td.slabSize;
  for (int i = 0; i < td.slabSize; i++) {
    p->defaults.append(slab[i]);
  }
  for (const auto &tp : td.texParams) {
    TextureProgramParam mp;
    mp.name = tp.name;
    mp.isRamp = tp.kind == sbrush::TexParamKind::Ramp;
    mp.isConst = tp.isConst;
    mp.def = (float)tp.defaultValue;
    mp.hasRange = tp.hasRange;
    mp.rangeMin = (float)tp.rangeMin;
    mp.rangeMax = (float)tp.rangeMax;
    mp.offset = tp.offset;
    p->params.append(mp);
  }
  for (const auto &dep : td.samplerDeps) {
    p->samplerDeps.append(dep);
  }

  // The WGSL module feeds the T5 stroke-shader splice; a dual-only emission
  // failure there must not take down the CPU program, so it is non-fatal.
  sbrush::EmitResult wr = sbrush::emitWgslTextureDefs(scratch);
  if (wr.errors.size() == 0) {
    p->wgsl = wr.text;
    // T4 refines this to "every samplerDep has a registered GPU impl".
    p->gpuAvailable = p->samplerDeps.size() == 0;
  }

  return p;
}

#else  // !SCULPTCORE_TEXTURE_PROGRAMS

TextureProgram *compileTextureScript(stringref, stringref, string &error)
{
  error = litestl::util::string(
      "runtime texture compilation is not built in (WASM builds use the precompiled registry)");
  return nullptr;
}

#endif

}  // namespace sculptcore::brush
