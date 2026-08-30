#pragma once

/** Multires grids store (displacementAndSubSurf plan, S2). One grid per cage
 * face corner — the per-quadrant-after-one-split / Ptex `__faceindex`
 * convention, matching the Refiner's grid enumeration (cage face id order,
 * loop order within a face). Level L holds (2^(L-1)+1)^2 verts per grid.
 *
 * Channels are per-level flat float arrays over a grid's elements — the
 * (S+1)^2 vert lattice or the S^2 quad cells, per the channel's GridElemDomain.
 * Channel 0 is the always-present "disp" float3 — the level's displacement
 * relative to the smoothed previous level, expressed in the level's tangent
 * frame (frames are computed elsewhere; the store is frame-agnostic). Storage
 * is chunked by whole grids with an offset-table-headed serialized form, so a
 * later disk-backing pass can page chunks without a format change (X5).
 *
 * Storage is `float` regardless of a channel's declared AttrType, so the rule
 * is **no float math on a typed channel** — see interpolatableType.
 *
 * A channel carries two independent flags, and conflating them is a bug the
 * store has already been through once:
 *
 *   GridLevelRule  how the values behave across a level transition. `disp` is
 *       a per-level Delta; everything else is Authored — a value the surface
 *       itself carries at every level, prolonged onto a new finest level and
 *       restricted back down when one is dropped.
 *   persist        whether the HOST has a container for this channel and will
 *       save and restore it. Pure host contract: the engine's own behaviour
 *       (lazy allocation, seeding, the draw overlay, the cage write-back) keys
 *       off the level rule, so a host that gains a container for an authored
 *       layer flips one bit and nothing else moves. Blender has exactly one
 *       such container (the scalar paint mask), which is why every layer the
 *       brushes paint there is a non-persistent Authored channel.
 *
 * Topology is implicit: within a grid, neighbors are ±1 u/v strides; grid
 * boundaries carry 4 links to adjacent grids (same-face neighbors on the
 * right/top sides, across-cage-edge neighbors on the left/bottom sides), each
 * a transpose mapping (param t preserved, u/v roles swapped). Boundary verts
 * are REPLICATED in every grid that contains them; seamMates enumerates the
 * aliases of a boundary coord so writers can keep replicas in sync (S4). */

#include "io/compress.h"

