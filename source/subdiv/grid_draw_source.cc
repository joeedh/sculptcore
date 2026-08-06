#include "grid_draw_source.h"

#include "grid_domain.h"
#include "grid_tree.h"
#include "multires.h"

#include "litestl/util/task.h"

#include <algorithm>

namespace sculptcore::subdiv {

using litestl::util::IndexRange;
namespace task = litestl::task;

GridDrawSource::GridDrawSource(Multires *mr, int level, int nodeTriTarget)
    : mr_(mr), level_(level)
{
  GridLevelDomain &d = *mr->gridDomain(level);
  side_ = d.gridSide();
  buildPartition(d, nodeTriTarget > 0 ? nodeTriTarget : kNodeTriTarget);
  boundGen_ = mr->domainGeneration();
  update();
}

GridDrawSource::~GridDrawSource() = default;

void GridDrawSource::buildPartition(GridLevelDomain &d, int triTarget)
{
  GridTree *tree = d.ensureTree();
  const int S = side_;
  const int trisPerRow = 2 * S;
  // Rows per node so a node holds ~triTarget tris; a whole small grid
  // contributes all its rows to the open node, a large grid splits into
  // bands. Grids pack in leaf-major order (per-leaf spatial coherence).
  const int rowsPerNode = int(std::max(1, triTarget / trisPerRow));

  rowNode_.resize(size_t(d.gridCount()) * S);
  Node open;
  int openRows = 0;
  auto flush = [&]() {
    if (openRows > 0) {
      open.verts = openRows * trisPerRow * 3;
      nodes_.append(std::move(open));
      open = Node();
      openRows = 0;
    }
  };
  auto push = [&](int grid, int row0, int rows) {
    for (int r = row0; r < row0 + rows; r++) {
      rowNode_[size_t(grid) * S + r] = int(nodes_.size());
    }
    if (open.spans.size() > 0 && open.spans.last().grid == grid &&
        open.spans.last().row0 + open.spans.last().rows == row0)
    {
      open.spans.last().rows += rows;
    }
    else {
      open.spans.append(GridSpan{grid, row0, rows});
    }
    openRows += rows;
    if (openRows >= rowsPerNode) {
      flush();
    }
  };
  for (const GridTree::Leaf &leaf : tree->leaves) {
    for (int g : leaf.grids) {
      int r = 0;
      while (r < S) {
        int take = std::min(S - r, rowsPerNode - openRows);
        push(g, r, take);
        r += take;
      }
    }
  }
  flush();
}

void GridDrawSource::fillNode(GridLevelDomain &d, Node &n)
{
  const int S = side_;
  n.pos.resize(n.verts);
  n.no.resize(n.verts);
  n.mask.resize(n.verts);
  n.aabb.reset();
  const auto &pos = d.pos();
  int out = 0;
  for (const GridSpan &sp : n.spans) {
    const int *gv = d.gridVerts(sp.grid);
    for (int row = sp.row0; row < sp.row0 + sp.rows; row++) {
      for (int u = 0; u < S; u++) {
        // Cell corners, buildLevelTopo's split: (a,b,c) + (a,c,d).
        const int a = gv[row * (S + 1) + u];
        const int b = gv[row * (S + 1) + u + 1];
        const int c = gv[(row + 1) * (S + 1) + u + 1];
        const int e = gv[(row + 1) * (S + 1) + u];
        const int corner[6] = {a, b, c, a, c, e};
        for (int k = 0; k < 6; k++) {
          const int v = corner[k];
          n.pos[out] = pos[v];
          n.no[out] = d.no[v];
          n.mask[out] = d.mask[v];
          n.aabb.add(pos[v]);
          out++;
        }
      }
    }
  }
}

void GridDrawSource::markVerts(std::span<const int> verts)
{
  if (!mr_ || !mr_->hasGridDomain(level_)) {
    return; // dropped: the generation check refills everything on revive
  }
  GridLevelDomain &d = *mr_->gridDomain(level_);
  const int S = side_;
  for (int v : verts) {
    std::span<const int> occ = d.occurrences(v);
    for (size_t i = 0; i < occ.size(); i += 3) {
      const int grid = occ[i];
      const int lv = occ[i + 2];
      // Cells reading v's position sit in rows lv-1..lv; the cell-Newell
      // normal closure reaches one lattice ring further: rows lv-2..lv+1.
      const int r0 = std::max(0, lv - 2);
      const int r1 = std::min(S - 1, lv + 1);
      for (int r = r0; r <= r1; r++) {
        Node &n = nodes_[rowNode_[size_t(grid) * S + r]];
        n.refill = true;
      }
    }
  }
}

void GridDrawSource::markGrids(std::span<const int> grids)
{
  const int S = side_;
  for (int g : grids) {
    for (int r = 0; r < S; r++) {
      nodes_[rowNode_[size_t(g) * S + r]].refill = true;
    }
  }
}

void GridDrawSource::markAllData()
{
  for (Node &n : nodes_) {
    n.refill = true;
  }
}

void GridDrawSource::update()
{
  if (!mr_ || !mr_->hasGridDomain(level_)) {
    return;
  }
  GridLevelDomain &d = *mr_->gridDomain(level_);
  const uint64_t gen = mr_->domainGeneration();
  if (gen != boundGen_) {
    boundGen_ = gen;
    markAllData();
  }
  Vector<int> dirty;
  for (int i = 0; i < int(nodes_.size()); i++) {
    if (nodes_[i].refill) {
      dirty.append(i);
    }
  }
  if (dirty.size() == 0) {
    return;
  }
  task::parallel_for(IndexRange(dirty.size()), [&](IndexRange range) {
    for (size_t i : range) {
      Node &n = nodes_[dirty[int(i)]];
      fillNode(d, n);
      n.refill = false;
      n.update |= Update_Data;
    }
  });
}

} // namespace sculptcore::subdiv
