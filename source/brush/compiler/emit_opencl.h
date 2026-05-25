#pragma once
#include "../kernels/ir/intrinsics.h"
#include "emit_cpp.h"

namespace sculptcore::brush::sbrush {

// OpenCL C backend. Unlike CUDA/HIP, OpenCL 1.2 has no program-scope global
// pointers, so buffers + uniforms are kernel arguments; the brush_* helpers
// are macro-bound to those argument names. clspv lowers the result to SPIR-V.
EmitResult emitOpencl(const Brush &brush);

} // namespace sculptcore::brush::sbrush
