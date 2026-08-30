/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** Grid attribute binding (plan P2): what a kernel's declared attr layers map
 * to on the grids domain, and the dense mirror the kernel actually writes.
 *
 * The routing rule is metadata-only — it reads a BrushAttrManifestEntry, never
 * a tool name — so widening it widens the grids roster with no host or
 * executor conditional to edit. Four answers:
 *
 *  - DefaultColumn: a read-only handle whose zero value is a documented,
 *    correct default on this domain (BSMOOTH's vclass 0 = plain Laplacian).
 *    Binds an executor-owned all-zero column; no storage.
 *  - SessionChannel: a kernel-written vertex or face layer, backed by an
 *    Authored GridsStore channel plus the dense mirror below. A Host- or
 *    Temp-class layer: declareHostAttr is what says the host can store
 *    grid-element data for it, and a Derived layer is a cache of the cage
 *    that a brush must reach through the cage instead (grid_attrs.h).
 *  - LayerScratch: a written SCULPT_LAYER handle under a live edit target —
 *    executor-owned per-dab scratch the stage fold turns into co motion,
 *    which the stroke-end writeback attributes into the target layer's own
 *    channel (Multires::writebackChannel). No storage of its own.
 *  - Unbindable: the brush falls back to the materialized-mesh path.
 *
 * The mirror exists because kernels index a dense mesh::AttrData by element id
 * while the store is per-grid with duplicated boundary samples. On the vertex
 * domain gather is canonical (one sample per vert, via
 * GridLevelDomain::vertGrid) and scatter writes every occurrence, so duplicates
 * stay in agreement — the same round-trip GridLevelDomain already uses for
 * mask. The face domain has no duplication at all: a grid cell belongs to
 * exactly one grid, so both directions are the same flat cell walk. Every copy
 * is a memcpy, never float math, which is what keeps a typed (INT) channel
 * legal in a float-backed store.
 *
 * The store is untouched until the stroke-end fold, so undo capture of a
 * written channel is a stroke-end captureGrids() immediately before scatter —
 * exactly what positions and mask already do. */

#include "brush_command.h"

#include "mesh/attribute.h"
#include "mesh/attribute_enums.h"
#include "subdiv/grid_attrs.h"
#include "subdiv/grid_domain.h"
#include "subdiv/grids.h"
#include "subdiv/multires.h"

#include "litestl/util/alloc.h"
#include "litestl/util/assert.h"
#include "litestl/util/vector.h"

#include <cstring>
#include <span>

namespace sculptcore::brush {

enum class GridAttrPlanKind : int {
  Unbindable = 0,
  DefaultColumn,
  SessionChannel,
  LayerScratch,
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

/** Route one declared attr layer onto the grids domain.
 *
 * `attrs` is the live stack's storage policy (grid_attrs.h), or null where
 * there is none to ask — the pre-session dispatch probe. Null keeps the
 * type/domain answers and skips only the storage-class term, so a caller that
 * can supply it always gets the narrower answer. */
inline GridAttrPlanKind gridAttrPlan(const BrushAttrManifestEntry &entry,
                                     const subdiv::MultiresAttrs *attrs = nullptr)
{
  if (!entry.kernelWrites) {
    // A read-only handle needs a source, not storage. Anything else here
    // (enhance's held displacement, the cross field) is filled by a mesh-path
    // pre-pass; binding zeros would make those brushes silent no-ops instead
    // of falling back.
    return gridAttrZeroDefault(entry) ? GridAttrPlanKind::DefaultColumn
                                      : GridAttrPlanKind::Unbindable;
  }
  if (entry.domain != AttrElemDomain::Vertex && entry.domain != AttrElemDomain::Face) {
    // Edge/corner element domains have no grid element to land on: the store's
    // two domains are the (S+1)^2 vert lattice and the S^2 quad cells.
    return GridAttrPlanKind::Unbindable;
  }
  if (int(entry.use) & int(mesh::AttrUse::SCULPT_LAYER)) {
    // Per-dab scratch the executor folds into co; writeback then attributes
    // the residual into the live edit target's channel. Without a target
    // there is nowhere to attribute it -- decline, like the mesh path's
    // LayerEditScope, which is inert without a settings row.
    return (attrs && attrs->hasLayerEditTarget() &&
            entry.domain == AttrElemDomain::Vertex &&
            entry.type == mesh::AttrType::FLOAT3)
               ? GridAttrPlanKind::LayerScratch
               : GridAttrPlanKind::Unbindable;
  }
  if (attrs &&
      attrs->storageFor(gridAttrLayerName(entry), entry.type, mesh::AttrFlag::NONE) ==
          subdiv::GridAttrStorage::Derived)
  {
    // The storage class, enforced where the routing decision is made rather
    // than left to the channel's persist flag. A Derived attribute's grid
    // elements are a cache of the cage; a kernel that writes them is authoring
    // data the host cannot store and the next rebuild would discard. The cage
    // route (mesh path + Multires::scatterVertFloat4ToCage per dab) is the one
    // a host without multires attributes has.
    return GridAttrPlanKind::Unbindable;
  }
  return gridAttrTypeFloats(entry.type) > 0 ? GridAttrPlanKind::SessionChannel
                                            : GridAttrPlanKind::Unbindable;
}

/** A kernel-written layer's dense column plus its store channel. */
struct GridAttrMirror {
  string handle;
  string layer;
  mesh::AttrType type = mesh::AttrType::FLOAT;
  subdiv::GridElemDomain domain = subdiv::GridElemDomain::Vertex;
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
template <class T>
inline mesh::AttrDataBase *gridAttrNewTypedColumn(const string &name, int n)
{
  auto *column = alloc::New<mesh::AttrData<T>>("grid attr column", name);
  column->resize(n);
  return column;
}

/** Allocate the dense column for `type`, sized to `n` verts, or null if the
 * type is not one gridAttrTypeFloats accepts. */
inline mesh::AttrDataBase *
gridAttrNewColumn(mesh::AttrType type, const string &name, int n)
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
 * so undo sees it).
 *
 * A level with no data yet is seeded from what the surface already shows — the
 * cage's derived samples for a vertex layer, the cage's own per-face value for
 * a face one — so a layer the host also carries (colour, face sets) is painted
 * *over* rather than from black. The seed is per level and one-shot:
 * MultiresAttrs::seedSessionChannel declines a level that already holds
 * authored values. */
inline int
gridAttrEnsureChannel(subdiv::Multires *mr, const GridAttrMirror &mirror, int level)
{
  int ch = mr->store.findChannel(mirror.layer);
  if (ch < 0) {
    // The host answers the persistence half. A layer it declared through
    // declareHostAttr has a container to be saved into; a layer it did not
    // declare is session-only. Either way the channel is marked Authored, and
    // the engine's own behaviour keys off that flag.
    const bool persist =
        mr->gridAttrs().storageFor(mirror.layer, mirror.type, mesh::AttrFlag::NONE) ==
        subdiv::GridAttrStorage::Host;
    ch = mr->store.addChannel(mirror.layer,
                              mirror.floats,
                              mirror.domain,
                              mirror.type,
                              persist,
                              subdiv::GridLevelRule::Authored);
  }
  Assert(mr->store.channelElemSize(ch) == mirror.floats &&
             mr->store.channelDomain(ch) == mirror.domain,
         "grid attr channel re-declared at another width or domain");
  if (!mr->store.channelLevelAllocated(level, ch)) {
    mr->gridAttrs().seedSessionChannel(level, mirror.layer);
  }
  return ch;
}

/** Push `verts`' mirror values into the derived draw samples of the same
 * layer, at every grid occurrence so a seam vert stays identical in each.
 *
 * The draw path reads MultiresAttrs' samples, not the store, and the store
 * only learns of a stroke at the fold — so without this a colour stroke is
 * invisible until mouse-up. No-op when the layer has no derived samples (the
 * cage carries no such attribute, so nothing renders it either). */
inline void gridAttrMirrorToSamples(GridAttrMirror &mirror,
                                    subdiv::GridLevelDomain *domain,
                                    std::span<const int> verts)
{
  subdiv::Multires *mr = domain->multires();
  const int level = domain->level();
  int comps = 0;
  float *dst = mr->gridAttrs().mutableSamples(level, mirror.layer, &comps);
  if (!dst || comps != mirror.floats) {
    return;
  }
  const int w = subdiv::GridsStore::elemWidth(level, subdiv::GridElemDomain::Vertex);
  const size_t bytes = size_t(comps) * sizeof(float);
  for (int v : verts) {
    const void *src = mirror.column->getElemData(v);
    auto occs = domain->occurrences(v);
    for (size_t i = 0; i < occs.size(); i += 3) {
      const size_t sample =
          (size_t(occs[i]) * size_t(w) + size_t(occs[i + 2])) * size_t(w) +
          size_t(occs[i + 1]);
      std::memcpy(&dst[sample * size_t(comps)], src, bytes);
    }
  }
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
                mr->store.elem(level, mirror.channel, g[0], g[1], g[2]),
                bytes);
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
      std::memcpy(
          mr->store.elem(level, mirror.channel, occs[i], occs[i + 1], occs[i + 2]),
          src,
          bytes);
    }
  }
  mr->noteAttrEdit(level, mirror.channel);
}

