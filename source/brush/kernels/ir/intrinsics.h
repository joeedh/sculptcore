#pragma once

#include "litestl/util/string.h"
#include "litestl/util/vector.h"
#include "../../compiler/ir.h"

// Single declaration table for all sbrush intrinsics. Lives under
// kernels/ir/ rather than compiler/ because intrinsics are kernel-author
// surface — the compiler is the engine, this is the data.
//
// Wave 1: just the minimum set needed for draw.sbrush. Each Wave 2+
// brush adds entries here. Derivative slots stay nullable until
// Wave 6 (forward-mode autodiff) wires them up.

namespace sculptcore::brush::sbrush {

using litestl::util::string;
using litestl::util::stringref;
using litestl::util::Vector;

enum class BackendKind : int {
  Cpp,
  // Wave 3+
  Wgsl,
  Spirv,
  Cuda,
  Hip,
  Opencl,
};

constexpr int BackendCount = 6;

struct IntrinsicEmit {
  // C-style format string. Placeholders:
  //   $0, $1, ... — argument expressions (already lowered)
  //   $ctx       — runtime ctx pointer for backends that thread it
  // Example: "ctx.strength($0)" or "length($0)"
  const char *pattern = nullptr;
};

struct IntrinsicDef {
  const char *name;
  TypeKind returnType;
  int arity;
  TypeKind argTypes[4];
  // One pattern slot per backend, indexed by BackendKind.
  IntrinsicEmit emit[BackendCount];
};

const IntrinsicDef *findIntrinsic(litestl::util::stringref name);
const Vector<IntrinsicDef> &allIntrinsics();

} // namespace sculptcore::brush::sbrush
