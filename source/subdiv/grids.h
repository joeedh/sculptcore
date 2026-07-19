#pragma once

/** Multires grids store (displacementAndSubSurf plan, S2). One grid per cage
 * face corner — the per-quadrant-after-one-split / Ptex `__faceindex`
 * convention, matching the Refiner's grid enumeration (cage face id order,
 * loop order within a face). Level L holds (2^(L-1)+1)^2 verts per grid.
 *
 * Channels are per-level flat float arrays over grid verts: channel 0 is the
 * always-present "disp" float3 — the level's displacement relative to the
 * smoothed previous level, expressed in the level's tangent frame (frames are
 * computed elsewhere; the store is frame-agnostic). Storage is chunked by
 * whole grids with an offset-table-headed serialized form, so a later
 * disk-backing pass can page chunks without a format change (X5).
 *
 * Topology is implicit: within a grid, neighbors are ±1 u/v strides; grid
 * boundaries carry 4 links to adjacent grids (same-face neighbors on the
 * right/top sides, across-cage-edge neighbors on the left/bottom sides), each
 * a transpose mapping (param t preserved, u/v roles swapped). Boundary verts
 * are REPLICATED in every grid that contains them; seamMates enumerates the
 * aliases of a boundary coord so writers can keep replicas in sync (S4). */

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <iosfwd>

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::subdiv {

/** Bump when the on-disk layout changes (mirrors mesh serial versioning). */
inline constexpr uint32_t kGridsFormatVersion = 1;

/** A grid side. LEFT/BOTTOM cross a cage edge (absent on mesh boundary);
 * RIGHT/TOP always link to a same-face neighbor grid. Side coords, param t:
 * LEFT (0,t), RIGHT (S,t), BOTTOM (t,0), TOP (t,S). */
enum GridSideType : int {
  GRID_SIDE_LEFT = 0,
  GRID_SIDE_BOTTOM = 1,
  GRID_SIDE_RIGHT = 2,
  GRID_SIDE_TOP = 3,
};

struct GridLink {
  int grid = -1; // -1: cage boundary or non-manifold/inconsistent winding
  int side = -1; // the GridSideType this seam is on the neighbor grid
};

struct GridCoord {
  int grid = -1;
  int u = 0, v = 0;

  bool operator==(const GridCoord &b) const
  {
    return grid == b.grid && u == b.u && v == b.v;
  }
};

struct GridsStore {
  /** Vert-lattice side length exponent: level L grids are S x S quad cells,
   * S = 2^(L-1), with (S+1)^2 verts. Levels are 1-based (level 1 = the first
   * subdivision, one quad per grid). */
  static int sideForLevel(int level)
  {
    return 1 << (level - 1);
  }

  /** Derive gridCount + the 4 per-grid links from a cage mesh, enumerating
   * grids exactly like Refiner::refine. Thaws frozen topology. Resets any
   * existing levels/channel data (topology defines the store). */
  void buildFromCage(mesh::Mesh &cage);

  /** Append the next level (level == levelCount()+1 after the call); every
   * channel gets zero-filled storage for it. */
  void addLevel();

  /** Drop the finest level (level == levelCount()) from every channel — the
   * inverse of addLevel(). No-op when empty. */
  void dropTopLevel();

  int levelCount() const
  {
    return levelCount_;
  }

  int gridCount() const
  {
    return gridCount_;
  }

  /** Add a named channel of 1..4 floats per grid vert; allocates (zeroed)
   * storage for all existing levels. Returns the channel index. Channel 0 is
   * always "disp" (3 floats). */
  int addChannel(const litestl::util::string &name, int floatsPerElem);

  int channelCount() const
  {
    return int(channels_.size());
  }

  /** Channel index for `name`, or -1. Channel 0 is always "disp". */
  int findChannel(const litestl::util::string &name) const
  {
    for (int i = 0; i < int(channels_.size()); i++) {
      if (channels_[i].name == name) {
        return i;
      }
    }
    return -1;
  }

  /** Drop channel `channel` (its storage across every level, resident or
   * evicted). Refuses channel 0; later channel indices shift down by one. */
  void removeChannel(int channel)
  {
    if (channel < 1 || channel >= int(channels_.size())) {
      return;
    }
    // Vector::remove_at move-assigns onto a destructed slot, which
    // double-frees Channel's nested Vectors — shift live + pop instead.
    for (int i = channel; i + 1 < int(channels_.size()); i++) {
      channels_[i] = std::move(channels_[i + 1]);
    }
    channels_.pop_back();
  }

  const litestl::util::string &channelName(int channel) const
  {
    return channels_[channel].name;
  }

  int channelElemSize(int channel) const
  {
    return channels_[channel].floatsPerElem;
  }

  /** Pointer to the floatsPerElem floats of one grid vert. O(1). */
  float *elem(int level, int channel, int grid, int u, int v);
  const float *elem(int level, int channel, int grid, int u, int v) const;

  /** One lattice step (|du| + |dv| == 1) from `c` at `level`, crossing into
   * the adjacent grid at seams. False when the step exits the cage (mesh
   * boundary) — there is no vert there. The result is always lattice- (and
   * hence mesh-edge-) adjacent to `c`. */
  bool neighbor(int level, const GridCoord &c, int du, int dv, GridCoord &out) const;

  /** All OTHER (grid,u,v) coords aliasing the same surface vert as `c` — the
   * replicas a seam write must sync. Empty for grid-interior coords. */
  void seamMates(int level, const GridCoord &c, litestl::util::Vector<GridCoord> &out) const;

  const GridLink &link(int grid, int side) const
  {
    return links_[grid * 4 + side];
  }

  /** Serialize as a BinFile + lz4 blob (the writeMesh container shape); the
   * payload is an offset-table header followed by the raw chunks. */
  bool write(std::ostream &out);
  /** Read a write() blob into this store (replaces all contents). */
  bool read(std::istream &in);

  /** Exposed for tests/pager: chunk geometry of one (channel, level). Chunks
   * hold whole grids; a grid never straddles chunks. */
  int gridsPerChunk(int level, int channel) const
  {
    return channels_[channel].levels[level - 1].gridsPerChunk;
  }
  int chunkCount(int level, int channel) const
  {
    return int(channels_[channel].levels[level - 1].chunks.size());
  }

  // ---- X5: compressed eviction (activates the chunked layout) ----
  // A level's chunks can be evicted to one lz4 blob per channel and
  // rehydrated transparently on the next elem() touch — synchronous and
  // backend-agnostic (wasm has no synchronous disk IO; a native mmap/spill
  // pass can layer under the same seam later). Chunk geometry is
  // deterministic from (gridCount, level, floatsPerElem), so the blob needs
  // no layout header.
  /** Compress + free every channel's chunks for `level` (no-op if already
   * evicted). Safe for any level: readers self-heal through elem(). */
  void evictLevel(int level);
  /** Rehydrate `level` in every channel (no-op when resident). */
  void ensureLevelResident(int level);
  bool levelResident(int level) const;
  /** Live (uncompressed) chunk bytes across all channels/levels. */
  size_t residentBytes() const;
  /** Compressed bytes held for evicted levels. */
  size_t evictedBytes() const;

private:
  struct LevelData {
    int gridsPerChunk = 1;
    litestl::util::Vector<litestl::util::Vector<float>> chunks;
    // X5: when non-empty, the level's chunks live here lz4-compressed and
    // `chunks` is empty; rawFloats is the concatenated float count.
    litestl::util::Vector<uint8_t> evicted;
    size_t rawFloats = 0;
  };

  struct Channel {
    litestl::util::string name;
    int floatsPerElem = 1;
    litestl::util::Vector<LevelData> levels; // [0] = level 1
  };

  void allocLevel(Channel &ch, int level);
  void rehydrate(Channel &ch, LevelData &ld, int level);

  int gridCount_ = 0;
  int levelCount_ = 0;
  litestl::util::Vector<GridLink> links_; // gridCount*4, [g*4 + side]
  litestl::util::Vector<Channel> channels_;
};

} // namespace sculptcore::subdiv
