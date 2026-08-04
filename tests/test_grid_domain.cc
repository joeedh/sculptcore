/* Grids-native brush path, G1 gate (grid_domain.h / grid_tree.h). On a
 * displaced cube cage, per level:
 *   - the domain's positions ARE the chain cache (bit-equal to the
 *     materialized level mesh);
 *   - the lattice 1-ring CSR equals the materialized mesh's topo_cache.ring1
 *     as SETS per vert (order differs: lattice vs edge-cycle enumeration);
 *   - domain normals match the mesh path's within tolerance;
 *   - GridTree partition invariants (every vert owned by exactly one leaf,
 *     every grid in exactly one leaf), conservative sphere query, and
 *     castRay parity against the materialized SpatialTree over a ray battery;
 *   - touched-set normal refresh equals a full refill bit-exactly;
 *   - the mask mirror round-trips through the store channel with seam
 *     replicas consistent;
 *   - a mesh-path writeback drops the domain (fold-point contract). */
#include "test_util.h"

#include "mesh/mesh.h"
#include "mesh/mesh_proxy.h"
#include "mesh/mesh_shapes.h"
#include "mesh/mesh_topo_cache.h"
#include "spatial/spatial.h"
#include "spatial/spatial_base.h"
#include "subdiv/grid_domain.h"
#include "subdiv/grid_tree.h"
#include "subdiv/grids.h"
#include "subdiv/multires.h"
#include "subdiv/subdiv.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>
#include <cstring>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace litestl;
using namespace litestl::math;
using litestl::util::Vector;
using subdiv::GridLevelDomain;
using subdiv::GridRayHit;
using subdiv::GridTree;
using subdiv::Multires;
using subdiv::MultiresSlot;

/* Smooth per-VERT displacement (a low-frequency field of the level's base
 * positions): seam replicas stay consistent (per-vert), and the surface stays
 * smooth at lattice scale so normal comparisons against the mesh path are
 * meaningful — lattice-scale noise makes vertex normals weight-sensitive
 * random sums. Chains are cached during evaluation, so invalidate after. */
static void injectDisp(Multires &mr)
{
  for (int level = 1; level <= mr.maxLevel(); level++) {
    // Copy: the store writes below stale this chain entry.
    Vector<float3> base = mr.levelPositions(level);
    subdiv::SubdivLevel &lvl = mr.refiner.levels[level - 1];
    int S = lvl.gridSide, w = S + 1;
    for (int g = 0; g < mr.store.gridCount(); g++) {
      const int *gv = &lvl.gridVerts[g * w * w];
      for (int v = 0; v < w; v++) {
        for (int u = 0; u < w; u++) {
          const float3 &p = base[gv[v * w + u]];
          float *d = mr.store.elem(level, 0, g, u, v);
          // Amplitude stays small vs cell size: recalc_normals' corner-tri
          // face normal flips on strongly non-planar quads, which would make
          // the sanity comparison below meaningless.
          d[0] = 0.02f * std::sin(3.0f * p[0]) * std::cos(2.0f * p[1]);
          d[1] = 0.02f * std::sin(2.0f * p[1] + 1.0f) * std::cos(2.5f * p[2]);
          d[2] = 0.025f * std::sin(2.5f * p[2] + 0.5f) * std::cos(3.0f * p[0]);
        }
      }
    }
  }
  mr.invalidateAll();
}

