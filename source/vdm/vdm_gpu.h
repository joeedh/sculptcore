#pragma once

/** VDM GPU residency packing (displacementAndSubSurf plan, V3). C++ owns the
 * byte layout (the gpuBrushes D1 rule): the app allocates its texture/buffer
 * from `VdmGpuLayout`, uploads the blobs packed here, and never re-derives a
 * layout.
 *
 * The fragment path samples the store through two GPU resources:
 *  - the **tile atlas**: live tiles packed row-major into one rgba32float
 *    image (`atlas_w × atlas_h`; xyz = displacement, w = 0), each tile at a
 *    stable *slot* that survives incremental edits, so a dab uploads only its
 *    dirty tiles (no full re-pack, the VDM analogue of "no regen_gpu_node");
 *  - the **page table**: a `grid × grid` int array over UV [0,1]² mapping
 *    tile coords → atlas slot (-1 = unallocated → sample zero). Tiles outside
 *    [0,1]² hold slots but are unreachable through the table.
 *
 * Edit tracking: writeTexel/addTexel/applyDelta mark tiles GPU-dirty;
 * `takeGpuDirty` drains the slot list (and reports page-table currency) once
 * per frame. Slots are assigned lazily at first pack/drain and recycled
 * through a free list when a delta removes a tile.
 */

#include "litestl/util/vector.h"
#include "vdm_store.h"

namespace sculptcore::vdm {

struct VdmGpuLayout {
  int tile_size = 0;     // texels per tile side
  int resolution = 0;    // texels across UV [0,1]
  int grid = 0;          // page-table side: ceil(resolution / tile_size)
  int slots = 0;         // atlas capacity in tiles (highest slot + 1)
  int atlas_tiles_x = 0; // atlas arrangement, slots laid row-major
  int atlas_tiles_y = 0;
  int atlas_w = 0; // atlas pixel dims = atlas_tiles_{x,y} · tile_size
  int atlas_h = 0;
};

/* Assign slots to any unslotted tiles and return the current layout. */
VdmGpuLayout gpuLayout(VdmStore &store);

/* Fill the page table: grid² ints row-major by (ty·grid + tx). */
void gpuPageTable(VdmStore &store, const VdmGpuLayout &layout, util::Vector<int> &out);

/* PTEX (X2 stage 3): the flat per-grid offset table the fragment path binds
 * instead of the [0,1]² page table. Layout (all i32):
 *   out[0] = gridCount G
 *   out[1 + g*3 .. +2] = { slotTableOffset (absolute index into out),
 *                          R_g, tilesPerSide_g }
 *   then per grid: tps² slot ints row-major by (lty·tps + ltx), -1 = absent.
 * Tile coords are STORAGE coords (guard ring included: R+2 texel lattice). */
void gpuPtexTable(VdmStore &store, const VdmGpuLayout &layout, util::Vector<int> &out);

/* Pack one slot's tile as rgba32float (tile_size² · 4 floats, row-major).
 * A freed/never-assigned slot packs zeros. Returns false on bad slot. */
bool gpuTilePixels(VdmStore &store,
                   const VdmGpuLayout &layout,
                   int slot,
                   util::Vector<float> &out);

/* Full atlas pixels (atlas_w · atlas_h · 4 floats) — the initial upload. */
void gpuAtlasPixels(VdmStore &store,
                    const VdmGpuLayout &layout,
                    util::Vector<float> &out);

/* Drain the GPU-dirty slot list into `slots` (deduped). Returns true when the
 * page table / atlas capacity also changed (tiles created or removed) — the
 * app re-uploads the table (and grows the atlas) before the per-slot writes. */
bool takeGpuDirty(VdmStore &store, util::Vector<int> &slots);

} // namespace sculptcore::vdm
