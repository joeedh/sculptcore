#pragma once

#include "gpu/types.h"
#include "litestl/util/string.h"

namespace sculptcore::gpu {

/* How a requested attribute's vertex buffer is filled when the mesh has no
 * matching source layer. Zero = (0,0,0,0); White = (1,1,1,1) (the legacy
 * default for a missing per-vertex color stream). */
enum class AttrDefaultKind {
  Zero = 0,
  White = 1,
};

/* One geometry attribute a material's shader reads, handed down from the TS
 * renderengine to the spatial tree. The tree builds one vertex buffer per
 * entry (in `slot` order, after the implicit position@0 / normal@1), gathering
 * from the mesh attribute named `name` of `srcType` on `domain`; when that
 * layer is absent it default-fills by `defaultKind` and flags the slot in
 * SpatialTree::missingAttrSlots. It never throws — a missing attr renders with
 * defaults rather than aborting the frame.
 *
 * `srcType` / `domain` are raw integer mirrors of the mesh enums so the struct
 * stays dependency-free across the C-API seam:
 *   srcType = sculptcore::mesh::AttrType  (FLOAT=1, FLOAT2=2, FLOAT3=4, FLOAT4=8, …)
 *   domain  = TS AttrDomain flag          (VERTEX=1, EDGE=2, CORNER=4, FACE=16) */
struct RequestedAttr {
  litestl::util::string name;
  int srcType = 0;
  GPUType gpuType = GPUType::FLOAT32;
  int elemSize = 0;
  int slot = 0;
  int domain = 1;
  AttrDefaultKind defaultKind = AttrDefaultKind::Zero;
};

} // namespace sculptcore::gpu
