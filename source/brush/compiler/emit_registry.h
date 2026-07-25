#pragma once

#include "ir.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

namespace sculptcore::brush::sbrush {

/** One parsed extra kernel, in id order (id = SculptBrushesBuiltinCount + index). */
struct RegistryEntry {
  string stem;     // input filename minus extension; includes <stem>.brush.gen.h
  string attrName; // @brush("name")
  string cppName;  // brush <CppName> { ... }; factory is create<CppName>Brush
  bool usesNeighbor = false;
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

} // namespace sculptcore::brush::sbrush
