#include "grids.h"

#include "mesh/mesh.h"

#include "io/binfile.h"
#include "io/compress.h"

#include "litestl/util/assert.h"
#include "litestl/util/task.h"
#include "litestl/util/vector.h"

#include <algorithm>
#include <cstring>
#include <istream>
#include <iterator>
#include <ostream>
#include <sstream>
#include <string>

using namespace litestl;
using litestl::util::Assert;
using litestl::util::string;
using litestl::util::Vector;

namespace sculptcore::subdiv {

// Chunk sizing target: ~256 KiB of floats. Chunks hold whole grids, so a grid
// larger than the target gets a chunk to itself.
static constexpr int kTargetChunkFloats = 64 * 1024;

void GridsStore::fillChunks(const Channel &ch, LevelData &ld, int level)
{
  const int gridFloats = elemsPerGrid(level, ch.domain) * ch.floatsPerElem;
  for (int g0 = 0; g0 < gridCount_; g0 += ld.gridsPerChunk) {
    int grids = gridCount_ - g0 < ld.gridsPerChunk ? gridCount_ - g0 : ld.gridsPerChunk;
    Vector<float> chunk;
    chunk.resize(size_t(grids) * gridFloats); // zero-filled
    ld.chunks.append(std::move(chunk));
  }
}

void GridsStore::allocLevel(Channel &ch, int level)
{
  const int gridFloats = elemsPerGrid(level, ch.domain) * ch.floatsPerElem;
  LevelData ld;
  ld.gridsPerChunk = gridFloats > 0 ? kTargetChunkFloats / gridFloats : 1;
  if (ld.gridsPerChunk < 1) {
    ld.gridsPerChunk = 1;
  }
  // An Authored channel stays unallocated until elem() first touches it; the
  // geometry above is still fixed now, so lazy and eager chunks agree.
  if (ch.rule != GridLevelRule::Authored) {
    fillChunks(ch, ld, level);
  }
  ch.levels.append(std::move(ld));
}

int GridsStore::addChannel(const string &name,
                           int floatsPerElem,
                           GridElemDomain domain,
                           mesh::AttrType type,
                           bool persist,
                           GridLevelRule rule)
{
  Assert(floatsPerElem >= 1 && floatsPerElem <= 4, "grid channel elem size");
  Channel ch;
  ch.name = name;
  ch.floatsPerElem = floatsPerElem;
  ch.domain = domain;
  ch.type = type;
  ch.persist = persist;
  ch.rule = rule;
  for (int l = 1; l <= levelCount_; l++) {
    // Eviction state is per (channel, level): joining an evicted level resident
    // would leave the level half-paged-in and never swept by evictLevel.
    const bool evicted = !levelResident(l);
    allocLevel(ch, l);
    if (evicted) {
      evictChannelLevel(ch.levels.last());
    }
  }
  channels_.append(std::move(ch));
  return int(channels_.size()) - 1;
}

void GridsStore::addLevel()
{
  levelCount_++;
  for (Channel &ch : channels_) {
    allocLevel(ch, levelCount_);
    if (ch.rule == GridLevelRule::Authored) {
      seedLevelFromBelow(ch, levelCount_);
    }
  }
}

void GridsStore::dropTopLevel()
{
  if (levelCount_ < 1) {
    return;
  }
  // pop_back destructs the tail LevelData (and its nested Vectors) cleanly;
  // the remove_at double-free noted in removeChannel is mid-vector-only.
  for (Channel &ch : channels_) {
    if (ch.rule == GridLevelRule::Authored) {
      restrictLevelToBelow(ch, levelCount_);
    }
    // The level below now holds what the dropped one held, so it inherits its
    // debt: injection is the exact left inverse of the prolongation, so a
    // level with nothing owing lands back on what it seeded and owes nothing.
    if (levelCount_ >= 2 && ch.levels.last().downPending) {
      ch.levels[levelCount_ - 2].downPending = true;
    }
    ch.levels.pop_back();
  }
  levelCount_--;
}

void GridsStore::seedLevelFromBelow(Channel &ch, int level)
{
  if (level < 2) {
    return;
  }
  const LevelData &below = ch.levels[level - 2];
  if (!below.chunks.size() && !below.evicted.size()) {
    return; // never touched: the zero-fill above already matches it
  }
  const int fpe = ch.floatsPerElem;
  const int w = elemWidth(level, ch.domain);
  const bool lerp = ch.domain == GridElemDomain::Vertex && interpolatableType(ch.type);
  for (int g = 0; g < gridCount_; g++) {
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        float *dst = elemIn(ch, level, g, u, v);
        const int cu = u >> 1, cv = v >> 1;
        if (!lerp) {
          // A face cell splits into four copies of itself; a typed vert channel
          // snaps to the coarse sample it sits on or nearest to.
          const float *src = elemIn(ch, level - 1, g, cu, cv);
          for (int k = 0; k < fpe; k++) {
            dst[k] = src[k];
          }
          continue;
        }
        // One 4-tap covers copy/edge/centre alike: an even fine coord repeats
        // its coarse sample into both taps, so duplicates collapse.
        const int du = u & 1, dv = v & 1;
        const float *s00 = elemIn(ch, level - 1, g, cu, cv);
        const float *s10 = elemIn(ch, level - 1, g, cu + du, cv);
        const float *s01 = elemIn(ch, level - 1, g, cu, cv + dv);
        const float *s11 = elemIn(ch, level - 1, g, cu + du, cv + dv);
        for (int k = 0; k < fpe; k++) {
          dst[k] = 0.25f * (s00[k] + s10[k] + s01[k] + s11[k]);
        }
      }
    }
  }
}

