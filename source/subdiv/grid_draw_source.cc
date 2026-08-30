#include "grid_draw_source.h"

#include "grid_domain.h"
#include "grid_tree.h"
#include "multires.h"
#include "multires_tuning.h"

#include "litestl/util/task.h"

#include <algorithm>
#include <cstdlib>

namespace sculptcore::subdiv {

using litestl::util::IndexRange;
namespace task = litestl::task;

GridDrawSource::GridDrawSource(Multires *mr, int level, int nodeTriTarget)
    : mr_(mr), level_(level)
{
  GridLevelDomain &d = *mr->gridDomain(level);
  side_ = d.gridSide();
  const char *env = getenv("SC_GRIDS_INDEXED");
  indexed_ = !(env && env[0] == '0');
  /* One node is one host draw call, so the target scales with the level
   * (multires_tuning.h) instead of being a fixed tri count. */
  buildPartition(d,
                 nodeTriTarget > 0
                     ? nodeTriTarget
                     : multiresAutoTune(d.vertCount(), d.gridCount(), d.gridSide())
                           .drawNodeTriTarget);
  buildMaterials();
  boundGen_ = mr->domainGeneration();
  update();
}

GridDrawSource::~GridDrawSource() = default;

void GridDrawSource::buildPartition(GridLevelDomain &d, int triTarget)
{
  triTarget_ = triTarget;
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
      if (indexed_) {
        // Shared lattice verts: rows row0..row0+rows per span, (S+1) wide.
        int verts = 0;
        for (const GridSpan &sp : open.spans) {
          verts += (sp.rows + 1) * (S + 1);
        }
        open.verts = verts;
        // Static per-node indices, same winding as the soup fill: per cell
        // (a,b,c) + (a,c,e). Pure function of spans + S, built once.
        open.indices.ensure_capacity(size_t(openRows) * trisPerRow * 3);
        uint32_t base = 0;
        for (const GridSpan &sp : open.spans) {
          const uint32_t w = uint32_t(S + 1);
          for (int r = 0; r < sp.rows; r++) {
            for (int u = 0; u < S; u++) {
              const uint32_t a = base + uint32_t(r) * w + uint32_t(u);
              const uint32_t b = a + 1;
              const uint32_t c = a + w + 1;
              const uint32_t e = a + w;
              open.indices.append(a);
              open.indices.append(b);
              open.indices.append(c);
              open.indices.append(a);
              open.indices.append(c);
              open.indices.append(e);
            }
          }
          base += uint32_t(sp.rows + 1) * w;
        }
      } else {
        open.verts = openRows * trisPerRow * 3;
      }
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
    } else {
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

void GridDrawSource::buildMaterials()
{
  Vector<int> gridMat;
  if (!mr_->gridMaterials(gridMat)) {
    return; // no cage materials: every node stays at 0
  }
  for (Node &n : nodes_) {
    if (n.spans.size() > 0) {
      n.material = gridMat[n.spans[0].grid];
    }
  }
}

void GridDrawSource::fillNode(GridLevelDomain &d, Node &n)
{
  const int S = side_;
  const int w = S + 1;
  n.pos.resize(n.verts);
  n.no.resize(n.verts);
  n.mask.resize(n.verts);
  n.color.resize(colorSrc_ ? n.verts : 0);
  n.uv.resize(uvSrc_ ? n.verts : 0);
  n.fset.resize((fsetSrc_ || fsetSampleSrc_) ? n.verts : 0);
  n.aabb.reset();
  const auto &pos = d.pos();
  int out = 0;
  // Derived attributes are per grid SAMPLE, not per domain vert: a seam vert
  // exists once in the domain but carries a different UV in each grid.
  auto fillAttrs = [&](int grid, size_t sample, int at) {
    if (colorSrc_) {
      n.color[at] = colorSrc_[sample];
    }
    if (uvSrc_) {
      n.uv[at] = uvSrc_[sample];
    }
    if (fsetSampleSrc_) {
      n.fset[at] = fsetSampleSrc_[sample];
    } else if (fsetSrc_) {
      n.fset[at] = fsetSrc_[grid];
    }
  };
  if (n.indices.size() > 0) {
    // Indexed: the spans' lattice rows row0..row0+rows, row-major, 7 floats
    // per vert — the triangles come from the static index stream.
    for (const GridSpan &sp : n.spans) {
      const int *gv = d.gridVerts(sp.grid);
      const size_t gbase = size_t(sp.grid) * size_t(w * w);
      for (int row = sp.row0; row <= sp.row0 + sp.rows; row++) {
        for (int u = 0; u <= S; u++) {
          const int li = row * w + u;
          const int v = gv[li];
          n.pos[out] = pos[v];
          n.no[out] = d.no[v];
          n.mask[out] = d.mask[v];
          fillAttrs(sp.grid, gbase + size_t(li), out);
          n.aabb.add(pos[v]);
          out++;
        }
      }
    }
    return;
  }
  for (const GridSpan &sp : n.spans) {
    const int *gv = d.gridVerts(sp.grid);
    const size_t gbase = size_t(sp.grid) * size_t(w * w);
    for (int row = sp.row0; row < sp.row0 + sp.rows; row++) {
      for (int u = 0; u < S; u++) {
        // Cell corners, buildLevelTopo's split: (a,b,c) + (a,c,d).
        const int la = row * w + u;
        const int lb = row * w + u + 1;
        const int lc = (row + 1) * w + u + 1;
        const int le = (row + 1) * w + u;
        const int corner[6] = {la, lb, lc, la, lc, le};
        for (int k = 0; k < 6; k++) {
          const int v = gv[corner[k]];
          n.pos[out] = pos[v];
          n.no[out] = d.no[v];
          n.mask[out] = d.mask[v];
          fillAttrs(sp.grid, gbase + size_t(corner[k]), out);
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
  // Lazy builders, and a layer built for another level reallocates in place —
  // so resolve every update on the calling thread, never in the parallel fill.
  MultiresAttrs &ga = mr_->gridAttrs();
  const auto *color = ga.colorSamples(level_);
  const auto *uv = ga.uvSamples(level_);
  const auto *fset = ga.gridFaceSetColors();
  const auto *fsetSamples = ga.faceSetSampleColors(level_);
  if (ga.generation() != attrGen_ || color != colorSrc_ || uv != uvSrc_ ||
      fset != fsetSrc_ || fsetSamples != fsetSampleSrc_)
  {
    attrGen_ = ga.generation();
    colorSrc_ = color;
    uvSrc_ = uv;
    fsetSrc_ = fset;
    fsetSampleSrc_ = fsetSamples;
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
