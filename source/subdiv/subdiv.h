#pragma once

/** Uniform Catmull-Clark refiner + cached stencil tables (displacementAndSubSurf
 * plan, S1). Level 1 splits every n-gon into n quads; later levels are regular
 * quad refinement. Sharp/boundary crease rules come from boundary::EDGE_SHARP
 * plus mesh boundary (non-2-manifold edges are treated as creases). Each level
 * caches a StencilTable (fine vert = fixed sparse combination of the previous
 * level's verts); position evaluation is DEFINED as evaluating those rows, so
 * re-evaluating from the cage through the cached chain is bit-identical to
 * re-running the refiner — the S5 GPU SpMV consistency anchor. */

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::subdiv {

/** Sparse CSR map from one level's verts to the next (fine row i = verts of the
 * refined level, in creation order == vert id). Row entries are ascending by
 * coarse vert id and evaluated in that order as a fused-multiply-add chain
 * (std::fma per component — single IEEE rounding, so the GPU SpMV's fma()
 * reproduces it bit-exactly; plain mul+add would be driver-contractable).
 * This is the canonical arithmetic the refiner and the S5 GPU pass share. */
struct StencilTable {
  litestl::util::Vector<int> offsets;   /* fineCount+1, offsets[0] == 0 */
  litestl::util::Vector<int> indices;   /* coarse vert ids, ascending per row */
  litestl::util::Vector<float> weights; /* index-parallel with `indices` */
  int coarseCount = 0; /* coarse id space (v.capacity() of the source level) */
  int fineCount = 0;

  /** Evaluate every row: dst[i] = Σ src[indices[k]] * weights[k], in row order.
   * `src` is indexed by coarse vert id (size >= coarseCount live-id range). */
  void eval(const litestl::util::Vector<litestl::math::float3> &src,
            litestl::util::Vector<litestl::math::float3> &dst) const;
};

/** One refinement level's outputs. Grids follow the Ptex __faceindex
 * convention: one grid per cage face corner, in cage (face id, loop order)
 * enumeration order. Grid axes: (0,0) = the corner's vert, +u along the
 * corner's edge, +v along the previous corner's edge; vert (u,v) lives at
 * gridVerts[g*(S+1)^2 + v*(S+1) + u] and quad cell (u,v) at
 * gridFaces[g*S^2 + v*S + u], S = gridSide. */
struct SubdivLevel {
  int vertCount = 0;
  int gridSide = 1; /* quad cells per grid side: 2^(levelIndex) */
  StencilTable stencil;

  /* Previous-level element id -> this level's vert id (ELEM_NONE for holes). */
  litestl::util::Vector<int> facePointOf;
  litestl::util::Vector<int> edgePointOf;
  litestl::util::Vector<int> vertPointOf;

  litestl::util::Vector<int> gridVerts;
  litestl::util::Vector<int> gridFaces;

  mesh::Mesh *mesh = nullptr; /* owned by the Refiner */
};

struct Refiner {
  Refiner() = default;
  Refiner(const Refiner &) = delete;
  Refiner &operator=(const Refiner &) = delete;
  ~Refiner();

  /** Uniformly refine `levelCount` times from `cage` (not owned, not mutated
   * beyond a topology thaw). Rebuilds all levels from scratch. */
  void refine(mesh::Mesh &cage, int levelCount);

  /** Evaluate level `level` (1-based) positions from cage positions through the
   * cached stencil chain — bit-identical to the positions refine() produced.
   * `cageCo` is indexed by cage vert id (use gatherVertCo; size v.capacity()). */
  void evalFromCage(const litestl::util::Vector<litestl::math::float3> &cageCo,
                    int level,
                    litestl::util::Vector<litestl::math::float3> &out) const;

  void clear();

  /** Free every level's materialized mesh, keeping stencils, counts, and grid
   * tables. Topology is rebuildable from gridVerts (see subdiv::Multires) —
   * this is the memory-reclaim step after refine() for stack owners that
   * materialize levels on demand. */
  void releaseMeshes();

  int gridCount() const
  {
    return gridCount_;
  }

  litestl::util::Vector<SubdivLevel> levels; /* [0] = first subdivision */
  int gridCount_ = 0;                        /* == total cage corners */
};

/** Gather a mesh's vert positions into a dense-by-id vector (size
 * v.capacity(); free slots zeroed) — the `src`/`cageCo` layout eval expects. */
void gatherVertCo(mesh::Mesh &m, litestl::util::Vector<litestl::math::float3> &out);

} // namespace sculptcore::subdiv
