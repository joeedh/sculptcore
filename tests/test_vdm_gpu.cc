/* VDM GPU-residency packing (displacementAndSubSurf plan, V3 engine half):
 * stable atlas slot assignment, page-table correctness over UV [0,1]²,
 * atlas/tile pixel packing vs the store's texels, and the dirty-slot drain
 * (per-edit incremental uploads, page-table currency on create/remove). */
#include "test_util.h"

#include "vdm/vdm_gpu.h"
#include "vdm/vdm_store.h"
#include "vdm/vdm_undo.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::vdm;
using litestl::math::float3;
using litestl::util::Vector;

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  VdmStoreParams params;
  params.tile_size = 8;
  params.resolution = 64; // grid = 8×8 tiles over [0,1]²
  VdmStore store(params);

  const float3 a(1.0f, 2.0f, 3.0f), b(-4.0f, 0.0f, 0.5f);
  store.writeTexel(3, 3, a);   // tile (0,0)
  store.writeTexel(20, 10, b); // tile (2,1)

  // --- layout + slot assignment ---
  VdmGpuLayout layout = gpuLayout(store);
  test_assert(layout.grid == 8);
  test_assert(layout.slots == 2);
  test_assert(layout.atlas_w == layout.atlas_tiles_x * 8);
  test_assert(layout.atlas_h == layout.atlas_tiles_y * 8);

  // --- page table maps exactly the live tiles ---
  Vector<int> table;
  gpuPageTable(store, layout, table);
  test_assert(int(table.size()) == 64);
  int mapped = 0;
  for (int i = 0; i < 64; i++) {
    if (table[i] >= 0) {
      mapped++;
    }
  }
  test_assert(mapped == 2);
  int slot00 = table[0 * 8 + 0];
  int slot21 = table[1 * 8 + 2];
  test_assert(slot00 >= 0 && slot21 >= 0 && slot00 != slot21);

  // --- tile pixels round-trip (texel (3,3) is local (3,3) of tile (0,0)) ---
  Vector<float> pix;
  test_assert(gpuTilePixels(store, layout, slot00, pix));
  test_assert(int(pix.size()) == 8 * 8 * 4);
  int off = (3 * 8 + 3) * 4;
  test_assert(pix[off] == 1.0f && pix[off + 1] == 2.0f && pix[off + 2] == 3.0f);

  // --- atlas pixels place each slot at its row-major cell ---
  Vector<float> atlas;
  gpuAtlasPixels(store, layout, atlas);
  {
    int ax = (slot00 % layout.atlas_tiles_x) * 8 + 3;
    int ay = (slot00 / layout.atlas_tiles_x) * 8 + 3;
    int aoff = (ay * layout.atlas_w + ax) * 4;
    test_assert(atlas[aoff] == 1.0f && atlas[aoff + 1] == 2.0f);
    int bx = (slot21 % layout.atlas_tiles_x) * 8 + (20 - 16);
    int by = (slot21 / layout.atlas_tiles_x) * 8 + (10 - 8);
    int boff = (by * layout.atlas_w + bx) * 4;
    test_assert(atlas[boff] == -4.0f && atlas[boff + 2] == 0.5f);
  }

  // --- dirty drain: initial pack marks both tiles + topo ---
  Vector<int> dirty;
  bool topo = takeGpuDirty(store, dirty);
  test_assert(topo);
  test_assert(int(dirty.size()) == 2);
  test_assert(!takeGpuDirty(store, dirty));
  test_assert(dirty.size() == 0);

  // Editing an existing tile dirties just that slot, no topo change.
  store.writeTexel(3, 4, a);
  topo = takeGpuDirty(store, dirty);
  test_assert(!topo);
  test_assert(int(dirty.size()) == 1 && dirty[0] == slot00);

  // A new tile => topo dirty + the fresh slot queued (assigned at drain).
  store.writeTexel(40, 40, b); // tile (5,5)
  topo = takeGpuDirty(store, dirty);
  test_assert(topo);
  test_assert(int(dirty.size()) == 1);
  layout = gpuLayout(store);
  test_assert(layout.slots == 3);

  // --- undo delta removal recycles the slot + flags topo ---
  store.beginDelta();
  store.writeTexel(60, 60, a); // creates tile (7,7)
  VdmDelta *delta = store.endDelta();
  test_assert(delta != nullptr);
  gpuLayout(store); // slot the new tile
  takeGpuDirty(store, dirty);

  store.applyDelta(*delta); // undo: tile (7,7) removed
  topo = takeGpuDirty(store, dirty);
  test_assert(topo);
  gpuPageTable(store, gpuLayout(store), table);
  test_assert(table[7 * 8 + 7] == -1);

  store.applyDelta(*delta); // redo: tile back, recycled slot, dirty again
  topo = takeGpuDirty(store, dirty);
  test_assert(topo);
  test_assert(int(dirty.size()) == 1);
  gpuPageTable(store, gpuLayout(store), table);
  test_assert(table[7 * 8 + 7] == dirty[0]);
  alloc::Delete(delta);

  // --- bound wrappers agree with the direct API ---
  Vector<int> lay;
  int slots = store.gpuLayoutOut(lay);
  test_assert(int(lay.size()) == 10 && lay[3] == slots);
  Vector<float> atlas2;
  store.gpuAtlasPixelsOut(atlas2);
  test_assert(atlas2.size() > 0);

  fprintf(stderr, "vdm gpu: slots=%d atlas=%dx%d\n", slots, lay[6], lay[7]);

  // --- PTEX per-grid offset table (X2 stage 3) ---
  {
    VdmStoreParams pp;
    pp.tile_size = 8;
    pp.resolution = 8; // storage 10 (guard ring) → tps = 2
    pp.backend = VdmBackend::PTEX;
    VdmStore ps(pp);
    ps.setPtexGridCount(3);
    ps.setGridRes(1, 16); // storage 18 → tps = 3
    ps.writeTexelP(0, 1, 1, a);
    ps.writeTexelP(1, 15, 15, b);

    Vector<int> pt;
    ps.gpuPtexTableOut(pt);
    test_assert(pt[0] == 3);
    int off0 = pt[1], r0 = pt[2], tps0 = pt[3];
    int off1 = pt[4], r1 = pt[5], tps1 = pt[6];
    int off2 = pt[7], r2 = pt[8], tps2 = pt[9];
    test_assert(r0 == 8 && tps0 == 2);
    test_assert(r1 == 16 && tps1 == 3);
    test_assert(r2 == 8 && tps2 == 2);
    test_assert(off0 == 10 && off1 == off0 + 4 && off2 == off1 + 9);
    test_assert(int(pt.size()) == off2 + 4);
    // Texel (1,1) → storage (2,2) → tile (0,0) of grid 0; texel (15,15) →
    // storage (16,16) → tile (2,2) of grid 1. Everything else absent.
    test_assert(pt[off0 + 0 * tps0 + 0] >= 0);
    test_assert(pt[off1 + 2 * tps1 + 2] >= 0);
    int occupied = 0;
    for (int i = off0; i < int(pt.size()); i++) {
      occupied += pt[i] >= 0 ? 1 : 0;
    }
    test_assert(occupied == 2);

    Vector<int> play;
    ps.gpuLayoutOut(play);
    test_assert(play[8] == int(VdmBackend::PTEX) && play[9] == 3);
    fprintf(
        stderr, "vdm gpu ptex: table=%d ints occupied=%d\n", int(pt.size()), occupied);
  }

  return retval;
}
