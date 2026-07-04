#pragma once

/** VdmStore — the vector-displacement tile store (displacementAndSubSurf plan,
 * workstream V1; design: final-displacement-architecture.md §6).
 *
 * A sparse, tiled float3 texel store keyed by the corner-UV atlas, owned by
 * the sculpt-layer system — never by SpatialTree (texels do not enter the
 * spatial tree; the tree gets only per-face max|D| bounds, F2). Two
 * parameterization backends share the `sample(face, u, v)` seam: this atlas
 * backend ignores `face` and treats (u, v) as atlas UV; the Ptex backend
 * (workstream X2) keys per-face grids on it.
 *
 * Texel space is `uv * resolution` with texel centers at integer+0.5; tiles
 * are `tile_size`² texel blocks allocated on first write. Unallocated space
 * reads as zero displacement, so sampling never throws and a fresh store is a
 * no-op layer. Each tile carries a max|D| magnitude bound (the coarse level of
 * the bound pyramid); `exportFaceBounds` reduces those to the per-face
 * conservative bounds `SpatialTree::setFaceDisplacementBounds` consumes.
 *
 * Undo is a tile-delta channel: `beginDelta()`/`endDelta()` bracket edits (V2
 * runs one bracket inside the dab's MeshLog step), producing a self-inverse
 * `VdmDelta` — `applyDelta` swaps recorded tile contents with the live store,
 * so the same blob serves undo and redo (the LogChunkElems::swap pattern).
 */

#include "litestl/binding/binding.h"
#include "litestl/math/vector.h"
#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <iosfwd>
#include <span>

namespace sculptcore::mesh {
struct Mesh;
template <typename T> struct AttrData;
}

namespace sculptcore::vdm {
using litestl::math::float3;
namespace util = litestl::util;

/* Corner attrs a Ptex-parameterized mesh carries (written by
 * subdiv::Multires::assignGridUVs): the owning grid id and the grid-local
 * param in [0,1]² — exact, unlike the packed chart uv. */
inline constexpr const char *PTEX_GRID_ATTR = ".ptex.c.grid";
inline constexpr const char *PTEX_UV_ATTR = ".ptex.c.uv";

/* Parameterization backend (X2). ATLAS: one global uv·resolution texel plane.
 * PTEX: per-grid R_g×R_g lattices keyed on the `face` of sample(face,u,v). */
enum class VdmBackend : int { ATLAS = 0, PTEX = 1 };

struct VdmStoreParams {
  /* Texels per tile side (power of two). */
  int tile_size = 64;
  /* ATLAS: texels across one UV unit. PTEX: the default per-grid texel side
   * R_g (setGridRes overrides per grid — the adaptivity hook). */
  int resolution = 1024;
  VdmBackend backend = VdmBackend::ATLAS;
};

struct VdmTile {
  int tx = 0, ty = 0;          // tile coords (ATLAS: global; PTEX: grid-local)
  int grid = -1;               // PTEX: owning grid id (-1 on the atlas plane)
  util::Vector<float3> texels; // tile_size² row-major
  float bound = 0.0f;          // max |texel| over the tile
  bool boundDirty = true;
  int deltaGen = 0; // last delta bracket that snapshotted this tile
  /* GPU residency (vdm_gpu.h): stable atlas slot (-1 = unassigned) and the
   * pending-upload flag (in gpuDirtySlots_, or awaiting slot assignment). */
  int gpuSlot = -1;
  bool gpuDirty = false;
};

/** Self-inverse tile-content delta: `entries[i].texels` holds the *other*
 * state of tile `key` (empty = tile absent in that state). */
struct VdmDelta {
  struct Entry {
    uint64_t key = 0;
    util::Vector<float3> texels;
  };
  util::Vector<Entry> entries;

  bool empty() const
  {
    return entries.size() == 0;
  }
};

struct VdmStore {
  VdmStoreParams params;

  VdmStore() = default;
  explicit VdmStore(const VdmStoreParams &p) : params(p)
  {
  }
  ~VdmStore();
  VdmStore(const VdmStore &) = delete;
  VdmStore &operator=(const VdmStore &) = delete;

