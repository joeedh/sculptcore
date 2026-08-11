#pragma once

#include "binding/binding_constructor_builder.h"
#include "brushes/types.h"
#include "litestl/binding/binding.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

namespace sculptcore::brush {

using namespace litestl::util;
namespace util = litestl::util;

/** One sparse float override applied on top of a brush's authored props for the
 * duration of a single sub-command. Resolved by `name` when set (the generated
 * per-kernel uniforms — `mu`, `planeoff`, ...), else by `BrushProp` id for the
 * common props (the TS binding runtime can't marshal a JS string into a
 * `util::string` arg, so the int path stays for that bridge surface). */
struct BrushFloatOverride {
  int propId = 0;
  float value = 0.0f;
  util::string name;
};

/** Redirects one of a kernel's declared attribute handles (by its 0-based index
 * in the kernel's attr manifest) to a specific existing mesh layer (by its
 * index in that domain's AttrGroup), instead of the codegen default of
 * ensure-by-handle-name. This is how the TS attribute manager points the
 * color/poly-group/UV brushes at the user-selected "active" layer per category
 * — all ints, since the TS binding runtime can't marshal a JS string into a
 * `util::string` method arg (same reason BrushFloatOverride is propId-keyed). */
struct BrushAttrLayerOverride {
  int attrIdx = 0;    // index into BrushCommandDef::attrs (the manifest)
  int layerIndex = 0; // index into the domain's AttrGroup::attrs
};

/** A single sub-command in a composite brush program: a brush type plus a set of
 * sparse property overrides. Overrides are pushed onto the brush's authored
 * props before the command runs and rolled back after, so the brush's base
 * props survive the dab unmodified. */
struct BrushCommandEntry {
  SculptBrushes type = SculptBrushes::DRAW;
  Vector<BrushFloatOverride> floatOverrides;
  Vector<BrushAttrLayerOverride> attrLayerOverrides;
  bool overrideInvert = false;
  bool invertValue = false;
};

/** An ordered list of brush sub-commands run over the *same* node set per dab —
 * the composite-brush ("command list") abstraction the brush executor doc
 * describes. Autosmooth is a `[main, SMOOTH]` program; a future dyntopo pass is
 * just an entry prepended to `commands` with no API change. Built from TS via
 * the bound methods below and handed to `CommandExecutor::execProgram` (or the
 * grid executor's `applyProgram`). */
struct BrushProgram {
  Vector<BrushCommandEntry> commands;

  void clear()
  {
    commands.clear();
  }

  /** Append a command for the given SculptBrushes value (passed as int so the
   * binding stays a plain scalar method); returns its index. */
  int addCommand(int type)
  {
    BrushCommandEntry entry;
    entry.type = static_cast<SculptBrushes>(type);
    commands.append(std::move(entry));
    return int(commands.size()) - 1;
  }

  void setCommandFloat(int idx, int propId, float v)
  {
    if (idx < 0 || idx >= int(commands.size())) {
      return;
    }
    commands[idx].floatOverrides.append(BrushFloatOverride{propId, v});
  }

  /** Override a kernel uniform by its declared name (the generated per-kernel
   * props: `mu`, `nu`, `planeoff`, ...). Distinct from setCommandFloat, which
   * keys the common props by int id. */
  void setCommandFloatByName(int idx, util::string name, float v)
  {
    if (idx < 0 || idx >= int(commands.size())) {
      return;
    }
    BrushFloatOverride ov;
    ov.value = v;
    ov.name = name;
    commands[idx].floatOverrides.append(std::move(ov));
  }

  void setCommandInvert(int idx, bool inv)
  {
    if (idx < 0 || idx >= int(commands.size())) {
      return;
    }
    commands[idx].overrideInvert = true;
    commands[idx].invertValue = inv;
  }

  /** Redirect declared attr handle `attrIdx` of command `idx` to the mesh layer
   * at `layerIndex` in that attr's domain group (see BrushAttrLayerOverride). */
  void setCommandAttrLayer(int idx, int attrIdx, int layerIndex)
  {
    if (idx < 0 || idx >= int(commands.size())) {
      return;
    }
    commands[idx].attrLayerOverrides.append(BrushAttrLayerOverride{attrIdx, layerIndex});
  }

  static litestl::binding::types::Struct<BrushProgram> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<BrushProgram> *st = new types::Struct<BrushProgram>(
        "sculptcore::brush::BrushProgram", sizeof(BrushProgram));

    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_METHOD(st, clear, MARGS());
    BIND_STRUCT_METHOD(st, addCommand, MARGS("type"));
    BIND_STRUCT_METHOD(st, setCommandFloat, MARGS("idx", "propId", "v"));
    BIND_STRUCT_METHOD(st, setCommandFloatByName, MARGS("idx", "name", "v"));
    BIND_STRUCT_METHOD(st, setCommandInvert, MARGS("idx", "inv"));
    BIND_STRUCT_METHOD(st, setCommandAttrLayer, MARGS("idx", "attrIdx", "layerIndex"));

    return st;
  }
};

} // namespace sculptcore::brush
