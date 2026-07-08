#pragma once

/** Sculpt-layer settings sidecar (see documentation/sculpt-layers-design.md §2).
 *
 * A vertex sculpt layer is a FLOAT3 VERTEX attribute tagged
 * AttrUse::SCULPT_LAYER plus one SculptLayerSettings record on the mesh,
 * keyed by attribute name. The records live here (not in source/displace/)
 * because mesh_serialize.cc persists them with the mesh and the mesh module
 * cannot depend on displace; the compositor that *evaluates* the stack lives
 * in source/displace/.
 */

#include "litestl/binding/binding.h"
#include "litestl/util/string.h"

namespace sculptcore::mesh {

/* SculptLayerSettings.mode — how stored values map to world displacement.
 * DELTA is the polygon default (exact under dyntopo interp, commutative);
 * TANGENT is reserved for the subsurf-multires path (workstream S). */
enum class SculptLayerMode { DELTA = 0, TANGENT = 1 };
/* SculptLayerSettings.space — reference frame for DELTA mode. The engine
 * works in object space, so WORLD and OBJECT are currently synonyms. */
enum class SculptLayerSpace { WORLD = 0, OBJECT = 1 };

/** Name of the TEMP FLOAT3 vertex column holding the edit target's rest
 * positions (V2 implicit-active model): rest(v) = co(v) − d_active(v),
 * snapshotted when a layer becomes the edit target. Folding derives the
 * layer's delta as co − rest. Never serialized (AttrFlag::TEMP). */
constexpr const char *SCULPT_LAYER_REST_ATTR = ".slayer.rest";

struct SculptLayerSettings {
  /* Name of the VERTEX FLOAT3 attribute this record describes. */
  litestl::util::string name;
  int mode = int(SculptLayerMode::DELTA);
  int space = int(SculptLayerSpace::WORLD);
  /* Index of the layer this one is relative to (-1 = base). Unused by the
   * DELTA compositor (deltas are absolute); reserved for TANGENT stacks. */
  int parent = -1;
  float weight = 1.0f;
  bool enabled = true;
  /* Excluded from active editing (brush writes), still composited. */
  bool frozen = false;
  /* α in the fold-bound clamp |D| ≤ α·ρ_min (VDM / TANGENT carriers only;
   * inert for DELTA layers). */
  float clampFrac = 0.5f;

  static litestl::binding::types::Struct<SculptLayerSettings> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<SculptLayerSettings> *st = new types::Struct<SculptLayerSettings>(
        "sculptcore::mesh::SculptLayerSettings", sizeof(SculptLayerSettings));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, name);
    BIND_STRUCT_MEMBER(st, mode);
    BIND_STRUCT_MEMBER(st, space);
    BIND_STRUCT_MEMBER(st, parent);
    BIND_STRUCT_MEMBER(st, weight);
    BIND_STRUCT_MEMBER(st, enabled);
    BIND_STRUCT_MEMBER(st, frozen);
    BIND_STRUCT_MEMBER(st, clampFrac);
    return st;
  }
};

} // namespace sculptcore::mesh
