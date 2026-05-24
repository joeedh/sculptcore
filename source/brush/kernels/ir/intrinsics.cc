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
// used. Wave 3 adds the WGSL slot; SPIR-V / CUDA / HIP / OpenCL stay
// nullptr until their waves.
#define INTR_CW(NAME, RET, ARITY, ARGS, CPP, WGSL) \
  { NAME, RET, ARITY, ARGS, {{CPP}, {WGSL}, {nullptr}, {nullptr}, {nullptr}, {nullptr}} }

#define ARG1(A)         {A, TypeKind::Unknown, TypeKind::Unknown, TypeKind::Unknown}
#define ARG2(A, B)      {A, B,                  TypeKind::Unknown, TypeKind::Unknown}
#define ARG3(A, B, C)   {A, B, C,                                  TypeKind::Unknown}

static IntrinsicDef sIntrinsicsRaw[] = {
  // strength(co) — brush falloff strength at world-space position.
  // C++ delegates to CommandCtx::strength; WGSL inlines via the
  // brush_strength helper that emit_wgsl writes once per kernel (kept in
  // sync with brush_command.h:55 by hand for now).
  INTR_CW("strength", TypeKind::Float,  1, ARG1(TypeKind::Float3),
          "ctx.strength($0)",         "brush_strength($0)"),

  // Math — C++ uses litestl::math::Vec<N,T> member calls; WGSL has
  // free-function builtins with matching names.
  INTR_CW("length",    TypeKind::Float,  1, ARG1(TypeKind::Float3),
          "($0).length()",            "length($0)"),
  INTR_CW("dot",       TypeKind::Float,  2, ARG2(TypeKind::Float3, TypeKind::Float3),
          "($0).dot($1)",             "dot($0, $1)"),
  INTR_CW("normalize", TypeKind::Float3, 1, ARG1(TypeKind::Float3),
          "($0).normalized()",        "normalize($0)"),
  INTR_CW("cross",     TypeKind::Float3, 2, ARG2(TypeKind::Float3, TypeKind::Float3),
          "($0).cross($1)",           "cross($0, $1)"),
  INTR_CW("distance",  TypeKind::Float,  2, ARG2(TypeKind::Float3, TypeKind::Float3),
          "(($1) - ($0)).length()",   "distance($0, $1)"),
  INTR_CW("mix",       TypeKind::Float,  3, ARG3(TypeKind::Float, TypeKind::Float, TypeKind::Float),
          "(($0) + (($1) - ($0)) * ($2))", "mix($0, $1, $2)"),

  INTR_CW("min",       TypeKind::Float,  2, ARG2(TypeKind::Float, TypeKind::Float),
          "std::min($0, $1)",         "min($0, $1)"),
  INTR_CW("max",       TypeKind::Float,  2, ARG2(TypeKind::Float, TypeKind::Float),
          "std::max($0, $1)",         "max($0, $1)"),
  INTR_CW("clamp",     TypeKind::Float,  3, ARG3(TypeKind::Float, TypeKind::Float, TypeKind::Float),
          "std::clamp<float>($0, $1, $2)", "clamp($0, $1, $2)"),
  INTR_CW("abs",       TypeKind::Float,  1, ARG1(TypeKind::Float),
          "std::abs($0)",             "abs($0)"),
  INTR_CW("sqrt",      TypeKind::Float,  1, ARG1(TypeKind::Float),
          "std::sqrt($0)",            "sqrt($0)"),
};

#undef ARG1
#undef ARG2
#undef ARG3
#undef INTR_CW

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