void GridsStore::restrictLevelToBelow(Channel &ch, int level)
{
  if (level < 2) {
    return;
  }
  LevelData &fine = ch.levels[level - 1];
  if (!fine.chunks.size() && !fine.evicted.size()) {
    return; // never touched: the level below already holds what it seeded
  }
  const int fpe = ch.floatsPerElem;
  const int w = elemWidth(level - 1, ch.domain);
  for (int g = 0; g < gridCount_; g++) {
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        // Injection. Both domains index the same way: a coarse vertex sits on
        // fine (2u, 2v), and a coarse cell's lower-left child is (2u, 2v).
        const float *src = elemIn(ch, level, g, u * 2, v * 2);
        float *dst = elemIn(ch, level - 1, g, u, v);
        for (int k = 0; k < fpe; k++) {
          dst[k] = src[k];
        }
      }
    }
  }
}

bool GridsStore::restrictChannelDown(int channel, int level)
{
  if (channel < 0 || channel >= int(channels_.size()) || level < 2 || level > levelCount_) {
    return false;
  }
  if (!channelLevelAllocated(level, channel)) {
    return false; // nothing authored up there to carry down
  }
  Channel &ch = channels_[channel];
  if (!interpolatableType(ch.type)) {
    // No float math on a typed channel, so injection is the only legal move --
    // and it is the one dropTopLevel already uses.
    restrictLevelToBelow(ch, level);
    return true;
  }

  const int fpe = ch.floatsPerElem;
  const int w = elemWidth(level - 1, ch.domain);
  const int wf = elemWidth(level, ch.domain);

  // Materialize both levels before taking any pointer into them: elemIn
  // allocates (or rehydrates) on first touch, and the loops below hold a coarse
  // pointer across fine reads. Nothing resizes a level's chunks afterwards.
  elemIn(ch, level, 0, 0, 0);
  elemIn(ch, level - 1, 0, 0, 0);

  if (ch.domain == GridElemDomain::Face) {
    // Cells are never shared between grids, so a coarse cell is exactly the
    // mean of its four children and there is no seam pass to run.
    for (int g = 0; g < gridCount_; g++) {
      for (int v = 0; v < w; v++) {
        for (int u = 0; u < w; u++) {
          float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
          for (int dv = 0; dv < 2; dv++) {
            for (int du = 0; du < 2; du++) {
              const float *src = elemIn(ch, level, g, u * 2 + du, v * 2 + dv);
              for (int k = 0; k < fpe; k++) {
                acc[k] += src[k];
              }
            }
          }
          float *dst = elemIn(ch, level - 1, g, u, v);
          for (int k = 0; k < fpe; k++) {
            dst[k] = acc[k] * 0.25f;
          }
        }
      }
    }
    return true;
  }

  // Vertex domain: full weighting, accumulated UNNORMALIZED into the coarse
  // level first. The seam merge below reads those partial sums straight out of
  // the store, so the weight totals travel alongside in `wsum` and nothing is
  // divided until every replica has been added up.
  Vector<float> wsum;
  wsum.resize(size_t(gridCount_) * w * w);

  for (int g = 0; g < gridCount_; g++) {
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float wt = 0.0f;
        for (int dv = -1; dv <= 1; dv++) {
          const int fv = v * 2 + dv;
          if (fv < 0 || fv >= wf) {
            continue; // off this grid: a seam mate's own gather covers it
          }
          for (int du = -1; du <= 1; du++) {
            const int fu = u * 2 + du;
            if (fu < 0 || fu >= wf) {
              continue;
            }
            const float t = (du ? 0.5f : 1.0f) * (dv ? 0.5f : 1.0f);
            const float *src = elemIn(ch, level, g, fu, fv);
            for (int k = 0; k < fpe; k++) {
              acc[k] += t * src[k];
            }
            wt += t;
          }
        }
        float *dst = elemIn(ch, level - 1, g, u, v);
        for (int k = 0; k < fpe; k++) {
          dst[k] = acc[k];
        }
        wsum[(size_t(g) * w + v) * w + u] = wt;
      }
    }
  }

  // Merge the partial sums across seams, in the same (grid, v, u) order the
  // normalize pass walks, so a running slot index pairs them up without a
  // per-coord side table. Every replica of a coord sees the same mate set and
  // therefore lands on the same value -- the C0 seam invariant is restored by
  // construction, not by a fixup afterwards.
  // Deliberate deviation: an on-seam tap is counted once per incident grid, so
  // a two-grid seam's centre weight normalizes to 1/3 rather than 1/4. Mildly
  // seam-biased; constants are still fixed points, so it cannot overshoot.
  Vector<float> bacc, bwt;
  Vector<GridCoord> mates;
  for (int g = 0; g < gridCount_; g++) {
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        if (u != 0 && v != 0 && u != w - 1 && v != w - 1) {
          continue; // grid-interior: nothing aliases it
        }
        float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float *own = elemIn(ch, level - 1, g, u, v);
        for (int k = 0; k < fpe; k++) {
          acc[k] = own[k];
        }
        float wt = wsum[(size_t(g) * w + v) * w + u];
        seamMates(level - 1, GridCoord{g, u, v}, mates);
        for (const GridCoord &m : mates) {
          const float *src = elemIn(ch, level - 1, m.grid, m.u, m.v);
          for (int k = 0; k < fpe; k++) {
            acc[k] += src[k];
          }
          wt += wsum[(size_t(m.grid) * w + m.v) * w + m.u];
        }
        for (int k = 0; k < 4; k++) {
          bacc.append(acc[k]);
        }
        bwt.append(wt);
      }
    }
  }

  int slot = 0;
  for (int g = 0; g < gridCount_; g++) {
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        const bool border = u == 0 || v == 0 || u == w - 1 || v == w - 1;
        float *dst = elemIn(ch, level - 1, g, u, v);
        // The centre tap is always in range, so no weight total is zero.
        const float inv = border ? 1.0f / bwt[slot] :
                                   1.0f / wsum[(size_t(g) * w + v) * w + u];
        for (int k = 0; k < fpe; k++) {
          dst[k] = (border ? bacc[slot * 4 + k] : dst[k]) * inv;
        }
        slot += border ? 1 : 0;
      }
    }
  }
  return true;
}

