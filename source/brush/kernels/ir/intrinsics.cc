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

static IntrinsicDef sIntrinsicsRaw[] = {
  // strength(co) — brush falloff strength at world-space position.
  // Maps to CommandCtx::strength(co) on the C++ backend.
  INTR_CPP("strength", TypeKind::Float,  1, ARG1(TypeKind::Float3),                  "ctx.strength($0)"),
};

#undef ARG1
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
