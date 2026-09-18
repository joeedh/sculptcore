#pragma once
#include "litestl/binding/binding.h"
#include "props/prop_dynamics.h"

namespace sculptcore::brush {
/** Decode into a candidate; neither caller buffers nor output change on failure. */
int decodeResponseDynamics(const util::Vector<int> &devices,
                           const util::Vector<int> &modes,
                           const util::Vector<float> &factors,
                           const util::Vector<int> &enabled,
                           const util::Vector<int> &offsets,
                           const util::Vector<float> &samples,
                           const util::Vector<int> &kinds,
                           const util::Vector<double> &parameters,
                           props::Dynamics &output);

/** An owned scalar response; errors never expose an uninitialized value. */
struct BrushScalarResult {
  int status = 0;
  double value = 0;
  static litestl::binding::types::Struct<BrushScalarResult> *defineBindings()
  {
    using namespace litestl::binding;
    auto *st = new types::Struct<BrushScalarResult>(
        "sculptcore::brush::BrushScalarResult", sizeof(BrushScalarResult));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, status);
    BIND_STRUCT_MEMBER(st, value);
    return st;
  }
};
} // namespace sculptcore::brush
