#pragma once

#include "emit_cpp.h"  // re-uses EmitResult
#include "ir.h"

namespace sculptcore::brush::sbrush {

// Wave 3 WGSL emitter.
//
// Each brush lowers to one compute kernel (`@compute @workgroup_size(64) fn main`)
// that processes one spatial node per workgroup, one vertex per thread, with
// vertex data fed through storage buffers and a `unique_verts` indirection
// table. Brush + ctx state are passed as uniform buffers with a fixed schema
// (see emit_wgsl.cc for layout) so every emitted kernel has the same bind
// group, which keeps the host-side dispatch simple.
//
// Scope: brushes that don't use `for_neighbor`. Brushes that do produce a
// short "skipped" WGSL stub (still valid WGSL, validates clean under tint)
// so the build doesn't have to maintain a skip-list — neighbor iteration
// arrives with mesh-connectivity buffers in a later wave.

EmitResult emitWgsl(const Brush &brush);

/** `--texture-unit` WGSL half: the module text for a scratch brush wrapping
 * a .stex unit's textures — defaults arrays, ramp helpers, the map-point
 * helper when needed, and the eval functions. Embedded in the unit's
 * .tex.gen.h for the T5 shader splice; no compute kernel around it. */
EmitResult emitWgslTextureDefs(const Brush &brush);

} // namespace sculptcore::brush::sbrush
