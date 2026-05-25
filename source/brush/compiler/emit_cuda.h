#pragma once

#include "../kernels/ir/intrinsics.h"  // BackendKind
#include "emit_cpp.h"                   // re-uses EmitResult
#include "ir.h"

namespace sculptcore::brush::sbrush {

// Wave 5 CUDA / HIP emitter.
//
// Lowers each brush to one self-contained `__global__` kernel plus a small
// device prelude (float2/3/4 + operators, scalar/vector math helpers, the
// brush_falloff / brush_strength / brush_sample_tex helpers kept in lockstep
// with brush.h, and the BrushUniforms / CtxUniforms device globals). The
// structure mirrors emit_wgsl.cc — one workgroup per spatial node, one
// thread per vertex — but emits C++-flavored device code so it can be
// syntactically compiled (clang `-x cuda` / `-x hip`, device-only, emit
// PTX/GCN) as a CI gate without a GPU.
//
// CUDA and HIP share one lowering; only the prelude's thread-index macros
// (nvvm sreg vs. amdgcn builtins) differ, selected by `target`. `target`
// must be BackendKind::Cuda or BackendKind::Hip.
//
// Scope matches the WGSL backend: brushes that use `for_neighbor` get the
// extra CSR neighbor globals; everything else lowers directly.

EmitResult emitCuda(const Brush &brush, BackendKind target);

} // namespace sculptcore::brush::sbrush
