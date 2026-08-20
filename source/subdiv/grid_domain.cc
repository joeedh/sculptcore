#include "grid_domain.h"

#include "grid_tree.h"
#include "multires.h"
#include "multires_tuning.h"

#include "litestl/util/alloc.h"
#include "litestl/util/assert.h"
#include "litestl/util/task.h"

namespace sculptcore::subdiv {

using namespace litestl;
using litestl::math::float3;
using litestl::util::Assert;

static constexpr float kNormalEps = 1e-20f;

GridLevelDomain::~GridLevelDomain()
{
  if (tree_) {
    alloc::Delete(tree_);
  }
}

const int *GridLevelDomain::gridVerts(int g) const
{
  const SubdivLevel &lvl = mr_->refiner.levels[level_ - 1];
  int w = side_ + 1;
  return &lvl.gridVerts[size_t(g) * w * w];
}

void GridLevelDomain::build(Multires &mr, int level)
{
  mr_ = &mr;
  level_ = level;
  side_ = GridsStore::sideForLevel(level);
  gridCount_ = mr.store.gridCount();
  vertCount_ = mr.refiner.levels[level - 1].vertCount;
  pos_ = &mr.levelPositions(level);
  Assert(int(pos_->size()) == vertCount_, "chain positions dense by level vert id");

  mr.levelVertGridCoordsOut(level, vertGrid);
  buildOccurrences();
  buildRing1();

  no.resize(vertCount_);
  refreshAllNormals();

  mask.resize(vertCount_);
  syncMaskFromStore();

  normalStamp_.resize(vertCount_);
  for (int i = 0; i < vertCount_; i++) {
    normalStamp_[i] = 0;
  }
  normalGen_ = 0;
}

void GridLevelDomain::buildOccurrences()
{
  int w = side_ + 1;

  occOffsets.resize(vertCount_ + 1);
  for (int i = 0; i <= vertCount_; i++) {
    occOffsets[i] = 0;
  }
  for (int g = 0; g < gridCount_; g++) {
    const int *gv = gridVerts(g);
    for (int i = 0; i < w * w; i++) {
      occOffsets[gv[i] + 1]++;
    }
  }
  for (int i = 0; i < vertCount_; i++) {
    occOffsets[i + 1] += occOffsets[i];
  }

  occCoords.resize(size_t(occOffsets[vertCount_]) * 3);
  Vector<int> fill;
  fill.resize(vertCount_);
  for (int i = 0; i < vertCount_; i++) {
    fill[i] = 0;
  }
  // Grid-ascending fill order makes the first occurrence the canonical
  // (lowest-grid) one — the same rule vertGrid encodes.
  for (int g = 0; g < gridCount_; g++) {
    const int *gv = gridVerts(g);
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        int vid = gv[v * w + u];
        int at = (occOffsets[vid] + fill[vid]++) * 3;
        occCoords[at] = g;
        occCoords[at + 1] = u;
        occCoords[at + 2] = v;
      }
    }
  }
}

/** Unique lattice-step neighbors of `vid` across all its occurrences, in
 * occurrence-major, (-u,+u,-v,+v) step order. Returns the count. */