float *GridsStore::elemIn(Channel &ch, int level, int grid, int u, int v)
{
  LevelData &ld = ch.levels[level - 1];
  if (ld.evicted.size()) {
    rehydrate(ch, ld, level); // X5: transparent rehydration on first touch
  }
  else if (!ld.chunks.size()) {
    fillChunks(ch, ld, level); // lazy session channel: allocate on first touch
  }
  const int w = elemWidth(level, ch.domain);
  const int local = grid % ld.gridsPerChunk;
  const size_t idx = (size_t(local) * w * w + size_t(v) * w + u) * ch.floatsPerElem;
  return &ld.chunks[grid / ld.gridsPerChunk][int(idx)];
}

float *GridsStore::elem(int level, int channel, int grid, int u, int v)
{
  return elemIn(channels_[channel], level, grid, u, v);
}

const float *GridsStore::elem(int level, int channel, int grid, int u, int v) const
{
  return const_cast<GridsStore *>(this)->elem(level, channel, grid, u, v);
}

/** The other radial corner of c1's edge, or -1 when the edge doesn't have
 * exactly two incident faces (boundary / non-manifold). */
static int radialMate(mesh::Mesh &m, int c1)
{
  int e1 = m.c.e[c1];
  int c0 = m.e.c[e1];
  if (c0 == ELEM_NONE) {
    return -1;
  }
  int n = 0, other = -1, cc = c0;
  do {
    n++;
    if (cc != c1) {
      other = cc;
    }
    cc = m.c.radial_next[cc];
  } while (cc != c0);
  return n == 2 ? other : -1;
}

