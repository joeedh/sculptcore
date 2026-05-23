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
// Helper for filling Wave-1-only intrinsics (C++ pattern only).
#define INTR_CPP(NAME, RET, ARITY, ARGS, PAT) \
  { NAME, RET, ARITY, ARGS, {{PAT}, {nullptr}, {nullptr}, {nullptr}, {nullptr}, {nullptr}} }

#define ARG1(A)         {A, TypeKind::Unknown, TypeKind::Unknown, TypeKind::Unknown}
#define ARG2(A, B)      {A, B,                  TypeKind::Unknown, TypeKind::Unknown}
#define ARG3(A, B, C)   {A, B, C,                                  TypeKind::Unknown}

static IntrinsicDef sIntrinsicsRaw[] = {
  // strength(co) — brush falloff strength at world-space position.
  // Maps to CommandCtx::strength(co) on the C++ backend.
  INTR_CPP("strength", TypeKind::Float,  1, ARG1(TypeKind::Float3),                  "ctx.strength($0)"),

  // Math — litestl::math::Vec<N,T> exposes these as member functions, so
  // the C++ patterns call them on the first arg. Other backends will fill
  // in their own (free-function) forms in Wave 3+.
  INTR_CPP("length",    TypeKind::Float,  1, ARG1(TypeKind::Float3),                  "($0).length()"),
  INTR_CPP("dot",       TypeKind::Float,  2, ARG2(TypeKind::Float3, TypeKind::Float3), "($0).dot($1)"),
  INTR_CPP("normalize", TypeKind::Float3, 1, ARG1(TypeKind::Float3),                  "($0).normalized()"),
  INTR_CPP("cross",     TypeKind::Float3, 2, ARG2(TypeKind::Float3, TypeKind::Float3), "($0).cross($1)"),
  INTR_CPP("distance",  TypeKind::Float,  2, ARG2(TypeKind::Float3, TypeKind::Float3), "(($1) - ($0)).length()"),
  INTR_CPP("mix",       TypeKind::Float,  3, ARG3(TypeKind::Float, TypeKind::Float, TypeKind::Float), "(($0) + (($1) - ($0)) * ($2))"),

  INTR_CPP("min",       TypeKind::Float,  2, ARG2(TypeKind::Float, TypeKind::Float),  "std::min($0, $1)"),
  INTR_CPP("max",       TypeKind::Float,  2, ARG2(TypeKind::Float, TypeKind::Float),  "std::max($0, $1)"),
  INTR_CPP("clamp",     TypeKind::Float,  3, ARG3(TypeKind::Float, TypeKind::Float, TypeKind::Float), "std::clamp<float>($0, $1, $2)"),
  INTR_CPP("abs",       TypeKind::Float,  1, ARG1(TypeKind::Float),                   "std::abs($0)"),
  INTR_CPP("sqrt",      TypeKind::Float,  1, ARG1(TypeKind::Float),                   "std::sqrt($0)"),
};

#undef ARG1
#undef ARG2
#undef ARG3
#undef INTR_CPP

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
