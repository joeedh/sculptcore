#include "grids.h"

#include "mesh/mesh.h"

#include "io/binfile.h"
#include "io/compress.h"

#include "litestl/util/assert.h"
#include "litestl/util/vector.h"

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

void GridsStore::allocLevel(Channel &ch, int level)
{
  int w = sideForLevel(level) + 1;
  int gridFloats = w * w * ch.floatsPerElem;
  LevelData ld;
  ld.gridsPerChunk = gridFloats > 0 ? kTargetChunkFloats / gridFloats : 1;
  if (ld.gridsPerChunk < 1) {
    ld.gridsPerChunk = 1;
  }
  for (int g0 = 0; g0 < gridCount_; g0 += ld.gridsPerChunk) {
    int grids = gridCount_ - g0 < ld.gridsPerChunk ? gridCount_ - g0 : ld.gridsPerChunk;
    Vector<float> chunk;
    chunk.resize(size_t(grids) * gridFloats); // zero-filled
    ld.chunks.append(std::move(chunk));
  }
  ch.levels.append(std::move(ld));
}

int GridsStore::addChannel(const string &name, int floatsPerElem)
{
  Assert(floatsPerElem >= 1 && floatsPerElem <= 4, "grid channel elem size");
  Channel ch;
  ch.name = name;
  ch.floatsPerElem = floatsPerElem;
  for (int l = 1; l <= levelCount_; l++) {
    allocLevel(ch, l);
  }
  channels_.append(std::move(ch));
  return int(channels_.size()) - 1;
}

void GridsStore::addLevel()
{
  levelCount_++;
  for (Channel &ch : channels_) {
    allocLevel(ch, levelCount_);
  }
}

float *GridsStore::elem(int level, int channel, int grid, int u, int v)
{
  Channel &ch = channels_[channel];
  LevelData &ld = ch.levels[level - 1];
  if (ld.evicted.size()) {
    rehydrate(ch, ld, level); // X5: transparent rehydration on first touch
  }
  int w = sideForLevel(level) + 1;
  int local = grid % ld.gridsPerChunk;
  size_t idx = (size_t(local) * w * w + size_t(v) * w + u) * ch.floatsPerElem;
  return &ld.chunks[grid / ld.gridsPerChunk][int(idx)];
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
  addChannel(string("disp"), 3);

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

void GridsStore::evictLevel(int level)
{
  if (level < 1 || level > levelCount_) {
    return;
  }
  for (Channel &ch : channels_) {
    LevelData &ld = ch.levels[level - 1];
    if (ld.evicted.size() || !ld.chunks.size()) {
      continue;
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
    size_t compSize =
        io::compressBlock(raw.data(), total * sizeof(float), comp);
    if (compSize == 0) {
      continue; // compression failed: stay resident (never lose data)
    }
    ld.evicted = std::move(comp);
    ld.rawFloats = total;
    ld.chunks = Vector<Vector<float>>();
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
  // Chunk geometry is deterministic — mirror allocLevel's sizing.
  int w = sideForLevel(level) + 1;
  int gridFloats = w * w * ch.floatsPerElem;
  const float *src = reinterpret_cast<const float *>(raw.data());
  size_t off = 0;
  for (int g0 = 0; g0 < gridCount_; g0 += ld.gridsPerChunk) {
    int grids = gridCount_ - g0 < ld.gridsPerChunk ? gridCount_ - g0 : ld.gridsPerChunk;
    Vector<float> chunk;
    chunk.resize(size_t(grids) * gridFloats);
    std::memcpy(chunk.data(), src + off, chunk.size() * sizeof(float));
    off += chunk.size();
    ld.chunks.append(std::move(chunk));
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
 *   u32 channelCount; per channel: string name; u32 floatsPerElem
 *   offset table, per (channel, level): u32 gridsPerChunk; u32 chunkCount;
 *     per chunk: u32 byteOffset (into the data section); u32 floatCount
 *   data section: chunk float payloads in (channel, level, chunk) order */
bool GridsStore::write(std::ostream &out)
{
  // The serializer walks raw chunks — rehydrate everything first.
  for (int l = 1; l <= levelCount_; l++) {
    ensureLevelResident(l);
  }
  std::stringstream ps(std::ios::in | std::ios::out | std::ios::binary);
  io::BinFile pbf(ps);

  pbf.writeUint32(uint32_t(gridCount_));
  pbf.writeUint32(uint32_t(levelCount_));
  for (const GridLink &l : links_) {
    pbf.writeInt32(l.grid);
    pbf.writeInt32(l.side);
  }
  pbf.writeUint32(uint32_t(channels_.size()));
  for (Channel &ch : channels_) {
    pbf.writeString(ch.name);
    pbf.writeUint32(uint32_t(ch.floatsPerElem));
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
  for (Channel &ch : channels_) {
    for (LevelData &ld : ch.levels) {
      for (Vector<float> &chunk : ld.chunks) {
        pbf.stream.write(reinterpret_cast<const char *>(chunk.data()),
                         std::streamsize(chunk.size() * sizeof(float)));
      }
    }
  }

  std::string payload = ps.str();
  size_t rawSize = payload.size();

  Vector<uint8_t> comp;
  size_t compSize = io::compressBlock(payload.data(), rawSize, comp);
  if (compSize == 0) {
    return false;
  }

  std::stringstream fs(std::ios::in | std::ios::out | std::ios::binary);
  io::BinFile obf(fs);
  obf.compressed = true;
  obf.writeHeader();
  obf.writeUint32(kGridsFormatVersion);
  obf.writeUint32(uint32_t(rawSize));
  obf.writeUint32(uint32_t(compSize));
  obf.stream.write(reinterpret_cast<const char *>(comp.data()),
                   std::streamsize(compSize));

  std::string s = fs.str();
  out.write(s.data(), std::streamsize(s.size()));
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
  uint32_t rawSize = obf.readUint32();
  uint32_t compSize = obf.readUint32();
  if (version == 0 || version > kGridsFormatVersion) {
    return false;
  }

  Vector<uint8_t> comp;
  comp.resize(compSize);
  if (compSize > 0) {
    obf.stream.read(reinterpret_cast<char *>(comp.data()), std::streamsize(compSize));
  }
  Vector<uint8_t> rawBuf;
  if (!io::decompressBlock(comp.data(), compSize, rawSize, rawBuf)) {
    return false;
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