void GridsStore::buildFromCage(mesh::Mesh &cage)
{
  cage.thawTopo();

  channels_.clear();
  levelCount_ = 0;
  addChannel(string("disp"),
             3,
             GridElemDomain::Vertex,
             mesh::AttrType::FLOAT,
             /*persist=*/true,
             GridLevelRule::Delta);

  Vector<int> gridOf;
  gridOf.resize(cage.c.capacity());
  for (int i = 0; i < int(cage.c.capacity()); i++) {
    gridOf[i] = -1;
  }

  gridCount_ = 0;
  for (int fi : cage.f) {
    int c0 = cage.l.c[cage.f.l[fi]], cc = c0;
    do {
      gridOf[cc] = gridCount_++;
      cc = cage.c.next[cc];
    } while (cc != c0);
  }

  links_.clear();
  links_.resize(size_t(gridCount_) * 4); // default {-1,-1}

  for (int fi : cage.f) {
    int c0 = cage.l.c[cage.f.l[fi]], cc = c0;
    do {
      int g = gridOf[cc];
      links_[g * 4 + GRID_SIDE_RIGHT] = {gridOf[cage.c.next[cc]], GRID_SIDE_TOP};
      links_[g * 4 + GRID_SIDE_TOP] = {gridOf[cage.c.prev[cc]], GRID_SIDE_RIGHT};

      // BOTTOM crosses cc's edge; the neighbor grid is the one at OUR corner
      // vert on the other face — the mate corner's successor under consistent
      // (opposite-traversal) winding. Inconsistent winding stays unlinked.
      int mate = radialMate(cage, cc);
      if (mate >= 0 && cage.c.v[mate] != cage.c.v[cc] &&
          cage.c.v[cage.c.next[mate]] == cage.c.v[cc])
      {
        links_[g * 4 + GRID_SIDE_BOTTOM] = {gridOf[cage.c.next[mate]], GRID_SIDE_LEFT};
      }

      // LEFT crosses the previous corner's edge; that mate corner sits at our
      // vert directly on the other face.
      int matep = radialMate(cage, cage.c.prev[cc]);
      if (matep >= 0 && cage.c.v[matep] == cage.c.v[cc]) {
        links_[g * 4 + GRID_SIDE_LEFT] = {gridOf[matep], GRID_SIDE_BOTTOM};
      }

      cc = cage.c.next[cc];
    } while (cc != c0);
  }
}

/** The seam coordinate on `side` of a grid with param t (transpose mapping:
 * every link preserves t). */
