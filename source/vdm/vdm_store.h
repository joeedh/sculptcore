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

#include "litestl/math/vector.h"
#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <iosfwd>
#include <span>

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::vdm {
using litestl::math::float3;
namespace util = litestl::util;

struct VdmStoreParams {
  /* Texels per tile side (power of two). */
  int tile_size = 64;
  /* Texels across one UV unit ([0,1] spans `resolution` texels). */
  int resolution = 1024;
};

struct VdmTile {
  int tx = 0, ty = 0;          // tile grid coords (texel coord / tile_size)
  util::Vector<float3> texels; // tile_size² row-major
  float bound = 0.0f;          // max |texel| over the tile
  bool boundDirty = true;
  int deltaGen = 0; // last delta bracket that snapshotted this tile
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

  /* ---- sampling ---- */
  /* Bilinear over texel centers; zero outside allocated tiles. `face` is the
   * parameterization seam (unused by the atlas backend). */
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

private:
  void snapshotForDelta(int tx, int ty, VdmTile *existing);
  void removeTile(uint64_t key);

  util::Map<uint64_t, VdmTile *> tiles_;
  int tileCount_ = 0;
  VdmDelta *activeDelta_ = nullptr;
  int deltaGen_ = 0;
};

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
