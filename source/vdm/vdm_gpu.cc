#include "vdm_gpu.h"

#include <cmath>
#include <cstring>

namespace sculptcore::vdm {

using litestl::util::Vector;

/* Atlas row width in tiles: wide enough to keep the texture roughly square
 * without exceeding common max-texture-size limits at production tile counts. */
constexpr int kAtlasTilesPerRow = 16;

VdmGpuLayout gpuLayout(VdmStore &store)
{
  // Assign stable slots to any unslotted tiles (recycling freed slots) and
  // queue them for upload.
  store.foreachTile([&](const VdmTile &ct) {
    VdmTile &t = const_cast<VdmTile &>(ct);
    if (t.gpuSlot >= 0) {
      return;
    }
    int slot;
    if (store.gpuFreeSlots_.size() > 0) {
      slot = store.gpuFreeSlots_.pop_back();
      store.gpuSlots_[slot] = &t;
    } else {
      slot = int(store.gpuSlots_.size());
      store.gpuSlots_.append(&t);
    }
    t.gpuSlot = slot;
    t.gpuDirty = true;
    store.gpuDirtySlots_.append(slot);
    store.gpuTopoDirty_ = true;
  });

  VdmGpuLayout layout;
  layout.tile_size = store.params.tile_size;
  layout.resolution = store.params.resolution;
  layout.grid =
      (store.params.resolution + store.params.tile_size - 1) / store.params.tile_size;
  layout.slots = int(store.gpuSlots_.size());
  layout.atlas_tiles_x = layout.slots < kAtlasTilesPerRow
                             ? (layout.slots > 0 ? layout.slots : 1)
                             : kAtlasTilesPerRow;
  layout.atlas_tiles_y =
      (layout.slots + layout.atlas_tiles_x - 1) / layout.atlas_tiles_x;
  if (layout.atlas_tiles_y < 1) {
    layout.atlas_tiles_y = 1;
  }
  layout.atlas_w = layout.atlas_tiles_x * layout.tile_size;
  layout.atlas_h = layout.atlas_tiles_y * layout.tile_size;
  return layout;
}

void gpuPageTable(VdmStore &store, const VdmGpuLayout &layout, Vector<int> &out)
{
  out.resize(layout.grid * layout.grid);
  for (int i = 0; i < int(out.size()); i++) {
    out[i] = -1;
  }
  store.foreachTile([&](const VdmTile &t) {
    if (t.gpuSlot < 0 || t.tx < 0 || t.ty < 0 || t.tx >= layout.grid ||
        t.ty >= layout.grid)
    {
      return;
    }
    out[t.ty * layout.grid + t.tx] = t.gpuSlot;
  });
}

void gpuPtexTable(VdmStore &store, const VdmGpuLayout & /*layout*/, Vector<int> &out)
{
  int G = store.ptexGridCount();
  int total = 1 + G * 3;
  for (int g = 0; g < G; g++) {
    int tps = store.gridRes(g) > 0
                  ? (store.gridRes(g) + 2 + store.params.tile_size - 1) /
                        store.params.tile_size
                  : 0;
    total += tps * tps;
  }
  out.resize(total);
  out[0] = G;
  int off = 1 + G * 3;
  for (int g = 0; g < G; g++) {
    int r = store.gridRes(g);
    int tps = r > 0 ? (r + 2 + store.params.tile_size - 1) / store.params.tile_size : 0;
    out[1 + g * 3 + 0] = off;
    out[1 + g * 3 + 1] = r;
    out[1 + g * 3 + 2] = tps;
    for (int i = 0; i < tps * tps; i++) {
      out[off + i] = -1;
    }
    off += tps * tps;
  }
  store.foreachTile([&](const VdmTile &t) {
    if (t.gpuSlot < 0 || t.grid < 0 || t.grid >= G) {
      return;
    }
    int tps = out[1 + t.grid * 3 + 2];
    if (t.tx < 0 || t.ty < 0 || t.tx >= tps || t.ty >= tps) {
      return;
    }
    out[out[1 + t.grid * 3 + 0] + t.ty * tps + t.tx] = t.gpuSlot;
  });
}

bool gpuTilePixels(VdmStore &store,
                   const VdmGpuLayout &layout,
                   int slot,
                   Vector<float> &out)
{
  int n = layout.tile_size * layout.tile_size;
  out.resize(n * 4);
  if (slot < 0 || slot >= int(store.gpuSlots_.size())) {
    return false;
  }
  const VdmTile *t = store.gpuSlots_[slot];
  if (!t) {
    std::memset(out.data(), 0, size_t(n) * 4 * sizeof(float));
    return true;
  }
  for (int i = 0; i < n; i++) {
    const float3 &d = t->texels[i];
    out[i * 4 + 0] = d[0];
    out[i * 4 + 1] = d[1];
    out[i * 4 + 2] = d[2];
    out[i * 4 + 3] = 0.0f;
  }
  return true;
}

void gpuAtlasPixels(VdmStore &store, const VdmGpuLayout &layout, Vector<float> &out)
{
  size_t total = size_t(layout.atlas_w) * size_t(layout.atlas_h) * 4;
  out.resize(total);
  std::memset(out.data(), 0, total * sizeof(float));

  int ts = layout.tile_size;
  for (int slot = 0; slot < int(store.gpuSlots_.size()); slot++) {
    const VdmTile *t = store.gpuSlots_[slot];
    if (!t) {
      continue;
    }
    int ax = (slot % layout.atlas_tiles_x) * ts;
    int ay = (slot / layout.atlas_tiles_x) * ts;
    for (int y = 0; y < ts; y++) {
      float *row = out.data() + (size_t(ay + y) * layout.atlas_w + ax) * 4;
      for (int x = 0; x < ts; x++) {
        const float3 &d = t->texels[y * ts + x];
        row[x * 4 + 0] = d[0];
        row[x * 4 + 1] = d[1];
        row[x * 4 + 2] = d[2];
        row[x * 4 + 3] = 0.0f;
      }
    }
  }
}

int VdmStore::gpuLayoutOut(Vector<int> &out)
{
  VdmGpuLayout layout = gpuLayout(*this);
  out.resize(10);
  out[0] = layout.tile_size;
  out[1] = layout.resolution;
  out[2] = layout.grid;
  out[3] = layout.slots;
  out[4] = layout.atlas_tiles_x;
  out[5] = layout.atlas_tiles_y;
  out[6] = layout.atlas_w;
  out[7] = layout.atlas_h;
  // X2 additions (older readers only consume the first 8).
  out[8] = int(params.backend);
  out[9] = ptexGridCount();
  return layout.slots;
}

void VdmStore::gpuPageTableOut(Vector<int> &out)
{
  gpuPageTable(*this, gpuLayout(*this), out);
}

void VdmStore::gpuPtexTableOut(Vector<int> &out)
{
  gpuPtexTable(*this, gpuLayout(*this), out);
}

void VdmStore::gpuAtlasPixelsOut(Vector<float> &out)
{
  gpuAtlasPixels(*this, gpuLayout(*this), out);
}

int VdmStore::gpuTilePixelsOut(int slot, Vector<float> &out)
{
  return gpuTilePixels(*this, gpuLayout(*this), slot, out) ? 1 : 0;
}

int VdmStore::gpuTakeDirtyOut(Vector<int> &outSlots)
{
  return takeGpuDirty(*this, outSlots) ? 1 : 0;
}

litestl::binding::types::Struct<VdmStore> *VdmStore::defineBindings()
{
  using namespace litestl::binding;
  types::Struct<VdmStore> *st =
      new types::Struct<VdmStore>("sculptcore::vdm::VdmStore", sizeof(VdmStore));
  BIND_STRUCT_METHOD(st, tileCount, MARGS());
  BIND_STRUCT_METHOD(st, contentRev, MARGS());
  BIND_STRUCT_METHOD(st, gpuLayoutOut, MARGS("out"));
  BIND_STRUCT_METHOD(st, gpuPageTableOut, MARGS("out"));
  BIND_STRUCT_METHOD(st, gpuPtexTableOut, MARGS("out"));
  BIND_STRUCT_METHOD(st, configurePtex, MARGS("gridCount", "defaultRes", "links"));
  BIND_STRUCT_METHOD(st, gpuAtlasPixelsOut, MARGS("out"));
  BIND_STRUCT_METHOD(st, gpuTilePixelsOut, MARGS("slot", "out"));
  BIND_STRUCT_METHOD(st, gpuTakeDirtyOut, MARGS("outSlots"));
  return st;
}

bool takeGpuDirty(VdmStore &store, Vector<int> &slots)
{
  gpuLayout(store); // slot any fresh tiles so their uploads drain here too
  slots.resize(0);
  for (int slot : store.gpuDirtySlots_) {
    if (slot >= 0 && slot < int(store.gpuSlots_.size())) {
      VdmTile *t = store.gpuSlots_[slot];
      if (t) {
        t->gpuDirty = false;
        slots.append(slot);
      }
    }
  }
  store.gpuDirtySlots_.clear();
  bool topo = store.gpuTopoDirty_;
  store.gpuTopoDirty_ = false;
  return topo;
}

} // namespace sculptcore::vdm
