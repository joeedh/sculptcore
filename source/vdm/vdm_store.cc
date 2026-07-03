#include "vdm_store.h"

#include "mesh/attribute.h"
#include "mesh/mesh.h"
#include "mesh/mesh_proxy.h"

#include "io/binfile.h"
#include "io/compress.h"

#include "litestl/util/alloc.h"

#include <cmath>
#include <cstring>
#include <istream>
#include <iterator>
#include <ostream>
#include <sstream>

namespace sculptcore::vdm {

using litestl::alloc::Delete;
using litestl::alloc::New;
using litestl::math::float2;

/* v1 = atlas-only payload; v2 adds the backend tag + Ptex grid tables. */
constexpr uint32_t kVdmFormatVersion = 2;

VdmStore::~VdmStore()
{
  if (activeDelta_) {
    Delete(activeDelta_);
  }
  for (const auto &pair : tiles_) {
    if (pair.value) {
      Delete(pair.value);
    }
  }
}

VdmTile *VdmStore::findTile(int tx, int ty) const
{
  VdmTile *const *t = const_cast<util::Map<uint64_t, VdmTile *> &>(tiles_).lookup_ptr(
      tileKey(tx, ty));
  return t ? *t : nullptr;
}

void VdmStore::snapshotForDelta(uint64_t key, VdmTile *existing)
{
  if (!activeDelta_) {
    return;
  }
  if (existing) {
    if (existing->deltaGen == deltaGen_) {
      return;
    }
    existing->deltaGen = deltaGen_;
    VdmDelta::Entry entry;
    entry.key = key;
    entry.texels = existing->texels; // pre-state copy
    activeDelta_->entries.append(std::move(entry));
  } else {
    // Tile about to be created inside the bracket: pre-state = absent.
    VdmDelta::Entry entry;
    entry.key = key;
    activeDelta_->entries.append(std::move(entry));
  }
}

VdmTile &VdmStore::ensureTileAt(uint64_t key, int grid, int tx, int ty)
{
  VdmTile *const *slot = tiles_.lookup_ptr(key);
  VdmTile *t = slot ? *slot : nullptr;
  if (t) {
    snapshotForDelta(key, t);
    return *t;
  }
  snapshotForDelta(key, nullptr);
  t = New<VdmTile>("VdmTile");
  t->tx = tx;
  t->ty = ty;
  t->grid = grid;
  t->deltaGen = deltaGen_;
  int n = params.tile_size * params.tile_size;
  t->texels.resize(n);
  for (int i = 0; i < n; i++) {
    t->texels[i] = float3(0.0f, 0.0f, 0.0f);
  }
  tiles_.insert(key, static_cast<VdmTile *>(t));
  tileCount_++;
  gpuTopoDirty_ = true;
  return *t;
}

VdmTile &VdmStore::ensureTile(int tx, int ty)
{
  return ensureTileAt(tileKey(tx, ty), -1, tx, ty);
}

void VdmStore::removeTile(uint64_t key)
{
  VdmTile **slot = tiles_.lookup_ptr(key);
  if (!slot || !*slot) {
    return;
  }
  VdmTile *t = *slot;
  if (t->gpuSlot >= 0) {
    gpuSlots_[t->gpuSlot] = nullptr;
    gpuFreeSlots_.append(t->gpuSlot);
    gpuTopoDirty_ = true;
  }
  Delete(t);
  tiles_.remove(key);
  tileCount_--;
}

void VdmStore::markGpuDirty(VdmTile *t)
{
  if (t->gpuDirty) {
    return;
  }
  t->gpuDirty = true;
  if (t->gpuSlot >= 0) {
    gpuDirtySlots_.append(t->gpuSlot);
  }
  // Slotless tiles are queued at slot assignment (vdm_gpu.cc gpuLayout).
}

/* Floor-divide texel coord into (tile, local) so negative coords work too. */
static inline void splitCoord(int x, int tileSize, int &tile, int &local)
{
  tile = x >= 0 ? x / tileSize : -((-x - 1) / tileSize) - 1;
  local = x - tile * tileSize;
}

float3 VdmStore::texel(int x, int y) const
{
  int tx, ty, lx, ly;
  splitCoord(x, params.tile_size, tx, lx);
  splitCoord(y, params.tile_size, ty, ly);
  VdmTile *t = findTile(tx, ty);
  if (!t) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  return t->texels[ly * params.tile_size + lx];
}

void VdmStore::writeTexel(int x, int y, const float3 &value)
{
  int tx, ty, lx, ly;
  splitCoord(x, params.tile_size, tx, lx);
  splitCoord(y, params.tile_size, ty, ly);
  VdmTile &t = ensureTile(tx, ty);
  t.texels[ly * params.tile_size + lx] = value;
  t.boundDirty = true;
  markGpuDirty(&t);
}

void VdmStore::addTexel(int x, int y, const float3 &value)
{
  int tx, ty, lx, ly;
  splitCoord(x, params.tile_size, tx, lx);
  splitCoord(y, params.tile_size, ty, ly);
  VdmTile &t = ensureTile(tx, ty);
  t.texels[ly * params.tile_size + lx] += value;
  t.boundDirty = true;
  markGpuDirty(&t);
}

/* ---- Ptex backend (X2): per-grid texel lattices ---- */

void VdmStore::setPtexGridCount(int gridCount)
{
  gridRes_.resize(gridCount);
  for (int i = 0; i < gridCount; i++) {
    gridRes_[i] = params.resolution;
  }
}

int VdmStore::gridRes(int grid) const
{
  return grid >= 0 && grid < int(gridRes_.size()) ? gridRes_[grid] : 0;
}

void VdmStore::setGridRes(int grid, int r)
{
  if (grid >= 0 && grid < int(gridRes_.size()) && r > 0) {
    gridRes_[grid] = r;
  }
}

void VdmStore::setPtexAdjacency(std::span<const int> links)
{
  adjacency_.resize(links.size());
  for (size_t i = 0; i < links.size(); i++) {
    adjacency_[int(i)] = links[i];
  }
}

int VdmStore::tilesPerSide(int grid) const
{
  int r = gridRes(grid);
  return r > 0 ? (r + params.tile_size - 1) / params.tile_size : 0;
}

VdmTile *VdmStore::findTileP(int grid, int ltx, int lty) const
{
  int tps = tilesPerSide(grid);
  if (tps == 0 || ltx < 0 || lty < 0 || ltx >= tps || lty >= tps) {
    return nullptr;
  }
  VdmTile *const *t = const_cast<util::Map<uint64_t, VdmTile *> &>(tiles_).lookup_ptr(
      ptexTileKey(grid, lty * tps + ltx));
  return t ? *t : nullptr;
}

VdmTile &VdmStore::ensureTileP(int grid, int ltx, int lty)
{
  int tps = tilesPerSide(grid);
  return ensureTileAt(ptexTileKey(grid, lty * tps + ltx), grid, ltx, lty);
}

float3 VdmStore::texelP(int grid, int x, int y) const
{
  int r = gridRes(grid);
  if (x < 0 || y < 0 || x >= r || y >= r) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  VdmTile *t = findTileP(grid, x / params.tile_size, y / params.tile_size);
  if (!t) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  int lx = x % params.tile_size, ly = y % params.tile_size;
  return t->texels[ly * params.tile_size + lx];
}

void VdmStore::writeTexelP(int grid, int x, int y, const float3 &value)
{
  int r = gridRes(grid);
  if (x < 0 || y < 0 || x >= r || y >= r) {
    return;
  }
  VdmTile &t = ensureTileP(grid, x / params.tile_size, y / params.tile_size);
  int lx = x % params.tile_size, ly = y % params.tile_size;
  t.texels[ly * params.tile_size + lx] = value;
  t.boundDirty = true;
  markGpuDirty(&t);
}

void VdmStore::addTexelP(int grid, int x, int y, const float3 &value)
{
  int r = gridRes(grid);
  if (x < 0 || y < 0 || x >= r || y >= r) {
    return;
  }
  VdmTile &t = ensureTileP(grid, x / params.tile_size, y / params.tile_size);
  int lx = x % params.tile_size, ly = y % params.tile_size;
  t.texels[ly * params.tile_size + lx] += value;
  t.boundDirty = true;
  markGpuDirty(&t);
}

float VdmStore::gridBound(int grid)
{
  updateBounds();
  float b = 0.0f;
  for (const auto &pair : tiles_) {
    VdmTile *t = pair.value;
    if (t && t->grid == grid && t->bound > b) {
      b = t->bound;
    }
  }
  return b;
}

float3 VdmStore::sample(int face, float u, float v) const
{
  if (params.backend == VdmBackend::PTEX) {
    int r = gridRes(face);
    if (r <= 0) {
      return float3(0.0f, 0.0f, 0.0f);
    }
    // Grid-local param; taps clamp to the lattice (skirts own the seams).
    float px = u * float(r) - 0.5f;
    float py = v * float(r) - 0.5f;
    float fx = std::floor(px);
    float fy = std::floor(py);
    int x0 = int(fx), y0 = int(fy);
    float ax = px - fx, ay = py - fy;
    auto cl = [r](int x) { return x < 0 ? 0 : (x >= r ? r - 1 : x); };

    float3 t00 = texelP(face, cl(x0), cl(y0));
    float3 t10 = texelP(face, cl(x0 + 1), cl(y0));
    float3 t01 = texelP(face, cl(x0), cl(y0 + 1));
    float3 t11 = texelP(face, cl(x0 + 1), cl(y0 + 1));

    float3 b = t00 * (1.0f - ax) + t10 * ax;
    float3 tpp = t01 * (1.0f - ax) + t11 * ax;
    return b * (1.0f - ay) + tpp * ay;
  }

  // Texel centers sit at integer+0.5 in texel space.
  float px = u * float(params.resolution) - 0.5f;
  float py = v * float(params.resolution) - 0.5f;
  float fx = std::floor(px);
  float fy = std::floor(py);
  int x0 = int(fx), y0 = int(fy);
  float ax = px - fx, ay = py - fy;

  float3 t00 = texel(x0, y0);
  float3 t10 = texel(x0 + 1, y0);
  float3 t01 = texel(x0, y0 + 1);
  float3 t11 = texel(x0 + 1, y0 + 1);

  float3 b = t00 * (1.0f - ax) + t10 * ax;
  float3 tpp = t01 * (1.0f - ax) + t11 * ax;
  return b * (1.0f - ay) + tpp * ay;
}

void VdmStore::updateBounds()
{
  for (const auto &pair : tiles_) {
    VdmTile *t = pair.value;
    if (!t || !t->boundDirty) {
      continue;
    }
    float b = 0.0f;
    for (const float3 &d : t->texels) {
      float l = d.length();
      b = l > b ? l : b;
    }
    t->bound = b;
    t->boundDirty = false;
  }
}

float VdmStore::maxBound()
{
  updateBounds();
  float b = 0.0f;
  for (const auto &pair : tiles_) {
    if (pair.value && pair.value->bound > b) {
      b = pair.value->bound;
    }
  }
  return b;
}

float VdmStore::maxBoundInUvRect(float u0, float v0, float u1, float v1)
{
  updateBounds();
  if (u1 < u0 || v1 < v0 || tileCount_ == 0) {
    return 0.0f;
  }
  float ts = float(params.tile_size);
  int tx0 = int(std::floor(u0 * float(params.resolution) / ts));
  int ty0 = int(std::floor(v0 * float(params.resolution) / ts));
  int tx1 = int(std::floor(u1 * float(params.resolution) / ts));
  int ty1 = int(std::floor(v1 * float(params.resolution) / ts));

  float b = 0.0f;
  int64_t area = int64_t(tx1 - tx0 + 1) * int64_t(ty1 - ty0 + 1);
  if (area > int64_t(tileCount_)) {
    // Sparse store, huge rect: scan live tiles instead of the coord range.
    for (const auto &pair : tiles_) {
      VdmTile *t = pair.value;
      if (t && t->tx >= tx0 && t->tx <= tx1 && t->ty >= ty0 && t->ty <= ty1 &&
          t->bound > b)
      {
        b = t->bound;
      }
    }
    return b;
  }
  for (int ty = ty0; ty <= ty1; ty++) {
    for (int tx = tx0; tx <= tx1; tx++) {
      VdmTile *t = findTile(tx, ty);
      if (t && t->bound > b) {
        b = t->bound;
      }
    }
  }
  return b;
}

void VdmStore::beginDelta()
{
  if (activeDelta_) {
    Delete(activeDelta_);
  }
  activeDelta_ = New<VdmDelta>("VdmDelta");
  deltaGen_++;
}

VdmDelta *VdmStore::endDelta()
{
  VdmDelta *d = activeDelta_;
  activeDelta_ = nullptr;
  if (d && d->empty()) {
    Delete(d);
    return nullptr;
  }
  return d;
}

void VdmStore::applyDelta(VdmDelta &delta)
{
  for (VdmDelta::Entry &entry : delta.entries) {
    VdmTile *live = nullptr;
    VdmTile **slot = tiles_.lookup_ptr(entry.key);
    if (slot) {
      live = *slot;
    }
    bool entryHas = entry.texels.size() > 0;

    if (live && entryHas) {
      std::swap(live->texels, entry.texels);
      live->boundDirty = true;
      markGpuDirty(live);
    } else if (live && !entryHas) {
      // Live tile becomes absent; the delta keeps its content.
      entry.texels = std::move(live->texels);
      removeTile(entry.key);
    } else if (!live && entryHas) {
      // Key decode branches on the backend (a store never mixes key spaces).
      int tx, ty, grid = -1;
      if (params.backend == VdmBackend::PTEX) {
        grid = int(uint32_t(entry.key >> 32));
        int idx = int(uint32_t(entry.key & 0xffffffffu));
        int tps = tilesPerSide(grid);
        tx = tps > 0 ? idx % tps : 0;
        ty = tps > 0 ? idx / tps : 0;
      } else {
        tx = int(int32_t(uint32_t(entry.key >> 32)));
        ty = int(int32_t(uint32_t(entry.key & 0xffffffffu)));
      }
      VdmTile *t = New<VdmTile>("VdmTile");
      t->tx = tx;
      t->ty = ty;
      t->grid = grid;
      t->texels = std::move(entry.texels);
      t->boundDirty = true;
      entry.texels.clear();
      tiles_.insert(uint64_t(entry.key), static_cast<VdmTile *>(t));
      tileCount_++;
      gpuTopoDirty_ = true;
      markGpuDirty(t);
    }
  }
}

/* On-disk layout mirrors serial::writeMesh: BinFile header, u32 version,
 * u32 rawSize, u32 compSize, lz4 block. v2 payload: i32 backend,
 * i32 tile_size, i32 resolution, [PTEX: u32 gridCount, per-grid i32 R_g,
 * u32 adjSize, adj ints], u32 tileCount, per tile: i32 grid, i32 tx, i32 ty,
 * tile_size²·3 floats. (v1 = the same minus backend/grid fields.) */
bool VdmStore::write(std::ostream &out)
{
  std::stringstream payloadStream(std::ios::in | std::ios::out | std::ios::binary);
  {
    io::BinFile pbf(payloadStream);
    pbf.writeInt32(int(params.backend));
    pbf.writeInt32(params.tile_size);
    pbf.writeInt32(params.resolution);
    if (params.backend == VdmBackend::PTEX) {
      pbf.writeUint32(uint32_t(gridRes_.size()));
      for (int r : gridRes_) {
        pbf.writeInt32(r);
      }
      pbf.writeUint32(uint32_t(adjacency_.size()));
      for (int a : adjacency_) {
        pbf.writeInt32(a);
      }
    }
    pbf.writeUint32(uint32_t(tileCount_));
    for (const auto &pair : tiles_) {
      VdmTile *t = pair.value;
      if (!t) {
        continue;
      }
      pbf.writeInt32(t->grid);
      pbf.writeInt32(t->tx);
      pbf.writeInt32(t->ty);
      for (const float3 &d : t->texels) {
        pbf.writeFloat(d[0]);
        pbf.writeFloat(d[1]);
        pbf.writeFloat(d[2]);
      }
    }
  }

  std::string payload = payloadStream.str();
  util::Vector<uint8_t> comp;
  size_t compSize = io::compressBlock(payload.data(), payload.size(), comp);
  if (compSize == 0) {
    return false;
  }

  std::stringstream fs(std::ios::in | std::ios::out | std::ios::binary);
  io::BinFile obf(fs);
  obf.compressed = true;
  obf.writeHeader();
  obf.writeUint32(kVdmFormatVersion);
  obf.writeUint32(uint32_t(payload.size()));
  obf.writeUint32(uint32_t(compSize));
  obf.stream.write(reinterpret_cast<const char *>(comp.data()),
                   std::streamsize(compSize));

  std::string s = fs.str();
  out.write(s.data(), std::streamsize(s.size()));
  return bool(out);
}

bool VdmStore::read(std::istream &in)
{
  std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::stringstream fs(raw, std::ios::in | std::ios::out | std::ios::binary);

  io::BinFile obf(fs);
  if (!obf.readHeader() || !obf.compressed) {
    return false;
  }
  uint32_t version = obf.readUint32();
  uint32_t rawSize = obf.readUint32();
  uint32_t compSize = obf.readUint32();
  if (version == 0 || version > kVdmFormatVersion) {
    return false;
  }

  util::Vector<uint8_t> comp;
  comp.resize(compSize);
  if (compSize > 0) {
    obf.stream.read(reinterpret_cast<char *>(comp.data()), std::streamsize(compSize));
  }
  util::Vector<uint8_t> rawBuf;
  if (!io::decompressBlock(comp.data(), compSize, rawSize, rawBuf)) {
    return false;
  }

  std::string payloadStr(reinterpret_cast<const char *>(rawBuf.data()), rawSize);
  std::stringstream ps(payloadStr, std::ios::in | std::ios::out | std::ios::binary);
  io::BinFile pbf(ps);
  pbf.littleEndian = obf.littleEndian;

  params.backend = VdmBackend::ATLAS;
  if (version >= 2) {
    params.backend = VdmBackend(pbf.readInt32());
  }
  params.tile_size = pbf.readInt32();
  params.resolution = pbf.readInt32();
  if (version >= 2 && params.backend == VdmBackend::PTEX) {
    uint32_t gn = pbf.readUint32();
    gridRes_.resize(gn);
    for (uint32_t i = 0; i < gn; i++) {
      gridRes_[int(i)] = pbf.readInt32();
    }
    uint32_t an = pbf.readUint32();
    adjacency_.resize(an);
    for (uint32_t i = 0; i < an; i++) {
      adjacency_[int(i)] = pbf.readInt32();
    }
  }
  uint32_t n = pbf.readUint32();
  int texelsPerTile = params.tile_size * params.tile_size;
  for (uint32_t i = 0; i < n; i++) {
    int grid = version >= 2 ? pbf.readInt32() : -1;
    int tx = pbf.readInt32();
    int ty = pbf.readInt32();
    VdmTile &t = grid >= 0 ? ensureTileP(grid, tx, ty) : ensureTile(tx, ty);
    for (int j = 0; j < texelsPerTile; j++) {
      // Sequenced reads: constructor-arg evaluation order is unspecified.
      float x = pbf.readFloat();
      float y = pbf.readFloat();
      float z = pbf.readFloat();
      t.texels[j] = float3(x, y, z);
    }
    t.boundDirty = true;
  }
  return bool(in) || in.eof();
}

/* Mirrors boundary.cc's findUvCorner (that one is file-static). */
mesh::AttrData<float2> *findUvCornerLayer(mesh::Mesh &m)
{
  for (mesh::AttrRef &attr : m.c.attrs.attrs) {
    if (attr.type == mesh::AttrType::FLOAT2 && attr.data &&
        bool(attr.use & mesh::AttrUse::UV))
    {
      return static_cast<mesh::AttrData<float2> *>(attr.data);
    }
  }
  return nullptr;
}

void exportFaceBounds(VdmStore &store,
                      mesh::Mesh &m,
                      std::span<const int> faces,
                      util::Vector<float> &out)
{
  out.resize(faces.size());
  mesh::AttrData<float2> *uv = findUvCornerLayer(m);
  store.updateBounds();

  for (size_t i = 0; i < faces.size(); i++) {
    out[int(i)] = 0.0f;
    if (!uv) {
      continue;
    }
    int f = faces[i];
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    bool has = false;
    mesh::FaceProxy face(&m, f);
    for (auto list : face.lists()) {
      for (auto c : list) {
        float2 t = uv->safe_get(c.i);
        if (!has) {
          u0 = u1 = t[0];
          v0 = v1 = t[1];
          has = true;
        } else {
          u0 = t[0] < u0 ? t[0] : u0;
          u1 = t[0] > u1 ? t[0] : u1;
          v0 = t[1] < v0 ? t[1] : v0;
          v1 = t[1] > v1 ? t[1] : v1;
        }
      }
    }
    if (has) {
      out[int(i)] = store.maxBoundInUvRect(u0, v0, u1, v1);
    }
  }
}

} // namespace sculptcore::vdm