/** The dense id of grid `g`'s cell `(u, v)` — grid-major, row-major, which is
 * also the store's own element order, so a whole grid is contiguous on both
 * sides. GridFaceIter numbers its faces by exactly this. */
inline int gridAttrFaceIndex(int grid, int u, int v, int S)
{
  return (grid * S + v) * S + u;
}

/** Store -> dense column over the S^2 cells of every grid. No canonical-sample
 * rule: a cell belongs to exactly one grid. */
inline void gridAttrGatherFace(GridAttrMirror &mirror, subdiv::GridLevelDomain *domain)
{
  subdiv::Multires *mr = domain->multires();
  const int level = domain->level();
  mr->store.ensureLevelResident(level);
  const size_t bytes = size_t(mirror.floats) * sizeof(float);
  const int S = domain->gridSide();
  const int gridCount = domain->gridCount();
  for (int g = 0; g < gridCount; g++) {
    for (int v = 0; v < S; v++) {
      for (int u = 0; u < S; u++) {
        std::memcpy(mirror.column->getElemData(gridAttrFaceIndex(g, u, v, S)),
                    mr->store.elem(level, mirror.channel, g, u, v),
                    bytes);
      }
    }
  }
}

/** Dense column -> store, for the cells of `grids` only. */
inline void gridAttrScatterFace(GridAttrMirror &mirror,
                                subdiv::GridLevelDomain *domain,
                                std::span<const int> grids)
{
  subdiv::Multires *mr = domain->multires();
  const int level = domain->level();
  mr->store.ensureLevelResident(level);
  const size_t bytes = size_t(mirror.floats) * sizeof(float);
  const int S = domain->gridSide();
  for (int g : grids) {
    for (int v = 0; v < S; v++) {
      for (int u = 0; u < S; u++) {
        std::memcpy(mr->store.elem(level, mirror.channel, g, u, v),
                    mirror.column->getElemData(gridAttrFaceIndex(g, u, v, S)),
                    bytes);
      }
    }
  }
  mr->noteAttrEdit(level, mirror.channel);
}