  /* ---- tiles ---- */
  static uint64_t tileKey(int tx, int ty)
  {
    return (uint64_t(uint32_t(tx)) << 32) | uint64_t(uint32_t(ty));
  }
  VdmTile *findTile(int tx, int ty) const;
  /* Find-or-allocate (zeroed); snapshots the pre-state into an open delta. */
  VdmTile &ensureTile(int tx, int ty);
  int tileCount() const
  {
    return tileCount_;
  }

  /* ---- texels (texel space; x = u·resolution) ---- */
  float3 texel(int x, int y) const; // zero when unallocated
  void writeTexel(int x, int y, const float3 &value);
  void addTexel(int x, int y, const float3 &value);

  /* ---- Ptex backend (X2): per-grid texel lattices ---- */
  /** Declare the patch space: `gridCount` grids, each an R×R lattice
   * (R = params.resolution unless setGridRes overrides). Call once on a
   * fresh PTEX store, before any write. */
  void setPtexGridCount(int gridCount);
  int ptexGridCount() const
  {
    return int(gridRes_.size());
  }
  /** R_g: the grid's texel side (0 for an out-of-range grid). */
  int gridRes(int grid) const;
  /** Per-grid resolution override (power of two; set before writing texels). */
  void setGridRes(int grid, int r);
  /** Cross-grid adjacency, 8 ints per grid ({grid, side} × 4 sides in S2's
   * GridLink order; -1 = boundary) — the skirt engine's input. The owner
   * (Multires hands over its GridsStore links) provides it so vdm stays
   * subdiv-free. */
  void setPtexAdjacency(std::span<const int> links);
  /** One-call PTEX setup for bound callers (Multires::vdmAdjacencyOut feeds
   * `links`): switches the backend, declares `gridCount` grids at
   * `defaultRes` (or params.resolution when <= 0), installs the adjacency. */
  void configurePtex(int gridCount, int defaultRes, util::Vector<int> &links);

  /* Same 32:32 packing as tileKey — a store only ever runs ONE backend, so
   * the key spaces never coexist and decode branches on params.backend. */
  static uint64_t ptexTileKey(int grid, int tileIdx)
  {
    return (uint64_t(uint32_t(grid)) << 32) | uint64_t(uint32_t(tileIdx));
  }
  VdmTile *findTileP(int grid, int ltx, int lty) const;
  VdmTile &ensureTileP(int grid, int ltx, int lty);

  /* Grid-local texels. Payload coords are [0, R_g); -1 and R_g address the
   * one-texel guard ring (the copied border skirt — architecture §6) that
   * makes clamped bilinear seamless across grids. Reads are zero and writes
   * no-ops outside [-1, R_g] (never throws). */
  float3 texelP(int grid, int x, int y) const;
  void writeTexelP(int grid, int x, int y, const float3 &value);
  void addTexelP(int grid, int x, int y, const float3 &value);

  /** Refresh `grid`'s guard ring from its neighbours' border payload through
   * the adjacency links (t preserved, roles swapped — S2's transpose
   * convention; resolutions may differ). Diagonal guards average their two
   * edge-guard neighbours. Call for a touched grid AND its link targets
   * after writing border payload; writes ride any open delta bracket. */
  void syncGridSkirts(int grid);

  /** Adjacency link target of `grid`'s `side` (GridSideType order), -1 when
   * boundary/absent — for splat-end skirt refresh of a touched grid's
   * neighbours (their guards read our border payload). */
  int gridLinkTarget(int grid, int side) const
  {
    int i = grid * 8 + side * 2;
    return i >= 0 && i + 1 < int(adjacency_.size()) ? adjacency_[i] : -1;
  }

  /** max|D| over one grid's tiles (the per-face bound export on Ptex bases). */
  float gridBound(int grid);

  /* ---- sampling ---- */
  /* Bilinear over texel centers; zero outside allocated tiles. ATLAS ignores
   * `face` and reads (u, v) as atlas UV; PTEX keys grid `face`'s lattice with
   * (u, v) as the grid-local param in [0, 1] (taps clamp to the lattice —
   * cross-grid continuity is the skirt pass's job). */
  float3 sample(int face, float u, float v) const;

