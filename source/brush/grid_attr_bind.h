/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** Grid attribute binding (plan P2): what a kernel's declared attr layers map
 * to on the grids domain, and the dense mirror the kernel actually writes.
 *
 * The routing rule is metadata-only — it reads a BrushAttrManifestEntry, never
 * a tool name — so widening it widens the grids roster with no host or
 * executor conditional to edit. Three answers:
 *
 *  - DefaultColumn: a read-only handle whose zero value is a documented,
 *    correct default on this domain (BSMOOTH's vclass 0 = plain Laplacian).
 *    Binds an executor-owned all-zero column; no storage.
 *  - SessionChannel: a kernel-written vertex layer, backed by a GridsStore
 *    session channel (persist=false) plus the dense mirror below.
 *  - Unbindable: the brush falls back to the materialized-mesh path.
 *
 * The mirror exists because kernels index a dense mesh::AttrData by vertex id
 * while the store is per-grid with duplicated boundary samples. Gather is
 * canonical (one sample per vert, via GridLevelDomain::vertGrid); scatter
 * writes every occurrence, so duplicates stay in agreement — the same
 * round-trip GridLevelDomain already uses for mask. Both directions are a
 * memcpy, never float math, which is what keeps a typed (INT) channel legal
 * in a float-backed store.
 *
 * The store is untouched until the stroke-end fold, so undo capture of a
 * written channel is a stroke-end captureGrids() immediately before scatter —
 * exactly what positions and mask already do. */

#include "brush_command.h"

#include "mesh/attribute.h"
#include "mesh/attribute_enums.h"
#include "subdiv/grid_domain.h"
#include "subdiv/grids.h"
#include "subdiv/multires.h"

#include "litestl/util/alloc.h"
#include "litestl/util/assert.h"
#include "litestl/util/vector.h"

#include <cstring>
#include <span>

namespace sculptcore::brush {

/** Process-global kill switch for grid attribute channels, spanning plan
 * P2-P4. Off reproduces the pre-P2 roster exactly. Global because the
 * dispatch entry point (GridStroke_supported) has no session handle. */
inline bool g_gridAttrsEnabled = false;

inline bool gridAttrsEnabled()
{
  return g_gridAttrsEnabled;
}
inline void setGridAttrsEnabled(bool enable)
{
  g_gridAttrsEnabled = enable;
}

enum class GridAttrPlanKind : int {
  Unbindable = 0,
  DefaultColumn,
  SessionChannel,
};

/** Floats one element of `type` occupies in a store channel, or 0 if the
 * grids store cannot carry it. Copy-compatible types only: the float family
 * plus INT, which rides along as raw bytes. */
inline int gridAttrTypeFloats(mesh::AttrType type)
{
  switch (type) {
    case mesh::AttrType::FLOAT:
    case mesh::AttrType::INT:
      return 1;
    case mesh::AttrType::FLOAT2:
      return 2;
    case mesh::AttrType::FLOAT3:
      return 3;
    case mesh::AttrType::FLOAT4:
      return 4;
    default:
      return 0;
  }
}

/** Whether an all-zero column is a correct source for this read-only handle.
 * A registry of layers, not of brushes: `.boundary.vert.class` 0 is "interior"
 * and selects the plain-Laplacian branch, which is what the grids lattice is. */
inline bool gridAttrZeroDefault(const BrushAttrManifestEntry &entry)
{
  return entry.boundName == string(".boundary.vert.class");
}

/** The store-channel / cage-layer name a handle binds under, matching the mesh
 * path's rule (brush_executor.h): an explicit boundName wins, else the handle. */
inline const string &gridAttrLayerName(const BrushAttrManifestEntry &entry)
{
  return entry.boundName.size() ? entry.boundName : entry.handle;
}

/** Route one declared attr layer onto the grids domain. */
inline GridAttrPlanKind gridAttrPlan(const BrushAttrManifestEntry &entry, bool sessionChannels)
{
  if (!entry.kernelWrites) {
    // A read-only handle needs a source, not storage. Anything else here
    // (enhance's held displacement, the cross field) is filled by a mesh-path
    // pre-pass; binding zeros would make those brushes silent no-ops instead
    // of falling back.
    return gridAttrZeroDefault(entry) ? GridAttrPlanKind::DefaultColumn :
                                        GridAttrPlanKind::Unbindable;
  }
  if (!sessionChannels) {
    return GridAttrPlanKind::Unbindable;
  }
  if (entry.domain != AttrElemDomain::Vertex) {
    // Face/edge/corner element domains reach the store in a later phase.
    return GridAttrPlanKind::Unbindable;
  }
  if (int(entry.use) & int(mesh::AttrUse::SCULPT_LAYER)) {
    // A sculpt-layer write is a delta the displace compositor folds into
    // positions. The grids domain runs no compositor, so writing the layer
    // alone would move nothing.
    return GridAttrPlanKind::Unbindable;
  }
  return gridAttrTypeFloats(entry.type) > 0 ? GridAttrPlanKind::SessionChannel :
                                              GridAttrPlanKind::Unbindable;
}

/** A kernel-written vertex layer's dense column plus its store channel. */
struct GridAttrMirror {
  string handle;
  string layer;
  mesh::AttrType type = mesh::AttrType::FLOAT;
  int floats = 0;
  int channel = -1;
  /** Stroke sequence the column was last gathered for; a re-gather per step
   * is what re-reads store bytes an undo/redo swapped behind our back. */
  uint32_t gatheredFor = 0;
  bool dirty = false;
  mesh::AttrDataBase *column = nullptr;
};

/** resize(), not the (name, size) constructor: that one sizes the page table
 * but leaves the pages unmaterialized, and getElemData then returns null for
 * every element the mirror gathers into. */
template<class T> inline mesh::AttrDataBase *gridAttrNewTypedColumn(const string &name, int n)
{
  auto *column = alloc::New<mesh::AttrData<T>>("grid attr column", name);
  column->resize(n);
  return column;
}

/** Allocate the dense column for `type`, sized to `n` verts, or null if the
 * type is not one gridAttrTypeFloats accepts. */
inline mesh::AttrDataBase *gridAttrNewColumn(mesh::AttrType type, const string &name, int n)
{
  switch (type) {
    case mesh::AttrType::FLOAT:
      return gridAttrNewTypedColumn<float>(name, n);
    case mesh::AttrType::FLOAT2:
      return gridAttrNewTypedColumn<float2>(name, n);
    case mesh::AttrType::FLOAT3:
      return gridAttrNewTypedColumn<float3>(name, n);
    case mesh::AttrType::FLOAT4:
      return gridAttrNewTypedColumn<float4>(name, n);
    case mesh::AttrType::INT:
      return gridAttrNewTypedColumn<int>(name, n);
    default:
      return nullptr;
  }
}

/** Owner of the executor's mirrors: a domain rebuild (attach) drops them all,
 * since dense ids remap and the columns are sized to the old domain. */
struct GridAttrMirrorSet {
  Vector<GridAttrMirror *> items;

  GridAttrMirrorSet() = default;
  GridAttrMirrorSet(const GridAttrMirrorSet &) = delete;
  GridAttrMirrorSet &operator=(const GridAttrMirrorSet &) = delete;

  ~GridAttrMirrorSet()
  {
    clear();
  }

  void clear()
  {
    for (GridAttrMirror *m : items) {
      if (m->column) {
        alloc::Delete(m->column);
      }
      alloc::Delete(m);
    }
    items.clear();
  }

  GridAttrMirror *find(const string &handle)
  {
    for (GridAttrMirror *m : items) {
      if (m->handle == handle) {
        return m;
      }
    }
    return nullptr;
  }
};

/** Find or create the session channel backing `mirror` (persist=false: engine
 * owned, lazily allocated per level, seeded by addLevel, and in the serializer
 * so undo sees it). */
inline int gridAttrEnsureChannel(subdiv::Multires *mr, const GridAttrMirror &mirror)
{
  int ch = mr->store.findChannel(mirror.layer);
  if (ch < 0) {
    ch = mr->store.addChannel(mirror.layer, mirror.floats, subdiv::GridElemDomain::Vertex,
                              mirror.type, /*persist=*/false);
  }
  Assert(mr->store.channelElemSize(ch) == mirror.floats &&
             mr->store.channelDomain(ch) == subdiv::GridElemDomain::Vertex,
         "grid attr channel re-declared at another width or domain");
  return ch;
}

/** Store -> dense column, one canonical sample per vertex. */
inline void gridAttrGather(GridAttrMirror &mirror, subdiv::GridLevelDomain *domain)
{
  subdiv::Multires *mr = domain->multires();
  const int level = domain->level();
  mr->store.ensureLevelResident(level);
  const size_t bytes = size_t(mirror.floats) * sizeof(float);
  const int n = domain->vertCount();
  for (int v = 0; v < n; v++) {
    const int *g = &domain->vertGrid[v * 3];
    std::memcpy(mirror.column->getElemData(v),
                mr->store.elem(level, mirror.channel, g[0], g[1], g[2]), bytes);
  }
}

/** Dense column -> store, through every occurrence so a boundary sample stays
 * identical in all the grids that carry it. */
inline void gridAttrScatter(GridAttrMirror &mirror,
                            subdiv::GridLevelDomain *domain,
                            std::span<const int> verts)
{
  subdiv::Multires *mr = domain->multires();
  const int level = domain->level();
  mr->store.ensureLevelResident(level);
  const size_t bytes = size_t(mirror.floats) * sizeof(float);
  for (int v : verts) {
    const void *src = mirror.column->getElemData(v);
    auto occs = domain->occurrences(v);
    for (size_t i = 0; i < occs.size(); i += 3) {
      std::memcpy(mr->store.elem(level, mirror.channel, occs[i], occs[i + 1], occs[i + 2]),
                  src, bytes);
    }
  }
}

} // namespace sculptcore::brush
