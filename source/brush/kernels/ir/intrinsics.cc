#include "intrinsics.h"
#include <cstring>

namespace sculptcore::brush::sbrush {

using litestl::util::Vector;
using litestl::util::stringref;

namespace {

// Wave 1 intrinsics — just the minimum to express draw.sbrush.
// The kernels/ir/ author surface grows as new brushes need more ops:
// length/dot/distance/min/max/mix in Wave 2, sample_tex/sample_falloff
// in Wave 2's falloff & texture work, neighbor_* in Wave 2's smooth
// support, bvh_query in Wave 4's global brushes.
// Pattern slots are indexed by BackendKind; nullptr means "not lowered on
// this backend" and the emitter will report an error if the intrinsic is
// used. Wave 3 adds the WGSL slot; SPIR-V reaches GPU via the WGSL slot
// (lowered through tint), so it stays nullptr. Wave 5 fills the CUDA + HIP
// slots — both share one pattern (GPU) since the emit_cuda prelude defines
// the same helper/math surface for either target. The OpenCL slot tracks
// WGSL's native math, differing only in spelling (fmin/fmax/fabs, no fract).
#define INTR_CW(NAME, RET, ARITY, ARGS, CPP, WGSL) \
  { NAME, RET, ARITY, ARGS, {{CPP}, {WGSL}, {nullptr}, {nullptr}, {nullptr}, {nullptr}} }
#define INTR_CWG(NAME, RET, ARITY, ARGS, CPP, WGSL, GPU) \
  { NAME, RET, ARITY, ARGS, {{CPP}, {WGSL}, {nullptr}, {GPU}, {GPU}, {nullptr}} }
// OpenCL C has native float2/3/4 + matching free-function math builtins, so
// its slot tracks WGSL closely; brush_strength/falloff/sample_tex come from
// the emit_opencl prelude. `OCL` differs only where the spelling does.
#define INTR_CWGO(NAME, RET, ARITY, ARGS, CPP, WGSL, GPU, OCL) \
  { NAME, RET, ARITY, ARGS, {{CPP}, {WGSL}, {nullptr}, {GPU}, {GPU}, {OCL}} }

#define ARG0()          {TypeKind::Unknown, TypeKind::Unknown, TypeKind::Unknown, TypeKind::Unknown}
#define ARG1(A)         {A, TypeKind::Unknown, TypeKind::Unknown, TypeKind::Unknown}
#define ARG2(A, B)      {A, B,                  TypeKind::Unknown, TypeKind::Unknown}
#define ARG3(A, B, C)   {A, B, C,                                  TypeKind::Unknown}

static IntrinsicDef sIntrinsicsRaw[] = {
  // strength(co) — brush slider x distance falloff at a world-space position,
  // invert-signed. Spatial + scalar only: the per-vertex masking factors are
  // automasks()/masks() below, so `strength(co) * masks()` applies each factor
  // exactly once. Kernels whose field has unbounded support (no radius at which
  // it dies) must not call this — the field is its own falloff.
  INTR_CWGO("strength", TypeKind::Float,  1, ARG1(TypeKind::Float3),
          "ctx.strength($0)",         "brush_strength($0)",     "brush_strength($0)", "brush_strength($0)"),

  // automasks() / masks() — per-vertex masking, no spatial term. Deliberately
  // argument-free: nothing here depends on position, so the signature can't
  // silently regrow a falloff. Both hidden operands ride in on placeholders —
  // `$v` (current vertex index) and `$vm` (the kernel's live painted mask).
  //   automasks() — cavity automask x view-normal; the masks a kernel cannot
  //                 otherwise reach. Used by kernels that *write* v.mask.
  //   masks()     — automasks() x (1 - painted mask). The common case.
  // CUDA/HIP/OpenCL have no automask binding yet, so they degrade to the
  // painted mask alone (matching the pre-split 1-arg brush_strength there).
  INTR_CWGO("automasks", TypeKind::Float, 0, ARG0(),
          "ctx.automasks($v)",        "brush_automasks($v)",    "1.0f",                   "1.0f"),
  INTR_CWGO("masks",     TypeKind::Float, 0, ARG0(),
          "ctx.masks($v, $vm)",       "brush_masks($v, $vm)",   "brush_masks($v, $vm)",   "brush_masks($v, $vm)"),

  // unbounded_window(co) — the C1 cutoff an @unbounded kernel must apply so its
  // field reaches exactly zero at the host's spatial-node filter radius. Every
  // @unbounded brush is required to call it (sema enforces); nothing else may.
  INTR_CWGO("unbounded_window", TypeKind::Float, 1, ARG1(TypeKind::Float3),
          "ctx.unboundedWindow($0)",  "brush_unbounded_window($0)",
          "brush_unbounded_window($0)", "brush_unbounded_window($0)"),

  // falloff(t) — raw curve sample at normalized centerwise input
  // (1 at brush center, 0 at radius). Dispatches on the brush's
  // FalloffKind selector. Brushes that just want the standard
  // strength*falloff*radius blend should keep using strength(co); this
  // is the lower-level primitive for kernels that compute t themselves.
  INTR_CWGO("falloff",  TypeKind::Float,  1, ARG1(TypeKind::Float),
          "ctx.brush.falloffEval($0)", "brush_falloff($0)", "brush_falloff($0)", "brush_falloff($0)"),

  // sampleBrushTex(co, no) — brush-texture modulation at world point `co`
  // with surface normal `no`, mapped to UV per the brush's TexCoordSpace.
  // Returns 1.0 when no texture is bound, so kernels multiply by it freely.
  // C++ delegates to CommandCtx::sampleBrushTex; WGSL inlines via the
  // brush_sample_tex helper emit_wgsl writes once per kernel.
  INTR_CWGO("sampleBrushTex", TypeKind::Float, 2, ARG2(TypeKind::Float3, TypeKind::Float3),
          "ctx.sampleBrushTex($0, $1)", "brush_sample_tex($0, $1)", "brush_sample_tex($0, $1)", "brush_sample_tex($0, $1)"),

  // Math — C++ uses litestl::math::Vec<N,T> member calls; WGSL has
  // free-function builtins with matching names; CUDA/HIP use the
  // sc_* float3 helpers from the emit_cuda prelude (no vector builtins
  // under -nogpuinc) and the *f scalar math functions.
  INTR_CWGO("length",    TypeKind::Float,  1, ARG1(TypeKind::Float3),
          "($0).length()",            "length($0)",        "sc_length($0)",      "length($0)"),
  INTR_CWGO("dot",       TypeKind::Float,  2, ARG2(TypeKind::Float3, TypeKind::Float3),
          "($0).dot($1)",             "dot($0, $1)",       "sc_dot($0, $1)",     "dot($0, $1)"),
  INTR_CWGO("normalize", TypeKind::Float3, 1, ARG1(TypeKind::Float3),
          "($0).normalized()",        "normalize($0)",     "sc_normalize($0)",   "normalize($0)"),
  INTR_CWGO("cross",     TypeKind::Float3, 2, ARG2(TypeKind::Float3, TypeKind::Float3),
          "($0).cross($1)",           "cross($0, $1)",     "sc_cross($0, $1)",   "cross($0, $1)"),
  INTR_CWGO("distance",  TypeKind::Float,  2, ARG2(TypeKind::Float3, TypeKind::Float3),
          "(($1) - ($0)).length()",   "distance($0, $1)",  "sc_length(($1) - ($0))", "distance($0, $1)"),
  INTR_CWGO("mix",       TypeKind::Float,  3, ARG3(TypeKind::Float, TypeKind::Float, TypeKind::Float),
          "(($0) + (($1) - ($0)) * ($2))", "mix($0, $1, $2)", "(($0) + (($1) - ($0)) * ($2))", "mix($0, $1, $2)"),

  INTR_CWGO("min",       TypeKind::Float,  2, ARG2(TypeKind::Float, TypeKind::Float),
          "std::min($0, $1)",         "min($0, $1)",       "fminf($0, $1)",      "fmin($0, $1)"),
  INTR_CWGO("max",       TypeKind::Float,  2, ARG2(TypeKind::Float, TypeKind::Float),
          "std::max($0, $1)",         "max($0, $1)",       "fmaxf($0, $1)",      "fmax($0, $1)"),
  INTR_CWGO("clamp",     TypeKind::Float,  3, ARG3(TypeKind::Float, TypeKind::Float, TypeKind::Float),
          "std::clamp<float>($0, $1, $2)", "clamp($0, $1, $2)", "sb_clampf($0, $1, $2)", "clamp($0, $1, $2)"),
  INTR_CWGO("abs",       TypeKind::Float,  1, ARG1(TypeKind::Float),
          "std::abs($0)",             "abs($0)",           "fabsf($0)",          "fabs($0)"),
  INTR_CWGO("sqrt",      TypeKind::Float,  1, ARG1(TypeKind::Float),
          "std::sqrt($0)",            "sqrt($0)",          "sqrtf($0)",          "sqrt($0)"),

  // Trig / rounding — needed by inline procedural textures. WGSL has
  // matching builtins; C++ uses <cmath>; CUDA/HIP use the *f scalar
  // device math. fract has no builtin anywhere, so it lowers to
  // x - floor(x).
  INTR_CWGO("sin",       TypeKind::Float,  1, ARG1(TypeKind::Float),
          "std::sin($0)",             "sin($0)",           "sinf($0)",           "sin($0)"),
  INTR_CWGO("cos",       TypeKind::Float,  1, ARG1(TypeKind::Float),
          "std::cos($0)",             "cos($0)",           "cosf($0)",           "cos($0)"),
  INTR_CWGO("floor",     TypeKind::Float,  1, ARG1(TypeKind::Float),
          "std::floor($0)",           "floor($0)",         "floorf($0)",         "floor($0)"),
  INTR_CWGO("fract",     TypeKind::Float,  1, ARG1(TypeKind::Float),
          "(($0) - std::floor($0))",  "fract($0)",         "(($0) - floorf($0))", "(($0) - floor($0))"),
};

#undef ARG1
#undef ARG2
#undef ARG3
#undef INTR_CW
#undef INTR_CWG
#undef INTR_CWGO

static Vector<IntrinsicDef> &table()
{
  static Vector<IntrinsicDef> *t = [] {
    auto *v = new Vector<IntrinsicDef>();
    for (auto &d : sIntrinsicsRaw) v->append(d);
    return v;
  }();
  return *t;
}

} // namespace

const IntrinsicDef *findIntrinsic(stringref name)
{
  for (const auto &d : table()) {
    if (std::strcmp(d.name, name.c_str()) == 0) return &d;
  }
  return nullptr;
}

const Vector<IntrinsicDef> &allIntrinsics() { return table(); }

} // namespace sculptcore::brush::sbrush