static GridCoord sideCoord(int grid, int side, int t, int S)
{
  switch (side) {
  case GRID_SIDE_LEFT:
    return {grid, 0, t};
  case GRID_SIDE_RIGHT:
    return {grid, S, t};
  case GRID_SIDE_BOTTOM:
    return {grid, t, 0};
  default:
    return {grid, t, S};
  }
}

bool GridsStore::neighbor(int level, const GridCoord &c, int du, int dv,
                          GridCoord &out) const
{
  Assert((du == 0) != (dv == 0), "neighbor() takes one lattice step");
  int S = sideForLevel(level);
  int u = c.u + du, v = c.v + dv;

  if (u >= 0 && u <= S && v >= 0 && v <= S) {
    out = {c.grid, u, v};
    return true;
  }

  int side, t;
  if (u < 0) {
    side = GRID_SIDE_LEFT;
    t = c.v;
  } else if (u > S) {
    side = GRID_SIDE_RIGHT;
    t = c.v;
  } else if (v < 0) {
    side = GRID_SIDE_BOTTOM;
    t = c.u;
  } else {
    side = GRID_SIDE_TOP;
    t = c.u;
  }

  const GridLink &l = links_[c.grid * 4 + side];
  if (l.grid < 0) {
    return false;
  }

  // Land on the alias of `c` on the neighbor's seam, then take the step's
  // remainder: one lattice unit inward, perpendicular to that seam.
  out = sideCoord(l.grid, l.side, t, S);
  switch (l.side) {
  case GRID_SIDE_LEFT:
    out.u = 1;
    break;
  case GRID_SIDE_RIGHT:
    out.u = S - 1;
    break;
  case GRID_SIDE_BOTTOM:
    out.v = 1;
    break;
  default:
    out.v = S - 1;
    break;
  }
  return true;
}

void GridsStore::seamMates(int level, const GridCoord &c, Vector<GridCoord> &out) const
{
  out.clear();
  int S = sideForLevel(level);

  Vector<GridCoord> seen, stack;
  seen.append(c);
  stack.append(c);

  while (stack.size() > 0) {
    GridCoord x = stack.pop_back();
    for (int side = 0; side < 4; side++) {
      bool onSide = side == GRID_SIDE_LEFT     ? x.u == 0
                    : side == GRID_SIDE_RIGHT  ? x.u == S
                    : side == GRID_SIDE_BOTTOM ? x.v == 0
                                               : x.v == S;
      if (!onSide) {
        continue;
      }
      const GridLink &l = links_[x.grid * 4 + side];
      if (l.grid < 0) {
        continue;
      }
      int t = (side == GRID_SIDE_LEFT || side == GRID_SIDE_RIGHT) ? x.v : x.u;
      GridCoord m = sideCoord(l.grid, l.side, t, S);
      if (!seen.contains(m)) {
        seen.append(m);
        stack.append(m);
      }
    }
  }

  for (int i = 1; i < int(seen.size()); i++) {
    out.append(seen[i]);
  }
}

void GridsStore::evictChannelLevel(LevelData &ld)
{
  if (ld.evicted.size() || !ld.chunks.size()) {
    return;
  }
  size_t total = 0;
  for (Vector<float> &c : ld.chunks) {
    total += c.size();
  }
  Vector<float> raw;
  raw.resize(total);
  size_t off = 0;
  for (Vector<float> &c : ld.chunks) {
    std::memcpy(raw.data() + off, c.data(), c.size() * sizeof(float));
    off += c.size();
  }
  Vector<uint8_t> comp;
  size_t compSize = io::compressBlock(raw.data(), total * sizeof(float), comp);
  if (compSize == 0) {
    return; // compression failed: stay resident (never lose data)
  }
  ld.evicted = std::move(comp);
  ld.rawFloats = total;
  ld.chunks = Vector<Vector<float>>();
}

void GridsStore::evictLevel(int level)
{
  if (level < 1 || level > levelCount_) {
    return;
  }
  for (Channel &ch : channels_) {
    evictChannelLevel(ch.levels[level - 1]);
  }
}

