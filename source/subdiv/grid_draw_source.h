#pragma once

/** Grids-fed external-draw geometry source (extdraw provider v2).
 *
 * Feeds the host's external-draw ABI straight from a level's GridLevelDomain:
 * per draw node, an engine-owned de-indexed triangle soup (pos/no/mask, 6
 * corners per lattice cell) plus an AABB and consume-on-read update flags.
 * The materialized slot mesh is not involved — this is what lets the slot
 * (and its ~GpuData) go lazy.
 *
 * Partition: cell ROWS of whole grids, packed to ~kNodeTriTarget triangles.
 * Grids smaller than the target pack together in leaf-major order; grids
 * larger than it split into row bands — the sub-leaf granularity a whole-grid
 * leaf floor cannot provide at deep levels. The partition is a pure function
 * of the level topology, so node ordinals (and therefore node ids) are stable
 * for the level's lifetime; ids live in a namespace disjoint from
 * SpatialNode::id (kIdBase bit) so a provider flip can never alias a host
 * batch. Every node is BORN dirty (DATA|TOPOLOGY): a host GPU cache can
 * outlive an engine session, and deterministic ids + fixed vert counts would
 * otherwise revive a previous session's batches.
 *
 * Dirty tracking is exact, not geometric: markVerts maps moved/reshaded verts
 * through the domain's occurrence table to the grid rows whose cells read
 * them (±2 rows — the cell-Newell normal closure), so seam replicas and
 * grab-class displacement are covered by construction.
 *
 * Lifecycle: owned by the extdraw registry; Multires keeps a backref for the
 * dirty feeds and clears it from its destructor. update() never BUILDS a
 * domain — a dropped domain leaves the last-filled buffers on screen (flags
 * NONE) until an addon-driven update finds it alive again; a domain
 * generation change refills everything. */

#include "litestl/math/aabb.h"
#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <span>

namespace sculptcore::subdiv {

struct Multires;
struct GridLevelDomain;

struct GridDrawSource {
  using float3 = litestl::math::float3;
  using AABB = litestl::math::AABB<float3>;
  template <typename T> using Vector = litestl::util::Vector<T>;

  static constexpr uint32_t kIdBase = 0x40000000u;
  static constexpr int kNodeTriTarget = 2048;

  enum UpdateFlags {
    Update_None = 0,
    Update_Data = 1 << 0,
    Update_Topo = 1 << 1,
  };

  /** `rows` cell rows of `grid` starting at cell row `row0`. */
  struct GridSpan {
    int grid = -1;
    int row0 = 0;
    int rows = 0;
  };

  struct Node {
    Vector<GridSpan> spans;
    Vector<float3> pos; // verts corners, 6 per cell
    Vector<float3> no;
    Vector<float> mask;
    AABB aabb;
    int verts = 0;
    // First span's cage-face material_index (host ABI: one material per node).
    int material = 0;
    uint32_t update = Update_Data | Update_Topo; // born dirty; consume-on-read
    bool refill = true;
  };

  /** Builds the partition and does the initial fill — the caller registers a
   * source only when the domain is (or is about to be) live, so construction
   * may fetch it. `nodeTriTarget` <= 0 uses kNodeTriTarget (tests shrink it
   * to exercise multi-node partitions on small fixtures). */
  GridDrawSource(Multires *mr, int level, int nodeTriTarget = 0);
  ~GridDrawSource();
  GridDrawSource(const GridDrawSource &) = delete;
  GridDrawSource &operator=(const GridDrawSource &) = delete;

  /** Multires is being destroyed with the source still registered: keep the
   * buffers (the host may still poll) but stop touching the stack. */
  void onMultiresDestroyed()
  {
    mr_ = nullptr;
  }

  int nodeCount() const
  {
    return int(nodes_.size());
  }
  Node &node(int i)
  {
    return nodes_[i];
  }
  uint32_t nodeId(int i) const
  {
    return kIdBase + uint32_t(i);
  }
  int level() const
  {
    return level_;
  }
  /** Triangles per node the partition was built to (reporting/tests). */
  int nodeTriTarget() const
  {
    return triTarget_;
  }
  Multires *multires()
  {
    return mr_;
  }

  /** Mark the draw nodes whose triangles read `verts` (positions or the
   * normal closure): every occurrence's grid rows v-2..v+1. No-op when the
   * domain is dropped (the generation check refills everything on revive). */
  void markVerts(std::span<const int> verts);
  /** Mark every node overlapping `grids` (undo block swaps, mask reloads). */
  void markGrids(std::span<const int> grids);
  void markAllData();

  /** Refill marked nodes from the live domain (parallel). Never builds a
   * domain; see the header comment. */
  void update();

private:
  void buildPartition(GridLevelDomain &d, int triTarget);
  /** Stamp each node's `material` from the cage's materials per grid
   * (#Multires::gridMaterials); every node stays 0 without them. */
  void buildMaterials();
  void fillNode(GridLevelDomain &d, Node &n);

  Multires *mr_ = nullptr;
  int level_ = 0;
  int side_ = 0;      // cells per grid side (domain gridSide)
  int triTarget_ = 0; // tris per node the partition was built to
  uint64_t boundGen_ = 0;
  Vector<Node> nodes_;
  Vector<int> rowNode_; // grid * side_ + cellRow -> node index
};

} // namespace sculptcore::subdiv
