#pragma once

#include "emit_cpp.h"  // EmitResult
#include "ir.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

namespace sculptcore::brush::sbrush {

/** A scalar-float uniform resolved to the Brush.namedFloats store (not
 * member-backed); the registry dedupes these by name across all extras and
 * assigns dense kExtraSlot_<name> indices. */
struct StoreUniform {
  string name;
  double def = 0.0; // DSL `= <n>` default (0 when absent)
};

/** One parsed extra kernel, in id order (id = SculptBrushesBuiltinCount + index). */
struct RegistryEntry {
  string stem;     // input filename minus extension; includes <stem>.brush.gen.h
  string attrName; // @brush("name")
  string cppName;  // brush <CppName> { ... }; factory is create<CppName>Brush
  bool usesNeighbor = false;
  bool fullTopo = false;  // `@fulltopo`
  bool faceStage = false; // has a `face` stage, so it walks the face loop
  Vector<StoreUniform> storeUniforms;
};

struct RegistryResult {
  string enumInc;   // sculptcore_extra_brushes_enum.inc content
  string genHeader; // sculptcore_extra_brushes.gen.h content
  Vector<string> errors;
};

/** Validate extras against the built-ins and emit the extra-brush registry
 * sources. `builtinStems`/`builtinAttrNames` come from parsing the --builtin
 * inputs; `reserved` holds the built-in SculptBrushes enum item names, passed
 * literally because they are not derivable from @brush names (FEATURE_ALIGN
 * vs "featurealign"). Errors leave enumInc/genHeader empty. */
RegistryResult emitRegistry(const Vector<RegistryEntry> &extras,
                            const Vector<string> &builtinStems,
                            const Vector<string> &builtinAttrNames,
                            const Vector<string> &reserved);

/** One parsed built-in kernel, for the built-in registry. `tools` is the kernel's
 * `@tool NAME[, ...]` list; a kernel with none is compiled but dispatched by no
 * tool (it takes no id and is left out of the generated header entirely). */
struct BuiltinEntry {
  string stem;
  Vector<string> tools;
  bool usesNeighbor = false;
  bool fullTopo = false;
  bool faceStage = false;
  bool gpu = false; // `@gpu` — has a GPU (WGSL/SPIR-V) port
};

struct BuiltinRegistryResult {
  string enumInc;   // brushes/generated/builtin_brushes_enum.inc content
  string genHeader; // brushes/generated/builtin_brushes.gen.h content
  Vector<string> errors;
};

/** Emit the built-in brush registry: the `Binder<SculptBrushes>` item list and
 * the id-keyed factory dispatch. `toolIds` is brushes/tools.txt in id order —
 * the ids are not derivable from the kernels (one kernel serves several tools,
 * and FEATURE_ALIGN is not "featurealign" uppercased). Every name in `toolIds`
 * must be claimed by exactly one kernel's @tool list, and vice versa. */
BuiltinRegistryResult emitBuiltinRegistry(const Vector<BuiltinEntry> &kernels,
                                          const Vector<string> &toolIds);

/** Compose one `<stem>.tex.gen.h` for a .stex unit: the guarded C++ eval
 * definitions + param manifests plus the unit's WGSL module text embedded as
 * a string constant. Moves the unit's textures into a scratch brush (the
 * unit is consumed). */
EmitResult emitTextureUnitHeader(TextureUnit &unit, const string &stem);

/** Emit sculptcore_textures.gen.h: one TextureRegistryEntry row per texture
 * across all units, includes for their `<stem>.tex.gen.h`. Duplicate texture
 * names and duplicate stems are errors. `stems` pairs with `units`. */
EmitResult emitTextureRegistry(const Vector<string> &stems,
                               const Vector<const TextureUnit *> &units);

} // namespace sculptcore::brush::sbrush