void GridsStore::rehydrate(Channel &ch, LevelData &ld, int level)
{
  Vector<uint8_t> raw;
  bool ok = io::decompressBlock(
      ld.evicted.data(), ld.evicted.size(), ld.rawFloats * sizeof(float), raw);
  Assert(ok, "grids eviction blob decompresses");
  if (!ok) {
    return;
  }
  // Chunk geometry is deterministic, and shared with allocation by construction
  // — a round trip cannot disagree about a grid's size.
  fillChunks(ch, ld, level);
  const float *src = reinterpret_cast<const float *>(raw.data());
  size_t off = 0;
  for (Vector<float> &chunk : ld.chunks) {
    std::memcpy(chunk.data(), src + off, chunk.size() * sizeof(float));
    off += chunk.size();
  }
  ld.evicted = Vector<uint8_t>();
  ld.rawFloats = 0;
}

void GridsStore::ensureLevelResident(int level)
{
  if (level < 1 || level > levelCount_) {
    return;
  }
  for (Channel &ch : channels_) {
    LevelData &ld = ch.levels[level - 1];
    if (ld.evicted.size()) {
      rehydrate(ch, ld, level);
    }
  }
}

bool GridsStore::levelResident(int level) const
{
  if (level < 1 || level > levelCount_) {
    return true;
  }
  for (const Channel &ch : channels_) {
    if (ch.levels[level - 1].evicted.size()) {
      return false;
    }
  }
  return true;
}

size_t GridsStore::residentBytes() const
{
  size_t n = 0;
  for (const Channel &ch : channels_) {
    for (const LevelData &ld : ch.levels) {
      for (const Vector<float> &c : ld.chunks) {
        n += c.size() * sizeof(float);
      }
    }
  }
  return n;
}

size_t GridsStore::evictedBytes() const
{
  size_t n = 0;
  for (const Channel &ch : channels_) {
    for (const LevelData &ld : ch.levels) {
      n += ld.evicted.size();
    }
  }
  return n;
}

/** Serialized payload (host-endian, inside the BinFile+lz4 container):
 *   u32 gridCount; u32 levelCount
 *   gridCount*4 x { i32 grid; i32 side }
 *   u32 channelCount; per channel: string name; u32 floatsPerElem;
 *     u32 domain; u32 type; u32 persist; u32 levelRule
 *   offset table, per (channel, level): u32 gridsPerChunk; u32 chunkCount;
 *     per chunk: u32 byteOffset (into the data section); u32 floatCount
 *   data section: chunk float payloads in (channel, level, chunk) order
 *
 * The payload is compressed in independent kCompressBlock-sized blocks so the
 * codec runs across cores: at a level-4 million-vert cage this store is ~23 MB
 * and it is re-serialized at the end of every sculpt stroke (the multires undo
 * snapshot), where a single-threaded lz4 pass alone cost ~50 ms.
 *
 * Authored channels are INCLUDED whether or not the host persists them: undo
 * is this serializer's only production consumer (the .blend is written from the
 * flush path, which never reaches the store), so excluding them would restore
 * an empty channel on every undo that falls back to the blob. A lazy channel
 * simply writes zero chunks and reads back lazy. */
