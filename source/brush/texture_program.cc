#include "texture_program.h"

#include "host_sampler.h"
#include "texture_jit.h"

#include "litestl/util/alloc.h"

#include <cctype>
#include <string>

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

#include <cmath>
#include <cstdio>
#include <cstring>
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

/** Synthesize the WGSL central-difference grad wrapper for a sampler whose
 * snippet defines only hs_<name> — the same 6-tap FD sb_hs_grad performs on
 * the CPU, so the two backends agree on grad() through the sampler. */
static void appendWgslFdGrad(string &out, const string &name, float fd_step)
{
  char h[48];
  std::snprintf(h, sizeof(h), "%.9g", (double)(fd_step > 0.0f ? fd_step : 1e-3f));
  bool hasDot = false;
  for (const char *c = h; *c; c++) {
    if (*c == '.' || *c == 'e' || *c == 'E') {
      hasDot = true;
      break;
    }
  }
  string step = string(h) + (hasDot ? "f" : ".0f");

  const string fn = string("hs_") + name;
  out += string("fn ") + fn + "_grad(p: vec3f, n: vec3f) -> vec4f {\n";
  out += string("  let h = ") + step + ";\n";
  out += string("  let v = ") + fn + "(p, n);\n";
  out += string("  let gx = (") + fn + "(p + vec3f(h, 0.0, 0.0), n) - " + fn +
         "(p - vec3f(h, 0.0, 0.0), n)) / (2.0 * h);\n";
  out += string("  let gy = (") + fn + "(p + vec3f(0.0, h, 0.0), n) - " + fn +
         "(p - vec3f(0.0, h, 0.0), n)) / (2.0 * h);\n";
  out += string("  let gz = (") + fn + "(p + vec3f(0.0, 0.0, h), n) - " + fn +
         "(p - vec3f(0.0, 0.0, h), n)) / (2.0 * h);\n";
  out += "  return vec4f(v, gx, gy, gz);\n}\n";
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
  for (auto &sd : pr.unit->samplers) {
    scratch.samplers.append(std::move(sd));
  }
  const sbrush::TextureDef &td = scratch.textures[0];

  // Every sampler the eval calls must be registered before compile — the JIT
  // binds each sb_hs_<name> handle to its registry entry's stable address.
  Vector<const HostSampler *> samplerEntries;
  for (const auto &dep : td.samplerDeps) {
    const HostSampler *hs = findHostSampler(stringref(dep.c_str()));
    if (!hs || !hs->fn) {
      error = string("sampler '") + dep + "' is not registered (registerHostSampler)";
      return nullptr;
    }
    samplerEntries.append(hs);
  }

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
            tcc_add_symbol(s, "fabsf", (const void *)jitFabsf) == 0;
  if (ok && td.samplerDeps.size() > 0) {
    ok = tcc_add_symbol(s, "sb_hs_value", (const void *)sb_hs_value) == 0 &&
         tcc_add_symbol(s, "sb_hs_grad", (const void *)sb_hs_grad) == 0;
  }
  ok = ok && tcc_compile_string(s, cr.text.c_str()) == 0 && tcc_relocate(s) == 0;
  if (!ok) {
    if (error.size() == 0) {
      error = string("tcc failed to compile the emitted texture TU");
    }
    tcc_delete(s);
    return nullptr;
  }

  // The sb_hs_<name> handles are TU-defined pointer slots (see emit_c.cc's
  // sampler prelude); point each at its registry entry now that the TU is
  // relocated.
  for (int i = 0; i < (int)td.samplerDeps.size(); i++) {
    string handle = string("sb_hs_") + td.samplerDeps[i];
    void **slot = (void **)tcc_get_symbol(s, handle.c_str());
    if (!slot) {
      error = string("JIT'd TU is missing the ") + handle + " sampler handle";
      tcc_delete(s);
      return nullptr;
    }
    *slot = (void *)(const void *)samplerEntries[i];
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
  // Sampler WGSL snippets are prepended (FD grad wrappers synthesized when a
  // snippet lacks one) so p->wgsl is self-contained; a sampler registered
  // without WGSL makes the texture CPU-only.
  sbrush::EmitResult wr = sbrush::emitWgslTextureDefs(scratch, /*paramsFromBinding=*/true);
  if (wr.errors.size() == 0) {
    bool gpu = wr.text.size() > 0;
    string prefix;
    for (int i = 0; gpu && i < (int)td.samplerDeps.size(); i++) {
      const HostSampler *hs = samplerEntries[i];
      if (hs->wgsl.size() == 0) {
        gpu = false;
        break;
      }
      prefix += hs->wgsl;
      prefix += "\n";
      string gradFn = string("fn hs_") + td.samplerDeps[i] + "_grad";
      if (!std::strstr(hs->wgsl.c_str(), gradFn.c_str())) {
        appendWgslFdGrad(prefix, td.samplerDeps[i], hs->fd_step);
      }
    }
    if (gpu) {
      p->wgsl = prefix + wr.text;
      p->gpuAvailable = true;
    }
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

string spliceTextureProgramWgsl(stringref kernelSrc, const TextureProgram &p, string &error)
{
  std::string src(kernelSrc.c_str());
  if (!p.gpuAvailable || p.wgsl.size() == 0) {
    error = string("texture program '") + p.name + "' has no GPU implementation";
    return string("");
  }

  // Only the definition site starts with "fn "; call sites are bare
  // "brush_sample_tex(". A kernel without one never samples a texture (skip
  // stubs, kernels with no strength()) — the CPU path ignores the program on
  // those too, so pass the module through untouched.
  const std::string def = "fn brush_sample_tex(";
  size_t at = src.find(def);
  if (at == std::string::npos) {
    return string(src.c_str());
  }
  if (src.find(def, at + def.size()) != std::string::npos) {
    error = string("kernel defines brush_sample_tex more than once");
    return string("");
  }
  src.replace(at, def.size(), "fn brush_sample_tex_bitmap(");

  std::string lowered;
  for (int i = 0; i < (int)p.name.size(); i++) {
    lowered += (char)std::tolower((unsigned char)p.name.c_str()[i]);
  }
  src += "\n// --- runtime texture program '";
  src += p.name.c_str();
  src += "' (T5 splice) ---\n";
  src += p.wgsl.c_str();
  src += "\nfn brush_sample_tex(co: vec3<f32>, no: vec3<f32>) -> f32 {\n";
  src += "  return tex_" + lowered + "_eval(co, no";
  if (p.usesMap) {
    src += ", ctx_u.render_matrix";
  }
  src += ");\n}\n";
  return string(src.c_str());
}

}  // namespace sculptcore::brush
