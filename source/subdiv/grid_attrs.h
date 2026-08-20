/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** Per-grid-element attribute data for a multires stack: the storage policy
 * that decides where a write to a named attribute lands, and the derived
 * (subdivided-from-the-cage) sample layers the draw path reads.
 *
 * ## Storage policy
 *
 * The grids store (grids.h) can hold any number of named float channels per
 * grid element, but a host can only *persist* the ones its own file format has
 * a slot for — Blender has exactly one, a scalar paint mask per grid vertex.
 * So each attribute gets a storage class (#GridAttrStorage):
 *
 *   Host     the host declared it persistable (#declareHostAttr); brushes may
 *            write it per grid element and it survives a save/load.
 *   Temp     AttrFlag::TEMP — engine scratch the host never sees, so per-grid
 *            -element writes are always allowed regardless of what the host
 *            can store.
 *   Derived  everything else: the grid-element data is a CACHE. A brush must
 *            write the cage attribute; this class re-subdivides from it.
 *
 * Derived layers deliberately live here and not in the grids store: they are
 * recomputable by definition and must stay out of the serialized level data and
 * the undo store blob.
 *
 * ## Subdivision rules (matching Blender)
 *
 * Blender evaluates a subdivided attribute directly at the ptex (u,v) of the
 * sample rather than iterating per level, so a layer is a pure function of the
 * cage attribute and the level's sample set — "subdivide once", no accumulation.
 *
 *   generic (point + corner domains)  bilinear over the four ptex-face corner
 *       values, `quad_weights_from_uv` in blenkernel's subdiv_mesh.cc. For a
 *       quad cage face the ptex face is the whole quad (so the field is
 *       bilinear across the whole face); for an n-gon corner it is
 *       {corner, mid-to-next, face average, mid-to-prev}.
 *   UV maps  face-varying Catmull-Clark instead (Blender routes UVs through
 *       OpenSubdiv's fvar limit evaluation). Reproduced by refining the *UV
 *       cage* — the cage with its vertices split wherever incident corners
 *       disagree on UV, which is exactly OSD's fvar topology — and projecting
 *       the refined level onto the limit surface. `uvSmooth` selects the fvar
 *       linear rule; the multires modifier defaults to PRESERVE_BOUNDARIES. */

#include "mesh/attribute_enums.h"
#include "subdiv.h"

#include "litestl/math/vector.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::subdiv {

struct Multires;

/** Where per-grid-element data for one attribute may live. */
enum class GridAttrStorage : int {
  /** No grid-element storage at all (an unsupported type). */
  None = 0,
  /** The host persists it per grid element; brushes may write it there. */
  Host = 1,
  /** Engine-owned cache, re-subdivided from the cage attribute. */
  Derived = 2,
  /** Engine scratch the host never stores; brushes may always write it. */
  Temp = 3,
};

/** `uv_smooth` on Blender's multires/subsurf modifier (eSubsurfUVSmooth), which
 * selects the OpenSubdiv face-varying linear-interpolation rule. Values match
 * DNA_modifier_types.h so a host can pass its enum through unmapped. */
enum class UvSmooth : int {
  /** Fully linear fvar — the generic bilinear rule (OSD FVAR_LINEAR_ALL). */
  None = 0,
  PreserveCorners = 1,
  PreserveCornersAndJunctions = 2,
  PreserveCornersJunctionsAndConcave = 3,
  /** Smooth interior, linear along fvar boundaries. The multires default. */
  PreserveBoundaries = 4,
  /** Fully smooth (OSD FVAR_LINEAR_NONE). */
  SmoothAll = 5,
};

/** One derived per-grid-sample layer: `comps` floats per sample, laid out
 * gridCount · (S+1)² samples in the refiner's grid-major, row-major lattice
 * order (the same indexing #GridLevelDomain::gridVerts walks). */
struct GridAttrLayer {
  litestl::util::string name;
  mesh::AttrType type = mesh::AttrType::NONE;
  int comps = 0;
  bool corner = false; // cage domain: corner (face-varying) vs point
  bool uvRule = false; // fvar Catmull-Clark instead of ptex-bilinear
  int level = 0;       // level `data` was built for (0 = not built)
  bool valid = false;
  litestl::util::Vector<float> data;
};

struct MultiresAttrs {
  template <typename T> using Vector = litestl::util::Vector<T>;
  using string = litestl::util::string;

  explicit MultiresAttrs(Multires &mr) : mr_(&mr)
  {
  }
  MultiresAttrs(const MultiresAttrs &) = delete;
  MultiresAttrs &operator=(const MultiresAttrs &) = delete;

  // ---- host capability ----

  /** Declare that the host can persist `name` (of `type`) per grid element.
   * Blender declares exactly one: the scalar "mask". */
  void declareHostAttr(const string &name, mesh::AttrType type);
  void clearHostAttrs();

  /** Where a write to `name` must land. TEMP attributes are always Temp;
   * a declared (name, type) pair is Host; everything else is Derived, meaning
   * the caller must write the CAGE attribute and let this class re-derive. */
  GridAttrStorage storageFor(const string &name,
                             mesh::AttrType type,
                             mesh::AttrFlag flags) const;

  /** Whether the multires has a live sculpt-layer edit target — an enabled
   * settings row Multires::writebackChannel() attributes into. The condition
   * under which a SCULPT_LAYER kernel write has somewhere to land
   * (brush/grid_attr_bind.h). */
  bool hasLayerEditTarget() const;

  // ---- derived layers ----

  /** Subdivided samples of the cage's active UV map at `level`, or null when
   * the cage carries no UV map. `gridCount·(S+1)²` float2 in lattice order. */
  const litestl::math::float2 *uvSamples(int level);
  /** Subdivided samples of the cage's vertex `color` layer, or null. */
  const litestl::math::float4 *colorSamples(int level);
  /** Per-grid face-set color (the cage `group` face attribute run through the
   * same hash the mesh draw path uses), or null when the cage has no groups.
   * One float3 per grid — a grid is one cage corner, so it is face-constant. */
  const litestl::math::float3 *gridFaceSetColors();

  /** Per-grid-*sample* face-set colors at `level`: gridCount x (S+1)^2 float3,
   * each sample the average of its incident cells *within its own grid* --
   * never across the occurrence table, because grid id is the cage corner, so
   * every face-set boundary lies on a grid seam and the indexed draw layout
   * duplicates seam samples on purpose to keep them crisp.
   *
   * Null unless a Face-domain session channel named "group" holds data for
   * `level` -- that is what a grids-native polygroup stroke allocates, and
   * without one the per-grid #gridFaceSetColors is still the truth. */
  const litestl::math::float3 *faceSetSampleColors(int level);

  /** Recompute one grid's per-sample face-set colors from a live per-cell int
   * source: `cells` is that grid's S^2 values in row-major order. A stroke's
   * face values live in the executor's dense mirror, which the store does not
   * see until the fold, so this is how a face dab reaches the draw path.
   * No-op before the cache is built; leaves #generation alone. */
  void refreshFaceSetSampleColorsForGrid(int level, int grid, const int *cells);

  /** The same for `count` grids, sourced from the store's own Face channel --
   * the return route for an undo/redo, which swaps store bytes behind the
   * cache's back. */
  void refreshFaceSetSampleColors(int level, const int *gridIds, int count);

  /** Re-tint just `count` grids of the already-built face-set color cache from
   * the cage's current `group` values. Deliberately leaves #generation alone:
   * a cage write-back (Multires::scatterFaceIntToCage) knows exactly which
   * grids moved, and bumping the generation would send the draw source through
   * markAllData() and refill every node. No-op before the cache is built. */
  void refreshFaceSetColors(const int *gridIds, int count);

  /** Generic access: the derived layer for cage attribute `name`, building it
   * if needed. Returns null (leaving `*r_comps` at 0) when the cage has no such
   * attribute or its type is not float-backed. */
  const float *samples(int level, const string &name, int *r_comps);

  /** The same samples, for a live writer to update in place.
   *
   * A vertex layer with a *session* store channel of the same name is no
   * longer a pure function of the cage: the channel is authored paint the cage
   * cannot reproduce, so it wins wherever it exists (see #seedSessionChannel).
   * A grids-native attribute stroke holds its values in a dense mirror and
   * overlays the touched samples here per dab, which is what keeps the draw
   * path current between the stroke's start and the fold that writes the
   * store. Null when the layer does not build. */
  float *mutableSamples(int level, const string &name, int *r_comps);

  /** Copy `name`'s derived samples into the store's session channel of the
   * same name, at `level` only. The seed a first bind needs: a fresh channel
   * is zeros, and paint has to start from what the surface already shows
   * rather than from black. No-op (false) when either side is missing, when
   * the widths disagree, or when the channel already holds data for `level`
   * -- seeding twice would discard the paint. */
  bool seedSessionChannel(int level, const string &name);

  /** Re-overlay just `count` grids of `name`'s derived samples from its
   * session channel — the return route for an undo/redo, which swaps store
   * bytes behind the samples' back. Deliberately leaves #generation alone, for
   * the same reason #refreshFaceSetColors does: the caller knows which grids
   * moved, and bumping it would refill every node. */
  void refreshSamplesFromChannel(const string &name, int level, const int *gridIds, int count);

  /** Re-derive `count` grids of `name`'s sample layer from the cage, in place.
   *
   * The `Derived` storage class made partial: the cage is authoritative, so a
   * brush that just wrote cage elements uses this to bring the grids those
   * elements touch back into agreement without rebuilding the whole layer. A
   * non-persisting session channel of the same name is re-stamped too, since
   * it would otherwise overlay the pre-collapse values on the next rebuild.
   * Returns the number of grids re-derived. Deliberately leaves #generation
   * alone, for the same reason #refreshFaceSetColors does: the caller knows
   * which grids moved and marks them. */
  int refreshFromCage(int level, const string &name, const int *gridIds, int count);

  /** The cage attribute changed (a brush wrote it, or the cage was replaced):
   * drop the derived layer so the next read re-subdivides. */
  void invalidate(const string &name);
  void invalidateAll();

  /** Bumped by every invalidation, so a consumer that mirrors these samples
   * (the draw source) can tell "nothing changed" from "rebuilt" without
   * comparing the data. */
  uint64_t generation() const
  {
    return generation_;
  }

  /** Bumped whenever the CAGE's attribute values change -- an invalidation, or
   * a write-back that just moved cage elements (Multires::scatterVertFloat4ToCage
   * and its face twin). Distinct from #generation, which the write-backs
   * deliberately leave alone so the draw source refills only the grids they
   * name: this one exists for consumers that hold a whole-level derived COPY
   * and cannot be refreshed grid by grid -- the resident level slots, whose
   * meshes Multires::materialize re-derives when they fall behind. */
  uint64_t cageGeneration() const
  {
    return cageGen_;
  }

  /** Report a cage-element write the caller has already reconciled at its own
   * level. Every other level's derived copy is now behind. */
  void noteCageEdit()
  {
    cageGen_++;
  }

  /** The fvar linear rule for UV subdivision (see #UvSmooth). Setting it
   * invalidates every UV layer. */
  void setUvSmooth(UvSmooth mode);
  UvSmooth uvSmooth() const
  {
    return uvSmooth_;
  }

private:
  struct HostAttr {
    string name;
    mesh::AttrType type = mesh::AttrType::NONE;
  };

  GridAttrLayer *findLayer(const string &name);
  /** Build (or rebuild for a different level) `layer` from the cage. */
  bool buildLayer(GridAttrLayer &layer, int level);
  /** Copy the cage's per-face `name` value into every cell of each grid of a
   * Face-domain session channel -- #seedSessionChannel's face half, split out
   * because a face channel has no derived sample layer to copy from. */
  bool seedFaceSessionChannel(int level, const string &name, int channel);

  /** Overlay the store's session channel of the same name onto a just-built
   * vertex layer, so authored paint survives a rebuild. No-op unless the
   * channel exists, is a vertex channel of matching width, and holds data for
   * `level` (an untouched level has nothing to say). */
  void overlaySessionChannel(GridAttrLayer &layer, int level);
  /** Read `layer`'s cage attribute into a flat, elem-indexed float array.
   * False when the cage has no such attribute. */
  bool gatherCageValues(GridAttrLayer &layer, mesh::Mesh &cage, Vector<float> &src);
  /** Blender's generic rule: bilinear over the four ptex-face corner values. */
  void buildBilinear(GridAttrLayer &layer, int level, mesh::Mesh &cage);
  /** The UV rule: refine the UV cage, then project onto the limit surface. */
  bool buildFaceVarying(GridAttrLayer &layer, int level, mesh::Mesh &cage);

  Multires *mr_ = nullptr;
  Vector<HostAttr> hostAttrs_;
  Vector<GridAttrLayer> layers_;
  Vector<litestl::math::float3> fsetColors_;
  bool fsetValid_ = false;
  Vector<litestl::math::float3> fsetSamples_;
  int fsetSamplesLevel_ = 0;
  bool fsetSamplesValid_ = false;
  UvSmooth uvSmooth_ = UvSmooth::PreserveBoundaries;
  uint64_t generation_ = 1;
  uint64_t cageGen_ = 1;
};

/** The name of the cage's active UV map (first FLOAT2 corner layer tagged
 * AttrUse::UV), or an empty string. */
litestl::util::string activeUvName(mesh::Mesh &m);

} // namespace sculptcore::subdiv