#include "mesh/attribute_enums.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <iosfwd>
#include <span>

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::subdiv {

/** Bump when the on-disk layout changes (mirrors mesh serial versioning).
 * v2 split the single lz4 block into independently-compressed blocks.
 * v3 added per-channel domain/type/persist.
 * v4 added the per-channel GridLevelRule. */
inline constexpr uint32_t kGridsFormatVersion = 4;

/** Serialized payload is cut into blocks of this size so lz4 runs in parallel.
 * Small enough that a ~23 MB store spreads over every core, large enough that
 * the ratio loss against one whole-payload block stays under a percent. */
inline constexpr uint32_t kCompressBlock = 1u << 21;

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

/** Which grid element a channel carries one value per: Vertex is the (S+1)^2
 * vert lattice (coord (u,v), 0..S), Face the S^2 quad cells (coord (u,v),
 * 0..S-1, cell id v*S + u — global face id grid*S*S + v*S + u). */
enum class GridElemDomain : int {
  Vertex = 0,
  Face = 1,
};

/** How a level transition treats a channel's values.
 *
 * Delta — the channel is a per-level correction against the level below
 * (channel 0, "disp"). A fresh finest level means "no correction yet", i.e.
 * zero, and the values of a dropped level say nothing about the level below,
 * so both transitions are a blank. This is the multires displacement rule and
 * matches Blender's own Subdivide / Delete Higher.
 *
 * Authored — the channel carries the surface's own value at every level
 * (colour, face sets, any painted layer). A fresh level is prolonged from the
 * level below and a dropped level is restricted back onto it, so an
 * addLevel/dropTopLevel round trip is the IDENTITY: prolongation reproduces
 * the coarse value exactly at coincident lattice sites (see seedLevelFromBelow)
 * and restriction reads exactly those sites back (see restrictLevelToBelow).
 * Blanking here would silently delete paint. */
enum class GridLevelRule : int {
  Delta = 0,
  Authored = 1,
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

  /** Row stride of one grid's element lattice at `level`: S+1 verts, S cells. */
  static int elemWidth(int level, GridElemDomain domain)
  {
    const int S = sideForLevel(level);
    return domain == GridElemDomain::Face ? S : S + 1;
  }

  /** Elements per grid at `level`: Vertex (S+1)^2, Face S^2. The single home of
   * that choice — every allocation, eviction and undo block sizes through it. */
  static int elemsPerGrid(int level, GridElemDomain domain)
  {
    const int w = elemWidth(level, domain);
    return w * w;
  }

  /** Derive gridCount + the 4 per-grid links from a cage mesh, enumerating
   * grids exactly like Refiner::refine. Thaws frozen topology.
   *
   * **Unconditionally destructive**: every channel and every level goes,
   * leaving only a fresh zero "disp". Cage topology *defines* the store's
   * element set, so per-grid-element data does not survive a rebuild even
   * when the new topology happens to match — there is no correspondence to
   * carry it across, and a "sometimes preserved" rule would be worse than no
   * rule at all. That makes this the store's LOAD boundary, not a refresh:
   * a host restores by declaring its channels (addChannel) and writing their
   * levels back afterwards, which is exactly the shape of the channel c-api
   * (c-api/grid_channel_c_api.h). Engine-side, Multires::init is the only
   * caller and it re-adds every level immediately. */
  void buildFromCage(mesh::Mesh &cage);

  /** Append the next level (level == levelCount()+1 after the call). A Delta
   * channel gets zero-filled storage (no correction yet is the identity); an
   * Authored channel is prolonged from the level below, since nothing
   * re-derives it. */
  void addLevel();

  /** Drop the finest level (level == levelCount()) from every channel — the
   * inverse of addLevel(). An Authored channel is restricted onto the level
   * below on the way out, so its paint survives at the resolution the
   * surviving level can hold; a Delta channel's correction simply goes, as
   * multires displacement does. No-op when empty. */
  void dropTopLevel();

  /** Carry `channel`'s authored values at `level` down onto level-1, in place:
   * full weighting for an interpolatable Vertex channel (the transpose of
   * seedLevelFromBelow's prolongation -- the 9-point stencil, 1/4 centre, 1/8
   * edge, 1/16 corner), the mean of the four children for an interpolatable
   * Face channel, and injection (restrictLevelToBelow's rule) for a typed one.
   *
   * Unlike dropTopLevel's injection this is a *filter*, which is what a coarse
   * level wants after a fine edit: the detail arrives as an average instead of
   * whichever sample happened to sit on the coarse site. Weights are
   * accumulated rather than assumed, so a stencil running off the grid still
   * normalizes to 1 -- a constant field restricts to itself everywhere,
   * including seams and mesh boundaries.
   *
   * Seams: a coord shared by several grids is stored once per grid and each
   * replica can only reach the taps inside its own grid, so the partial sums
   * are merged across seamMates() before normalizing and every replica is
   * written the same value. The taps sitting *on* the seam are then counted
   * once per incident grid, which tilts the stencil towards it (centre 1/3
   * rather than 1/4 on a two-grid seam). The result is still a normalized
   * average of the neighbourhood, so constants and the C0 seam invariant both
   * hold; the deviation is a slightly narrower filter along seams.
   *
   * False (and nothing written) when `level` holds nothing authored. */
  bool restrictChannelDown(int channel, int level);

  /** Carry an edit at `level` up into every finer level, in place. `coords`
   * is the touched lattice slots as (grid, u, v) triples — every seam replica,
   * the shape occurrences() hands back — and `deltas` the per-slot change
   * (new - old, floatsPerElem floats per slot). Each finer level receives the
   * 4-tap prolongation of the *delta* added onto what it already holds, so
   * authored fine detail rides a coarse edit instead of being overwritten;
   * a finer level nothing ever authored is instead seeded whole from the
   * (post-edit) level below, which is the same answer its implicit zeros
   * want. The down half of the edit contract stays restrictChannelDown debt.
   * Interpolatable Vertex channels only; anything else is a no-op. */
  void prolongateChannelEditUp(int channel,
                               int level,
                               std::span<const int> coords,
                               std::span<const float> deltas);

  /** Whether `channel` owes level-1 the values it holds at `level` (see
   * LevelData::downPending). Out-of-range reads false / is ignored. */
  bool channelLevelDebt(int level, int channel) const
  {
    if (level < 1 || level > levelCount_ || channel < 0 ||
        channel >= int(channels_.size()))
    {
      return false;
    }
    return channels_[channel].levels[level - 1].downPending;
  }
  void setChannelLevelDebt(int level, int channel, bool value)
  {
    if (level < 1 || level > levelCount_ || channel < 0 ||
        channel >= int(channels_.size()))
    {
      return;
    }
    channels_[channel].levels[level - 1].downPending = value;
  }
  /** Whether any channel owes level-1 at `level` — the cheap "is there work?"
   * test a level switch runs before walking the channels. */
  bool anyChannelLevelDebt(int level) const
  {
    for (int c = 0; c < int(channels_.size()); c++) {
      if (channelLevelDebt(level, c)) {
        return true;
      }
    }
    return false;
  }

  int levelCount() const
  {
    return levelCount_;
  }

  int gridCount() const
  {
    return gridCount_;
  }

  /** Add a named channel of 1..4 floats per element of `domain`. Returns the
   * channel index. Channel 0 is always "disp" (3 vertex floats).
   *
   * Storage is `float` throughout and `type` enforces nothing — it is metadata
   * for the host bridge and for the one decision the store itself makes
   * (whether seeding a new level may interpolate). The invariant is
   * **no float math on a typed channel**; interpolating consumers assert it.
   *
   * `rule` says what a level transition does to the values (see GridLevelRule)
   * and is the flag the engine reads: an Authored channel is allocated lazily
   * per level, so an untouched one costs nothing and channelLevelAllocated
   * means exactly "this level holds something authored".
   *
   * `persist` is the HOST contract — "I have a container for this and will
   * save and restore it" — and the engine never branches on it. Hosts read it
   * back when enumerating what they own. */
  int addChannel(const litestl::util::string &name,
                 int floatsPerElem,
                 GridElemDomain domain = GridElemDomain::Vertex,
                 mesh::AttrType type = mesh::AttrType::FLOAT,
                 bool persist = true,
                 GridLevelRule rule = GridLevelRule::Authored);

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

  /** Whether values of `type` may be averaged. The guard behind the storage
   * invariant: channel data is `float` throughout and `type` enforces nothing,
   * so there must be **no float math on a typed channel** — interpolating
   * consumers assert through here instead of trusting the column. */
  static bool interpolatableType(mesh::AttrType type)
  {
    return type == mesh::AttrType::FLOAT || type == mesh::AttrType::FLOAT2 ||
           type == mesh::AttrType::FLOAT3 || type == mesh::AttrType::FLOAT4;
  }

  bool channelInterpolatable(int channel) const
  {
    return interpolatableType(channels_[channel].type);
  }

  GridElemDomain channelDomain(int channel) const
  {
    return channels_[channel].domain;
  }

  mesh::AttrType channelType(int channel) const
  {
    return channels_[channel].type;
  }

  bool channelPersist(int channel) const
  {
    return channels_[channel].persist;
  }

  /** Claim (or disclaim) a channel for the host's own save/load. Nothing in
   * the engine reads this; it exists so a host that gains a container for a
   * layer mid-session can record the fact where the serializer will carry it. */
  void setChannelPersist(int channel, bool persist)
  {
    channels_[channel].persist = persist;
  }

  GridLevelRule channelLevelRule(int channel) const
  {
    return channels_[channel].rule;
  }

  /** Whether `channel` carries values the surface itself holds, rather than a
   * per-level correction. The discriminator every consumer of authored paint
   * asks — the draw overlay, the cage write-back, the undo return route — and
   * deliberately NOT `!channelPersist`, which asks a host-contract question. */
  bool channelAuthored(int channel) const
  {
    return channels_[channel].rule == GridLevelRule::Authored;
  }

  /** Whether `channel` has live storage for `level`. False only for an
   * Authored channel no one has touched at that level yet (a Delta channel is
   * allocated up front, and an evicted level rehydrates on the next elem()) --
   * i.e. exactly "this level holds nothing authored".  */
  bool channelLevelAllocated(int level, int channel) const
  {
    if (level < 1 || level > levelCount_ || channel < 0 ||
        channel >= int(channels_.size()))
    {
      return false;
    }
    const LevelData &ld = channels_[channel].levels[level - 1];
    return ld.chunks.size() > 0 || ld.evicted.size() > 0;
  }

  /** Elements per grid of `channel` at `level` — elemsPerGrid on its domain.
   * Multiply by channelElemSize for the float count of one grid's block. */
  int channelElemsPerGrid(int level, int channel) const
  {
    return elemsPerGrid(level, channels_[channel].domain);
  }

  /** Pointer to the floatsPerElem floats of one grid element. O(1). Rehydrates
   * an evicted level and allocates a lazy session channel on the way. */
  float *elem(int level, int channel, int grid, int u, int v);
  const float *elem(int level, int channel, int grid, int u, int v) const;

  /** One lattice step (|du| + |dv| == 1) from `c` at `level`, crossing into
   * the adjacent grid at seams. False when the step exits the cage (mesh
   * boundary) — there is no vert there. The result is always lattice- (and
   * hence mesh-edge-) adjacent to `c`. */
  bool neighbor(int level, const GridCoord &c, int du, int dv, GridCoord &out) const;

  /** All OTHER (grid,u,v) coords aliasing the same surface vert as `c` — the
   * replicas a seam write must sync. Empty for grid-interior coords. */
  void
  seamMates(int level, const GridCoord &c, litestl::util::Vector<GridCoord> &out) const;

  const GridLink &link(int grid, int side) const
  {
    return links_[grid * 4 + side];
  }

  /** Serialize into @p out (appending to whatever it already holds) as a
   * BinFile header followed by kCompressBlock-sized lz4 blocks; the compressed
   * payload is an offset-table header followed by the raw chunks.
   * @p hcLevel picks the lz4 mode: callers on an interactive path pass
   * io::kFastCompressLevel, which the reader handles identically. */
  bool writeBytes(litestl::util::Vector<uint8_t> &out,
                  int hcLevel = io::kDefaultCompressLevel);
  /** writeBytes to a stream. */
  bool write(std::ostream &out, int hcLevel = io::kDefaultCompressLevel);
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
    /** This (channel, level) carries authored values the level below has not
     * been given -- the attribute half of Multires::downPropPending_, kept
     * per channel because restricting a channel that was NOT edited would
     * smooth away whatever the user painted on the coarse level, and per
     * LevelData so it follows addLevel/dropTopLevel without bookkeeping. */
    bool downPending = false;
    litestl::util::Vector<litestl::util::Vector<float>> chunks;
    // X5: when non-empty, the level's chunks live here lz4-compressed and
    // `chunks` is empty; rawFloats is the concatenated float count.
    litestl::util::Vector<uint8_t> evicted;
    size_t rawFloats = 0;
  };

  struct Channel {
    litestl::util::string name;
    int floatsPerElem = 1;
    GridElemDomain domain = GridElemDomain::Vertex;
    mesh::AttrType type = mesh::AttrType::FLOAT;
    bool persist = true;
    GridLevelRule rule = GridLevelRule::Authored;
    litestl::util::Vector<LevelData> levels; // [0] = level 1
  };

  /** Append a LevelData for `level`, with chunks unless the channel is a
   * lazily-allocated Authored one. */
  void allocLevel(Channel &ch, int level);
  /** Zeroed chunk vectors for one (channel, level), sized off ld.gridsPerChunk.
   * The one sizing path — allocation, lazy first touch and rehydration all run
   * through it, so an eviction round trip cannot disagree with allocation. */
  void fillChunks(const Channel &ch, LevelData &ld, int level);
  void rehydrate(Channel &ch, LevelData &ld, int level);
  /** elem() against a channel the store holds by reference rather than index. */
  float *elemIn(Channel &ch, int level, int grid, int u, int v);
  /** Compress + free one (channel, level)'s chunks. No-op when already evicted
   * or not allocated. */
  void evictChannelLevel(LevelData &ld);
  /** Prolongation: copy `level`'s values down from level-1 (which the caller
   * must have just appended above), interpolating float channels and snapping
   * typed ones. Exact at coincident lattice sites — an even fine coord reads
   * its coarse sample four times, so the 4-tap collapses to a copy. */
  void seedLevelFromBelow(Channel &ch, int level);
  /** Restriction: write `level`'s values onto level-1, by injection — coarse
   * (u, v) takes fine (2u, 2v), the sample seedLevelFromBelow copied it into.
   *
   * Injection, not a weighted average, for three reasons that all point the
   * same way: it is the exact left inverse of the prolongation above (so an
   * addLevel/dropTopLevel round trip is bit-identical), it does no float math
   * and so is legal on a typed channel (an INT face-set column would be
   * destroyed by averaging), and it cannot blur across a grid seam — grid id
   * is the cage corner, so every face-set boundary lies on one. The cost is
   * that sub-coarse-sample detail is dropped, which is what dropping a level
   * means. No-op when `level` holds nothing. */
  void restrictLevelToBelow(Channel &ch, int level);

  int gridCount_ = 0;
  int levelCount_ = 0;
  litestl::util::Vector<GridLink> links_; // gridCount*4, [g*4 + side]
  litestl::util::Vector<Channel> channels_;
};

} // namespace sculptcore::subdiv