static void gateLevel(Multires &mr, int level)
{
  fprintf(stderr, "--- level %d ---\n", level);
  MultiresSlot *slot = mr.setActiveLevel(level);
  test_assert(slot && slot->mesh && slot->tree);
  Mesh &lm = *slot->mesh;

  GridLevelDomain *d = mr.gridDomain(level);
  subdiv::SubdivLevel &lvl = mr.refiner.levels[level - 1];
  int S = lvl.gridSide;

  test_assert(d->vertCount() == lvl.vertCount);
  test_assert(d->vertCount() == lm.v.count);
  test_assert(d->gridSide() == S);
  test_assert(d->gridCount() == mr.store.gridCount());

  /* Positions ARE the chain cache the level mesh was materialized from. */
  {
    bool same = true;
    for (int v = 0; v < d->vertCount(); v++) {
      if (std::memcmp(&d->pos()[v], &lm.v.co[v], sizeof(float3)) != 0) {
        same = false;
        break;
      }
    }
    test_assert(same);
  }

  /* 1-ring CSR vs the materialized mesh's edge-cycle CSR, as sets. */
  {
    const VertNbrCSR &ring = lm.topo_cache.ensureRing1(lm);
    bool ok = true;
    for (int v = 0; v < d->vertCount() && ok; v++) {
      auto mine = d->neighbors(v);
      int n = int(ring.offsets[v + 1] - ring.offsets[v]);
      if (int(mine.size()) != n) {
        fprintf(stderr, "ring1 count mismatch v=%d: %d vs %d\n", v, int(mine.size()),
                n);
        ok = false;
        break;
      }
      for (int k = 0; k < n; k++) {
        int nb = ring.nbr_verts[ring.offsets[v] + k];
        bool found = false;
        for (int x : mine) {
          if (x == nb) {
            found = true;
            break;
          }
        }
        if (!found) {
          fprintf(stderr, "ring1 member mismatch v=%d nb=%d\n", v, nb);
          ok = false;
          break;
        }
      }
    }
    test_assert(ok);
  }

  /* Normals, tight gate: a weight-consistent reference accumulated over MESH
   * connectivity (normalized Newell per face, each incident face once) must
   * match the domain's cell-fan normals almost exactly — this checks the
   * occurrence table covers every incident cell across seams. */
  {
    Vector<float3> ref;
    ref.resize(d->vertCount());
    for (int v = 0; v < d->vertCount(); v++) {
      ref[v] = float3(0.0f, 0.0f, 0.0f);
    }
    for (int f : lm.f) {
      FaceProxy face(&lm, f);
      Vector<int> vs;
      for (auto list : face.lists()) {
        for (auto c : list) {
          vs.append(c.v());
        }
      }
      float3 n(0.0f, 0.0f, 0.0f);
      for (int k = 0; k < int(vs.size()); k++) {
        const float3 &p = lm.v.co[vs[k]], &q = lm.v.co[vs[(k + 1) % vs.size()]];
        n[0] += (p[1] - q[1]) * (p[2] + q[2]);
        n[1] += (p[2] - q[2]) * (p[0] + q[0]);
        n[2] += (p[0] - q[0]) * (p[1] + q[1]);
      }
      n.normalize();
      for (int v : vs) {
        ref[v] += n;
      }
    }
    double minDot = 1.0;
    for (int v = 0; v < d->vertCount(); v++) {
      ref[v].normalize();
      double dot = double(d->no[v].dot(ref[v]));
      minDot = dot < minDot ? dot : minDot;
    }
    fprintf(stderr, "normals vs connectivity reference: min dot %.6f\n", minDot);
    test_assert(minDot > 0.999);
  }

  /* Normals vs the mesh path's recalc_normals (its per-edge-radial
   * accumulation weights fans differently, so tolerance only). */
  {
    double meanDot = 0.0, minDot = 1.0;
    int minVert = -1;
    for (int v = 0; v < d->vertCount(); v++) {
      double dot = double(d->no[v].dot(lm.v.no[v]));
      meanDot += dot;
      if (dot < minDot) {
        minDot = dot;
        minVert = v;
      }
    }
    meanDot /= double(d->vertCount());
    fprintf(stderr, "normals vs mesh: mean dot %.6f, min dot %.6f\n", meanDot, minDot);
    if (minDot < 0.8) {
      const float3 &mn = d->no[minVert], &rn = lm.v.no[minVert];
      const float3 &p = d->pos()[minVert];
      fprintf(stderr,
              "  worst v=%d occs=%d nbrs=%d p=(%.4f,%.4f,%.4f) mine=(%.4f,%.4f,%.4f) "
              "mesh=(%.4f,%.4f,%.4f)\n",
              minVert, int(d->occurrences(minVert).size() / 3),
              int(d->neighbors(minVert).size()), p[0], p[1], p[2], mn[0], mn[1], mn[2],
              rn[0], rn[1], rn[2]);
    }
    test_assert(meanDot > 0.98);
    test_assert(minDot > 0.8);
  }

  /* GridTree partition invariants. The vert target is set well under the
   * level's total so the partition actually produces multiple leaves. */
  GridTree *t = d->ensureTree(/*leafVertTarget=*/96);
  {
    fprintf(stderr, "tree: %d leaves over %d grids\n", int(t->leaves.size()),
            d->gridCount());
    test_assert(int(t->leaves.size()) > 1);
    Vector<int> vertLeafCount, gridLeafCount;
    vertLeafCount.resize(d->vertCount());
    gridLeafCount.resize(d->gridCount());
    for (int i = 0; i < d->vertCount(); i++) {
      vertLeafCount[i] = 0;
    }
    for (int i = 0; i < d->gridCount(); i++) {
      gridLeafCount[i] = 0;
    }
    for (int li = 0; li < int(t->leaves.size()); li++) {
      for (int v : t->leaves[li].ownedVerts) {
        vertLeafCount[v]++;
        test_assert(t->leafOfVert[v] == li);
      }
      for (int g : t->leaves[li].grids) {
        gridLeafCount[g]++;
        test_assert(t->leafOfGrid[g] == li);
      }
    }
    bool ok = true;
    for (int v = 0; v < d->vertCount(); v++) {
      ok = ok && vertLeafCount[v] == 1;
    }
    for (int g = 0; g < d->gridCount(); g++) {
      ok = ok && gridLeafCount[g] == 1;
    }
    test_assert(ok);
  }

  /* Sphere query is conservative: every leaf owning a vert inside the sphere
   * is returned. */
  {
    const float3 centers[3] = {float3(0.0f, 0.0f, 0.5f), float3(0.4f, 0.1f, 0.0f),
                               float3(-0.3f, -0.3f, -0.3f)};
    for (const float3 &c : centers) {
      float radius = 0.35f;
      Vector<int> hits;
      t->query(c, radius, hits);
      for (int li = 0; li < int(t->leaves.size()); li++) {
        bool inside = false;
        for (int v : t->leaves[li].ownedVerts) {
          if ((d->pos()[v] - c).length() <= radius) {
            inside = true;
            break;
          }
        }
        if (inside) {
          test_assert(hits.contains(li));
        }
      }
    }
  }

  /* castRay parity vs the materialized SpatialTree: same distance, same cell
   * (the level-mesh face id is the grid-major cell index). */
  {
    int hitsBoth = 0, faceMatch = 0;
    for (int i = 0; i < 64; i++) {
      // Deterministic direction battery on a sphere of origins.
      float a = float(i) * 0.61803399f * 6.2831853f;
      float z = 1.0f - 2.0f * (float(i) + 0.5f) / 64.0f;
      float r = std::sqrt(1.0f - z * z > 0.0f ? 1.0f - z * z : 0.0f);
      float3 dir(r * std::cos(a), r * std::sin(a), z);
      float3 orig = dir * -3.0f;

      spatial::CastRayIsect mi;
      bool mh = slot->tree->castRay(orig, dir, mi);
      GridRayHit gi;
      bool gh = t->castRay(orig, dir, gi);
      test_assert(mh == gh);
      if (!mh || !gh) {
        continue;
      }
      hitsBoth++;
      // Non-convex (displaced) quads may triangulate along the other
      // diagonal in the mesh tree (CDT path), so t is tolerance-compared.
      float dt = std::fabs(mi.t - gi.t);
      if (dt > 1e-3f) {
        fprintf(stderr, "ray %d: t %.6f vs %.6f\n", i, mi.t, gi.t);
      }
      test_assert(dt <= 1e-3f);
      int cellFace = gi.grid * S * S + gi.cellV * S + gi.cellU;
      if (cellFace == mi.faceIndex) {
        faceMatch++;
      }
      test_assert(t->leafOfVert[gi.nearestVert] >= 0);
    }
    fprintf(stderr, "raycast battery: %d/64 hit, %d same cell\n", hitsBoth, faceMatch);
    test_assert(hitsBoth > 32);
    // Shared-edge hits may legitimately resolve to an adjacent primitive.
    test_assert(faceMatch >= hitsBoth * 9 / 10);
  }

  /* Touched-set normal refresh == full refill, bit-exact. */
  {
    Vector<int> touched;
    Vector<float3> orig;
    for (int v = 0; v < d->vertCount(); v += 97) {
      touched.append(v);
      orig.append(d->pos()[v]);
      d->pos()[v] += d->no[v] * 0.02f;
    }
    d->refreshNormals(std::span<const int>(touched.data(), touched.size()));
    Vector<float3> incremental;
    incremental.resize(d->vertCount());
    for (int v = 0; v < d->vertCount(); v++) {
      incremental[v] = d->no[v];
    }
    d->refreshAllNormals();
    bool same = true;
    for (int v = 0; v < d->vertCount(); v++) {
      if (std::memcmp(&incremental[v], &d->no[v], sizeof(float3)) != 0) {
        fprintf(stderr, "normal refresh mismatch at v=%d\n", v);
        same = false;
        break;
      }
    }
    test_assert(same);
    // Restore geometry bit-exactly (the domain edits the chain cache in place).
    for (int i = 0; i < int(touched.size()); i++) {
      d->pos()[touched[i]] = orig[i];
    }
    d->refreshAllNormals();
    t->refreshAllBounds();
  }

  /* Mask mirror round-trip; every store replica of a seam vert gets the
   * write. */
  {
    for (int v = 0; v < d->vertCount(); v++) {
      d->mask[v] = float(v % 17) / 16.0f;
    }
    d->flushMaskToStore();
    int ch = mr.store.findChannel(util::string("mask"));
    test_assert(ch > 0);
    for (int v = 0; v < d->vertCount(); v++) {
      d->mask[v] = -1.0f;
    }
    d->syncMaskFromStore();
    bool ok = true;
    for (int v = 0; v < d->vertCount() && ok; v++) {
      ok = d->mask[v] == float(v % 17) / 16.0f;
      auto occs = d->occurrences(v);
      for (size_t i = 0; i < occs.size() && ok; i += 3) {
        ok = *mr.store.elem(level, ch, occs[i], occs[i + 1], occs[i + 2]) ==
             d->mask[v];
      }
    }
    test_assert(ok);
  }
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  Mesh *cage = createCube(2, 1.0f);
  Multires mr;
  mr.init(*cage, 3);
  injectDisp(mr);

  gateLevel(mr, 2);
  gateLevel(mr, 3);

  /* Fold-point contract: a mesh-path edit + writeback drops the domain; the
   * refetched domain reflects the new chain positions. */
  {
    int level = 3;
    MultiresSlot *slot = mr.setActiveLevel(level);
    GridLevelDomain *before = mr.gridDomain(level);
    float3 preEdit = before->pos()[0];
    slot->mesh->v.co[0] += float3(0.0f, 0.0f, 0.05f);
    int changed = mr.writeback(level);
    test_assert(changed >= 1);
    GridLevelDomain *after = mr.gridDomain(level);
    float3 postEdit = after->pos()[0];
    fprintf(stderr, "fold point: vert0 z %.5f -> %.5f\n", preEdit[2], postEdit[2]);
    test_assert(std::fabs((postEdit - preEdit)[2] - 0.05f) < 1e-6f);
  }

  fprintf(stderr, "grid domain gates passed\n");
  /* Skip test_end(): attr name strings stay live in the alloc tracker
   * (mirrors the other subdiv tests). */
  return retval;
}