/** Push `grids`' face-set cells from the mirror into the draw path's per-sample
 * face-set colors, so a face dab is visible before the stroke-end fold.
 *
 * Face-set-specific by construction: "group" is the one face layer the draw
 * path has a consumer for, and MultiresAttrs turns cells into colors. Another
 * face layer binds and paints exactly the same way; it just has nothing to
 * render itself with, so this is a no-op for it. */
inline void gridAttrMirrorFaceToSamples(GridAttrMirror &mirror,
                                        subdiv::GridLevelDomain *domain,
                                        std::span<const int> grids,
                                        Vector<int> &scratch)
{
  if (mirror.layer != string("group") || mirror.type != mesh::AttrType::INT) {
    return;
  }
  subdiv::Multires *mr = domain->multires();
  const int level = domain->level();
  const int S = domain->gridSide();
  // Build the cache first if this is the stroke's first dab: a per-grid
  // refresh is a no-op before it exists, and the lazy build that follows
  // would source the store, which the mirror has not reached yet.
  mr->gridAttrs().faceSetSampleColors(level);
  scratch.resize(size_t(S) * size_t(S));
  for (int g : grids) {
    for (int v = 0; v < S; v++) {
      for (int u = 0; u < S; u++) {
        scratch[v * S + u] = *static_cast<const int *>(
            mirror.column->getElemData(gridAttrFaceIndex(g, u, v, S)));
      }
    }
    mr->gridAttrs().refreshFaceSetSampleColorsForGrid(level, g, scratch.data());
  }
}

} // namespace sculptcore::brush