bool GridsStore::writeBytes(Vector<uint8_t> &out, int hcLevel)
{
  // The serializer walks raw chunks — rehydrate everything first.
  for (int l = 1; l <= levelCount_; l++) {
    ensureLevelResident(l);
  }
  // Metadata only (a few hundred KB at most); the bulk float data is memcpy'd
  // in below rather than pushed through the stream a chunk at a time.
  std::stringstream ps(std::ios::in | std::ios::out | std::ios::binary);
  io::BinFile pbf(ps);

  pbf.writeUint32(uint32_t(gridCount_));
  pbf.writeUint32(uint32_t(levelCount_));
  // One blit, not 4 * gridCount stream calls: GridLink is exactly the {grid,
  // side} int pair the reader pulls back out one at a time.
  static_assert(sizeof(GridLink) == 2 * sizeof(uint32_t));
  pbf.writeUint32Array(reinterpret_cast<const uint32_t *>(links_.data()), links_.size() * 2);
  pbf.writeUint32(uint32_t(channels_.size()));
  for (Channel &ch : channels_) {
    pbf.writeString(ch.name);
    pbf.writeUint32(uint32_t(ch.floatsPerElem));
    pbf.writeUint32(uint32_t(ch.domain));
    pbf.writeUint32(uint32_t(ch.type));
    pbf.writeUint32(uint32_t(ch.persist));
    pbf.writeUint32(uint32_t(ch.rule));
  }

  uint32_t offset = 0;
  for (Channel &ch : channels_) {
    for (LevelData &ld : ch.levels) {
      pbf.writeUint32(uint32_t(ld.gridsPerChunk));
      pbf.writeUint32(uint32_t(ld.chunks.size()));
      for (Vector<float> &chunk : ld.chunks) {
        pbf.writeUint32(offset);
        pbf.writeUint32(uint32_t(chunk.size()));
        offset += uint32_t(chunk.size()) * sizeof(float);
      }
    }
  }

  const std::string meta = ps.str();
  Vector<uint8_t> payload;
  payload.resize<false>(meta.size() + size_t(offset));
  std::memcpy(payload.data(), meta.data(), meta.size());
  size_t at = meta.size();
  for (Channel &ch : channels_) {
    for (LevelData &ld : ch.levels) {
      for (Vector<float> &chunk : ld.chunks) {
        const size_t n = chunk.size() * sizeof(float);
        std::memcpy(payload.data() + at, chunk.data(), n);
        at += n;
      }
    }
  }

  const size_t rawSize = payload.size();
  const int blockCount = int((rawSize + kCompressBlock - 1) / kCompressBlock);
  Vector<Vector<uint8_t>> blocks;
  blocks.resize(blockCount);
  Vector<uint32_t> blockSizes;
  blockSizes.resize(blockCount);
  task::parallel_for(util::IndexRange(size_t(blockCount)), [&](util::IndexRange range) {
    for (int i : range) {
      const size_t start = size_t(i) * kCompressBlock;
      const size_t n = std::min(size_t(kCompressBlock), rawSize - start);
      blockSizes[i] = uint32_t(io::compressBlock(payload.data() + start, n, blocks[i], hcLevel));
    }
  });
  size_t compTotal = 0;
  for (int i = 0; i < blockCount; i++) {
    if (blockSizes[i] == 0) {
      return false;
    }
    compTotal += blockSizes[i];
  }

  std::stringstream fs(std::ios::in | std::ios::out | std::ios::binary);
  io::BinFile obf(fs);
  obf.compressed = true;
  obf.writeHeader();
  obf.writeUint32(kGridsFormatVersion);
  obf.writeUint64(uint64_t(rawSize));
  obf.writeUint32(uint32_t(kCompressBlock));
  obf.writeUint32(uint32_t(blockCount));
  for (int i = 0; i < blockCount; i++) {
    obf.writeUint32(blockSizes[i]);
  }
  const std::string head = fs.str();

  const size_t base = out.size();
  out.resize<false>(base + head.size() + compTotal);
  std::memcpy(out.data() + base, head.data(), head.size());
  at = base + head.size();
  for (int i = 0; i < blockCount; i++) {
    std::memcpy(out.data() + at, blocks[i].data(), blockSizes[i]);
    at += blockSizes[i];
  }
  return true;
}

bool GridsStore::write(std::ostream &out, int hcLevel)
{
  Vector<uint8_t> buf;
  if (!writeBytes(buf, hcLevel)) {
    return false;
  }
  out.write(reinterpret_cast<const char *>(buf.data()), std::streamsize(buf.size()));
  return bool(out);
}

