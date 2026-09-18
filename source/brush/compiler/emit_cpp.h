#pragma once

#include "ir.h"
#include "litestl/util/string.h"

namespace sculptcore::brush::sbrush {

struct EmitResult {
  string text;
  Vector<string> errors;
  // Non-fatal codegen diagnostics — printed by sbrushc, they do not fail the
  // build (see the attr write/save cross-check in emit_cpp.cc).
  Vector<string> warnings;
};

struct CppEmitOptions {
  // Extra (out-of-repo) kernel: scalar fields that are not member-backed
  // lower to typed Brush named store slots (kExtraSlot_<name>, assigned by
  // the registry). Off (built-in kernels): an unlisted name is a codegen
  // error — the honesty tripwire on Brush::builtinPropNames.
  bool extras = false;
};

EmitResult emitCpp(const Brush &brush, const CppEmitOptions &opts = {});

/** Shared semantic gate for C++ output and registry collection. */
Vector<string> validateCppFields(const Brush &brush, const CppEmitOptions &opts = {});

/** Factory stem shared by generated definitions and registry call sites. */
string kernelCppName(const string &attrName, const string &cppName);

/** `--texture-unit` C++ half: the guarded defaults + eval definitions plus
 * the param manifests for a scratch brush wrapping a .stex unit's textures
 * (mark them `imported` for the SB_TEX_DEF_ guards). No stage machinery. */
EmitResult emitCppTextureDefs(const Brush &brush);

/** True for the hardcoded CommandCtxBase builtins (surfaceNo, mousePos, ...),
 * which lower to `ctx.<name>` rather than `ctx.brush.<name>`. */
bool isCtxBaseName(const char *name);

/** True when `name` lowers to a Brush member (Brush::builtinPropNames). */
bool isMemberBackedName(const char *name);

/** True when `f` reads through a typed named store in an extra kernel:
 * a non-attr, non-ctx-builtin field whose name is not member-backed. Type
 * validity (float/int/bool) is the emitter's/registry's job. */
bool fieldUsesStore(const Field &f);

/** Scalar storage and native semantic eligibility for a declared field. */
bool fieldDynamicCapable(const Field &field);

} // namespace sculptcore::brush::sbrush