static int gatherNeighbors(const GridLevelDomain &d,
                           const GridsStore &store,
                           int level,
                           int vid,
                           util::Vector<int, 64> &out)
{
  int w = d.gridSide() + 1;
  auto occs = d.occurrences(vid);
  static const int steps[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

  for (size_t i = 0; i < occs.size(); i += 3) {
    GridCoord c{occs[i], occs[i + 1], occs[i + 2]};
    for (auto &st : steps) {
      GridCoord n;
      if (!store.neighbor(level, c, st[0], st[1], n)) {
        continue;
      }
      int nv = d.gridVerts(n.grid)[n.v * w + n.u];
      if (nv != vid && !out.contains(nv)) {
        out.append(nv);
      }
    }
  }
  return int(out.size());
}

void GridLevelDomain::buildRing1()
{
  ring1Offsets.resize(vertCount_ + 1);
  ring1Offsets[0] = 0;

  Vector<int> counts;
  counts.resize(vertCount_);
  task::parallel_for(util::IndexRange(size_t(vertCount_)), [&](util::IndexRange range) {
    util::Vector<int, 64> nbrs;
    for (int v : range) {
      nbrs.clear();
      counts[v] = gatherNeighbors(*this, mr_->store, level_, v, nbrs);
    }
  });
  for (int v = 0; v < vertCount_; v++) {
    ring1Offsets[v + 1] = ring1Offsets[v] + counts[v];
  }

  ring1.resize(ring1Offsets[vertCount_]);
  task::parallel_for(util::IndexRange(size_t(vertCount_)), [&](util::IndexRange range) {
    util::Vector<int, 64> nbrs;
    for (int v : range) {
      nbrs.clear();
      int n = gatherNeighbors(*this, mr_->store, level_, v, nbrs);
      Assert(n == ring1Offsets[v + 1] - ring1Offsets[v], "ring1 gather deterministic");
      for (int k = 0; k < n; k++) {
        ring1[ring1Offsets[v] + k] = nbrs[k];
      }
    }
  });
}

/** Normalized Newell normal of grid cell (cu,cv) — buildLevelTopo's winding. */
static float3 cellNormal(const GridLevelDomain &d,
                         const litestl::util::Vector<float3> &pos,
                         const int *gv,
                         int cu,
                         int cv)
{
  int w = d.gridSide() + 1;
  int quad[4] = {gv[cv * w + cu], gv[cv * w + cu + 1], gv[(cv + 1) * w + cu + 1],
                 gv[(cv + 1) * w + cu]};
  float3 n(0.0f, 0.0f, 0.0f);
  for (int k = 0; k < 4; k++) {
    const float3 &p = pos[quad[k]], &q = pos[quad[(k + 1) & 3]];
    n[0] += (p[1] - q[1]) * (p[2] + q[2]);
    n[1] += (p[2] - q[2]) * (p[0] + q[0]);
    n[2] += (p[0] - q[0]) * (p[1] + q[1]);
  }
  float len = n.length();
  return len > kNormalEps ? n * (1.0f / len) : float3(0.0f, 0.0f, 0.0f);
}

float3 GridLevelDomain::vertNormal(int vid) const
{
  const Vector<float3> &p = *pos_;
  float3 n(0.0f, 0.0f, 0.0f);
  auto occs = occurrences(vid);
  // Cells are uniquely owned by their grid, and each occurrence lives in a
  // distinct grid, so this visits every incident cell exactly once.
  for (size_t i = 0; i < occs.size(); i += 3) {
    const int *gv = gridVerts(occs[i]);
    int u = occs[i + 1], v = occs[i + 2];
    for (int cv = v - 1; cv <= v; cv++) {
      for (int cu = u - 1; cu <= u; cu++) {
        if (cu < 0 || cv < 0 || cu >= side_ || cv >= side_) {
          continue;
        }
        n += cellNormal(*this, p, gv, cu, cv);
      }
    }
  }
  float len = n.length();
  return len > kNormalEps ? n * (1.0f / len) : float3(0.0f, 0.0f, 1.0f);
}

void GridLevelDomain::refreshAllNormals()
{
  task::parallel_for(util::IndexRange(size_t(vertCount_)), [&](util::IndexRange range) {
    for (int v : range) {
      no[v] = vertNormal(v);
    }
  });
}

void GridLevelDomain::refreshNormals(std::span<const int> touched)
{
  normalGen_++;
  Vector<int> work;
  work.ensure_capacity(touched.size() * 2);
  auto push = [&](int v) {
    if (normalStamp_[v] != normalGen_) {
      normalStamp_[v] = normalGen_;
      work.append(v);
    }
  };
  // A cell's Newell normal reads all four corners, so a moved vert dirties
  // every corner of every incident cell — the lattice 8-neighborhood
  // (diagonals included), not just the edge ring.
  int w = side_ + 1;
  for (int t : touched) {
    push(t);
    auto occs = occurrences(t);
    for (size_t i = 0; i < occs.size(); i += 3) {
      const int *gv = gridVerts(occs[i]);
      int u = occs[i + 1], v = occs[i + 2];
      for (int cv = v - 1; cv <= v; cv++) {
        for (int cu = u - 1; cu <= u; cu++) {
          if (cu < 0 || cv < 0 || cu >= side_ || cv >= side_) {
            continue;
          }
          push(gv[cv * w + cu]);
          push(gv[cv * w + cu + 1]);
          push(gv[(cv + 1) * w + cu + 1]);
          push(gv[(cv + 1) * w + cu]);
        }
      }
    }
  }
  task::parallel_for(util::IndexRange(work.size()), [&](util::IndexRange range) {
    for (int i : range) {
      no[work[i]] = vertNormal(work[i]);
    }
  });
}

static constexpr const char *kMaskChannel = GridLevelDomain::kMaskChannelName;

void GridLevelDomain::syncMaskFromStore()
{
  int ch = mr_->store.findChannel(util::string(kMaskChannel));
  if (ch < 0) {
    for (int i = 0; i < vertCount_; i++) {
      mask[i] = 0.0f;
    }
    return;
  }
  mr_->store.ensureLevelResident(level_);
  for (int v = 0; v < vertCount_; v++) {
    mask[v] = *mr_->store.elem(level_, ch, vertGrid[v * 3], vertGrid[v * 3 + 1],
                               vertGrid[v * 3 + 2]);
  }
}

int GridLevelDomain::ensureMaskChannel()
{
  int ch = mr_->store.findChannel(util::string(kMaskChannel));
  if (ch < 0) {
    // Both defaults are deliberate: the paint mask is Authored (subdividing
    // carries it up, deleting a level restricts it back) and persistent —
    // it is the one grid layer Blender itself has a container for.
    ch = mr_->store.addChannel(util::string(kMaskChannel), 1);
  }
  return ch;
}

bool GridLevelDomain::maskChannelExists() const
{
  return mr_ && mr_->store.findChannel(util::string(kMaskChannel)) >= 0;
}

void GridLevelDomain::flushMaskToStore()
{
  int ch = ensureMaskChannel();
  mr_->store.ensureLevelResident(level_);
  for (int v = 0; v < vertCount_; v++) {
    auto occs = occurrences(v);
    for (size_t i = 0; i < occs.size(); i += 3) {
      *mr_->store.elem(level_, ch, occs[i], occs[i + 1], occs[i + 2]) = mask[v];
    }
  }
  // Deliberately no noteAttrEdit: the whole-domain flush is the host SEEDING
  // this level from its own stored mask (Multires_writeDomainMask), not an edit
  // made here. Calling it a fine edit would push the seed down the stack on the
  // next level switch and overwrite the coarser levels with a filtered copy of
  // a level the user never painted.
}

void GridLevelDomain::flushMaskToStore(std::span<const int> verts)
{
  int ch = ensureMaskChannel();
  mr_->store.ensureLevelResident(level_);
  // Delta capture rides the write: the finer levels take new - old added onto
  // their own content (authored fine detail survives a coarse edit), so each
  // replica's previous value is read just before it is overwritten.
  Vector<int> coords;
  Vector<float> deltas;
  for (int v : verts) {
    auto occs = occurrences(v);
    for (size_t i = 0; i < occs.size(); i += 3) {
      float *dst = mr_->store.elem(level_, ch, occs[i], occs[i + 1], occs[i + 2]);
      const float d = mask[v] - *dst;
      *dst = mask[v];
      if (d != 0.0f) {
        coords.append(occs[i]);
        coords.append(occs[i + 1]);
        coords.append(occs[i + 2]);
        deltas.append(d);
      }
    }
  }
  // The touched-verts overload IS the edit: finer levels take the prolonged
  // delta now, the level below owes an update on the next level switch
  // (Multires::propagateAttrsDown), and alive finer domains re-mirror.
  mr_->store.prolongateChannelEditUp(ch,
                                     level_,
                                     std::span<const int>(coords.data(), coords.size()),
                                     std::span<const float>(deltas.data(), deltas.size()));
  mr_->noteAttrEdit(level_, ch);
  mr_->refreshFinerMaskMirrors(level_);
}

GridTree *GridLevelDomain::ensureTree(int leafVertTarget)
{
  if (!tree_) {
    if (leafVertTarget <= 0) {
      /* Size-derived (multires_tuning.h): a fixed vert target would let the
       * leaf count — and so the flat AABB scan every query pays — grow
       * linearly with the level. */
      leafVertTarget =
          multiresAutoTune(vertCount(), gridCount(), side_).gridLeafVertTarget;
    }
    tree_ = alloc::New<GridTree>("grid tree");
    tree_->build(*this, leafVertTarget);
  }
  return tree_;
}

} // namespace sculptcore::subdiv