bool GridsStore::read(std::istream &in)
{
  std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::stringstream fs(raw, std::ios::in | std::ios::out | std::ios::binary);

  io::BinFile obf(fs);
  if (!obf.readHeader() || !obf.compressed) {
    return false;
  }
  if (obf.littleEndian != io::hostLittleEndian) {
    return false; // foreign-endian blobs unsupported (mirrors mesh policy)
  }

  uint32_t version = obf.readUint32();
  if (version != kGridsFormatVersion) {
    return false;
  }
  const size_t rawSize = size_t(obf.readUint64());
  const size_t blockSize = size_t(obf.readUint32());
  const int blockCount = int(obf.readUint32());
  if (blockSize == 0 || size_t(blockCount) * blockSize < rawSize) {
    return false;
  }

  Vector<uint32_t> blockSizes;
  blockSizes.resize(blockCount);
  size_t compTotal = 0;
  for (int i = 0; i < blockCount; i++) {
    blockSizes[i] = obf.readUint32();
    compTotal += blockSizes[i];
  }
  Vector<uint8_t> comp;
  comp.resize<false>(compTotal);
  if (compTotal > 0) {
    obf.stream.read(reinterpret_cast<char *>(comp.data()), std::streamsize(compTotal));
  }
  if (!obf.stream) {
    return false;
  }

  Vector<size_t> compOffsets;
  compOffsets.resize(blockCount);
  size_t at = 0;
  for (int i = 0; i < blockCount; i++) {
    compOffsets[i] = at;
    at += blockSizes[i];
  }
  Vector<uint8_t> rawBuf;
  rawBuf.resize<false>(rawSize);
  // uint8 rather than BoolVector: the bands below write these concurrently and
  // a bit-packed vector would make neighbouring blocks share a word.
  Vector<uint8_t> ok;
  ok.resize(blockCount);
  task::parallel_for(util::IndexRange(size_t(blockCount)), [&](util::IndexRange range) {
    Vector<uint8_t> tmp;
    for (int i : range) {
      const size_t start = size_t(i) * blockSize;
      const size_t n = std::min(blockSize, rawSize - start);
      ok[i] = io::decompressBlock(comp.data() + compOffsets[i], blockSizes[i], n, tmp);
      if (ok[i]) {
        std::memcpy(rawBuf.data() + start, tmp.data(), n);
      }
    }
  });
  for (int i = 0; i < blockCount; i++) {
    if (!ok[i]) {
      return false;
    }
  }

  std::string payloadStr(reinterpret_cast<const char *>(rawBuf.data()), rawSize);
  std::stringstream ps(payloadStr, std::ios::in | std::ios::out | std::ios::binary);
  io::BinFile pbf(ps);

  gridCount_ = int(pbf.readUint32());
  levelCount_ = int(pbf.readUint32());
  links_.clear();
  links_.resize(size_t(gridCount_) * 4);
  for (int i = 0; i < gridCount_ * 4; i++) {
    links_[i].grid = pbf.readInt32();
    links_[i].side = pbf.readInt32();
  }

  channels_.clear();
  int nChannels = int(pbf.readUint32());
  for (int i = 0; i < nChannels; i++) {
    Channel ch;
    ch.name = pbf.readString();
    ch.floatsPerElem = int(pbf.readUint32());
    ch.domain = GridElemDomain(pbf.readUint32());
    ch.type = mesh::AttrType(pbf.readUint32());
    ch.persist = bool(pbf.readUint32());
    ch.rule = GridLevelRule(pbf.readUint32());
    channels_.append(std::move(ch));
  }

  for (Channel &ch : channels_) {
    for (int l = 1; l <= levelCount_; l++) {
      LevelData ld;
      ld.gridsPerChunk = int(pbf.readUint32());
      int nChunks = int(pbf.readUint32());
      for (int k = 0; k < nChunks; k++) {
        pbf.readUint32(); // byteOffset — implicit for the in-RAM reader
        Vector<float> chunk;
        chunk.resize(pbf.readUint32());
        ld.chunks.append(std::move(chunk));
      }
      ch.levels.append(std::move(ld));
    }
  }
  for (Channel &ch : channels_) {
    for (LevelData &ld : ch.levels) {
      for (Vector<float> &chunk : ld.chunks) {
        pbf.stream.read(reinterpret_cast<char *>(chunk.data()),
                        std::streamsize(chunk.size() * sizeof(float)));
      }
    }
  }
  return bool(pbf.stream);
}

} // namespace sculptcore::subdiv