  /* ---- magnitude bounds ---- */
  /* Refresh every dirty tile's max|D| bound. Call before reading bounds. */
  void updateBounds();
  float maxBound();
  /* Conservative (tile-granular) max bound over a UV rect. */
  float maxBoundInUvRect(float u0, float v0, float u1, float v1);

  /* ---- tile-delta undo bracket ---- */
  void beginDelta();
  /* Close the bracket; caller owns the returned delta (alloc::Delete it).
   * Returns nullptr when nothing was touched. */
  VdmDelta *endDelta();
  /* Swap the delta's recorded tile contents with the live store (self-inverse:
   * apply twice == no-op). Touched tiles get dirty bounds. */
  void applyDelta(VdmDelta &delta);

  /* ---- serialization (lz4 BinFile container, like serial::writeMesh) ---- */
  bool write(std::ostream &out);
  /* Read into a freshly constructed store. */
  bool read(std::istream &in);

  /* Iterate live tiles (order unspecified but stable between mutations). */
  template <typename F> void foreachTile(F &&fn) const
  {
    for (const auto &pair : tiles_) {
      if (pair.value) {
        fn(*pair.value);
      }
    }
  }

  /* ---- bound GPU-packing surface (impls in vdm_gpu.cc; marshal-safe
   * Vector out-params so both backends consume it via reflection) ---- */
  /* out = [tile_size, resolution, grid, slots, atlas_tiles_x, atlas_tiles_y,
   * atlas_w, atlas_h]; returns `slots`. Assigns slots to unslotted tiles. */
  int gpuLayoutOut(util::Vector<int> &out);
  void gpuPageTableOut(util::Vector<int> &out);
  /* PTEX: the flat per-grid offset table (vdm_gpu.h gpuPtexTable layout). */
  void gpuPtexTableOut(util::Vector<int> &out);
  void gpuAtlasPixelsOut(util::Vector<float> &out);
  int gpuTilePixelsOut(int slot, util::Vector<float> &out);
  /* Drains dirty slots; returns 1 when the page table / atlas capacity also
   * changed (re-upload the table and re-check the layout first). */
  int gpuTakeDirtyOut(util::Vector<int> &outSlots);

  static litestl::binding::types::Struct<VdmStore> *defineBindings();

  /* GPU residency state, managed by vdm_gpu.cc (slot table, free list,
   * dirty-slot queue, page-table currency). Mutators mark into these. */
  util::Vector<VdmTile *> gpuSlots_;
  util::Vector<int> gpuFreeSlots_;
  util::Vector<int> gpuDirtySlots_;
  bool gpuTopoDirty_ = false;

private:
  void snapshotForDelta(uint64_t key, VdmTile *existing);
  VdmTile &ensureTileAt(uint64_t key, int grid, int tx, int ty);
  void removeTile(uint64_t key);
  void markGpuDirty(VdmTile *t);
  int tilesPerSide(int grid) const;

  util::Map<uint64_t, VdmTile *> tiles_;
  int tileCount_ = 0;
  VdmDelta *activeDelta_ = nullptr;
  int deltaGen_ = 0;
  util::Vector<int> gridRes_;   // PTEX: per-grid R_g ([g])
  util::Vector<int> adjacency_; // PTEX: 8 ints per grid ({grid, side} × 4)
};

/** The mesh's active UV corner layer (first FLOAT2 CORNER attr tagged
 * AttrUse::UV), or null — the atlas parameterization every VDM read/write
 * keys on. */
mesh::AttrData<litestl::math::float2> *findUvCornerLayer(mesh::Mesh &m);

/** Per-face conservative max|D| bounds from the store's tile bounds: for each
 * face, the max over tiles intersecting its corner-UV bounding box (the
 * coarse-mip → per-face export feeding F2's setFaceDisplacementBounds).
 * Faces without UVs (or a mesh with no UV corner layer) export 0. Calls
 * updateBounds() first. `out` is resized to `faces.size()`, index-aligned. */
void exportFaceBounds(VdmStore &store,
                      mesh::Mesh &m,
                      std::span<const int> faces,
                      util::Vector<float> &out);

} // namespace sculptcore::vdm
