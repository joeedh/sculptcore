/* Multires grids store (displacementAndSubSurf plan, S2 gate). Builds a
 * GridsStore from the same cages the S1 refiner tests use (cube, open
 * triangle fan, lone pentagon) and cross-checks the store's implicit topology
 * against the refiner's actual level meshes: every neighbor() step — grid
 * boundary crossings included — must land on a mesh-edge-adjacent vert, and
 * seamMates() must enumerate exactly the replicas of a seam vert across all
 * grid tables. Channel storage is filled per-coord and round-tripped through
 * the offset-table-headed lz4 serialization bitwise. */
#include "test_util.h"

#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/mesh_shapes.h"
#include "subdiv/grids.h"
#include "subdiv/subdiv.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace litestl;
using namespace litestl::math;
using litestl::util::Vector;
using subdiv::GridCoord;
using subdiv::GridsStore;

static Mesh *buildCube()
{
  return createCube(2, 1.0f);
}

static Mesh *buildFan()
{
  Mesh *m = alloc::New<Mesh>("grids fan");
  float co[6][3] = {{0, 0, 0},           {1, 0, 0},  {0.75f, 0.75f, 0},
                    {0, 1, 0},           {-0.75f, 0.75f, 0}, {-1, 0, 0}};
  int ids[6];
  for (int i = 0; i < 6; i++) {
    ids[i] = m->make_vertex(float3(co[i][0], co[i][1], co[i][2]));
  }
  for (int i = 0; i < 4; i++) {
    int tri[3] = {ids[0], ids[i + 1], ids[i + 2]};
    m->make_face(std::span<int>(tri, 3));
  }
  return m;
}

static Mesh *buildPentagon()
{
  Mesh *m = alloc::New<Mesh>("grids pentagon");
  float co[5][3] = {
      {1, 0, 0}, {0.25f, 1, 0}, {-1, 0.5f, 0}, {-1, -0.5f, 0}, {0.25f, -1, 0}};
  int ids[5];
  for (int i = 0; i < 5; i++) {
    ids[i] = m->make_vertex(float3(co[i][0], co[i][1], co[i][2]));
  }
  m->make_face(std::span<int>(ids, 5));
  return m;
}

/* Mesh vert id a grid coord resolves to, via the refiner's grid tables. */
static int resolve(subdiv::Refiner &r, int level, const GridCoord &c)
{
  subdiv::SubdivLevel &lvl = r.levels[level - 1];
  int w = lvl.gridSide + 1;
  return lvl.gridVerts[c.grid * w * w + c.v * w + c.u];
}

static int edgeFaceCount(Mesh &m, int e)
{
  int c0 = m.e.c[e];
  if (c0 == ELEM_NONE) {
    return 0;
  }
  int n = 0, cc = c0;
  do {
    n++;
    cc = m.c.radial_next[cc];
  } while (cc != c0);
  return n;
}

static bool isBoundaryVert(Mesh &m, int v)
{
  for (int e : m.e_of_v(v)) {
    if (edgeFaceCount(m, e) != 2) {
      return true;
    }
  }
  return false;
}

/* Deterministic unique per-slot fill value (exact small integers in float). */
static float fillValue(int channel, int level, int grid, int u, int v, int k, int w)
{
  return float(((channel * 4 + level) * 1000 + grid) * 64 + v * w + u) + float(k) * 0.25f;
}

static void checkFixture(Mesh *(*build)(), int nLevels, const char *tag)
{
  Mesh *cage = build();

  subdiv::Refiner r;
  r.refine(*cage, nLevels);

  GridsStore gs;
  gs.buildFromCage(*cage);
  int maskCh = gs.addChannel(util::string("mask"), 1); /* pre-level channel */
  for (int i = 0; i < nLevels; i++) {
    gs.addLevel();
  }
  int postCh = gs.addChannel(util::string("post"), 2); /* post-level channel */

  test_assert(gs.gridCount() == r.gridCount());
  test_assert(gs.levelCount() == nLevels);
  test_assert(gs.channelCount() == 3);
  test_assert(gs.channelElemSize(0) == 3 && gs.channelElemSize(maskCh) == 1 &&
              gs.channelElemSize(postCh) == 2);

  for (int level = 1; level <= nLevels; level++) {
    subdiv::SubdivLevel &lvl = r.levels[level - 1];
    Mesh &lm = *lvl.mesh;
    int S = GridsStore::sideForLevel(level);
    test_assert(S == lvl.gridSide);
    int w = S + 1;

    /* Replica census: how many grid-table slots hold each mesh vert. */
    Vector<int> occurrences;
    occurrences.resize(lvl.vertCount);
    for (int i = 0; i < lvl.vertCount; i++) {
      occurrences[i] = 0;
    }
    for (int gi = 0; gi < gs.gridCount() * w * w; gi++) {
      occurrences[lvl.gridVerts[gi]]++;
    }

    int crossings = 0, boundaryStops = 0;
    Vector<GridCoord> mates;
    for (int g = 0; g < gs.gridCount(); g++) {
      for (int v = 0; v <= S; v++) {
        for (int u = 0; u <= S; u++) {
          GridCoord c = {g, u, v};
          int vc = resolve(r, level, c);

          /* Every lattice step lands on a mesh-edge-adjacent vert. */
          int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
          for (auto &d : dirs) {
            GridCoord nb;
            if (!gs.neighbor(level, c, d[0], d[1], nb)) {
              test_assert(isBoundaryVert(lm, vc));
              boundaryStops++;
              continue;
            }
            if (nb.grid != g) {
              crossings++;
            }
            int vn = resolve(r, level, nb);
            test_assert(vn != vc);
            test_assert(lm.find_edge(vc, vn) != ELEM_NONE);
          }

          /* seamMates == all other replicas of this vert, exactly. */
          gs.seamMates(level, c, mates);
          test_assert(int(mates.size()) == occurrences[vc] - 1);
          for (GridCoord &m2 : mates) {
            test_assert(!(m2 == c));
            test_assert(resolve(r, level, m2) == vc);
          }

          /* Channel fill (unique per slot — replicas are independent). */
          for (int ch = 0; ch < gs.channelCount(); ch++) {
            float *e = gs.elem(level, ch, g, u, v);
            for (int k = 0; k < gs.channelElemSize(ch); k++) {
              e[k] = fillValue(ch, level, g, u, v, k, w);
            }
          }
        }
      }
    }
    fprintf(stderr, "%s L%d: crossings=%d boundaryStops=%d\n", tag, level, crossings,
            boundaryStops);
    test_assert(crossings > 0);
  }

  /* Serialize -> fresh store -> everything identical. */
  std::stringstream blob(std::ios::in | std::ios::out | std::ios::binary);
  test_assert(gs.write(blob));

  GridsStore gs2;
  test_assert(gs2.read(blob));
  test_assert(gs2.gridCount() == gs.gridCount());
  test_assert(gs2.levelCount() == gs.levelCount());
  test_assert(gs2.channelCount() == gs.channelCount());

  for (int g = 0; g < gs.gridCount(); g++) {
    for (int side = 0; side < 4; side++) {
      test_assert(gs2.link(g, side).grid == gs.link(g, side).grid);
      test_assert(gs2.link(g, side).side == gs.link(g, side).side);
    }
  }

  int diffs = 0;
  for (int level = 1; level <= gs.levelCount(); level++) {
    int S = GridsStore::sideForLevel(level), w = S + 1;
    for (int ch = 0; ch < gs.channelCount(); ch++) {
      test_assert(gs2.channelElemSize(ch) == gs.channelElemSize(ch));
      test_assert(gs2.gridsPerChunk(level, ch) == gs.gridsPerChunk(level, ch));
      test_assert(gs2.chunkCount(level, ch) == gs.chunkCount(level, ch));
      for (int g = 0; g < gs.gridCount(); g++) {
        for (int v = 0; v <= S; v++) {
          for (int u = 0; u <= S; u++) {
            const float *a = gs.elem(level, ch, g, u, v);
            const float *b = gs2.elem(level, ch, g, u, v);
            for (int k = 0; k < gs.channelElemSize(ch); k++) {
              float expect = fillValue(ch, level, g, u, v, k, w);
              if (std::memcmp(&a[k], &b[k], sizeof(float)) != 0 || a[k] != expect) {
                diffs++;
              }
            }
          }
        }
      }
    }
  }
  fprintf(stderr, "%s: round-trip diffs=%d\n", tag, diffs);
  test_assert(diffs == 0);

  alloc::Delete(cage);
}

/* P1 domain infrastructure plus B1's two independent channel axes. Face-domain
 * and non-persistent channels round-trip through serialization and through an
 * eviction cycle byte-exact; a channel added while a level is evicted does not
 * land resident under it.
 *
 * The axes: `persist` is the host contract and drives NOTHING here, so the
 * persistent `fset` and the session `sessF` must behave identically; the level
 * rule is what branches. An Authored channel is lazy, prolonged onto a new
 * finest level and restricted back off a dropped one -- an addLevel /
 * dropTopLevel round trip is the identity. A Delta channel is eager and blanks
 * both ways. */
static void gateDomainsAndSession()
{
  using subdiv::GridElemDomain;

  Mesh *cage = buildCube();
  GridsStore gs;
  gs.buildFromCage(*cage);
  /* Authored, and persistent -- the layer B1 exists to make expressible. */
  const int fset =
      gs.addChannel(util::string("fset"), 1, GridElemDomain::Face, AttrType::INT, true);
  const int sess =
      gs.addChannel(util::string("sess"), 4, GridElemDomain::Vertex, AttrType::FLOAT4, false);
  const int sessF =
      gs.addChannel(util::string("sessF"), 1, GridElemDomain::Face, AttrType::INT, false);
  /* The disp-shaped channel: persistent AND a per-level delta. */
  const int dlt = gs.addChannel(util::string("dlt"), 1, GridElemDomain::Vertex,
                                AttrType::FLOAT, true, subdiv::GridLevelRule::Delta);
  const int nLevels = 3;
  for (int i = 0; i < nLevels; i++) {
    gs.addLevel();
  }

  test_assert(gs.channelDomain(0) == GridElemDomain::Vertex);
  test_assert(gs.channelDomain(fset) == GridElemDomain::Face);
  test_assert(gs.channelPersist(fset) && gs.channelPersist(dlt));
  test_assert(!gs.channelPersist(sess) && !gs.channelPersist(sessF));
  /* Orthogonal: persistence and level rule agree on neither channel. */
  test_assert(gs.channelAuthored(fset) && gs.channelAuthored(sess) && gs.channelAuthored(sessF));
  test_assert(!gs.channelAuthored(dlt) && !gs.channelAuthored(0)); /* 0 is disp */
  test_assert(gs.channelLevelRule(fset) == subdiv::GridLevelRule::Authored);
  test_assert(gs.channelLevelRule(0) == subdiv::GridLevelRule::Delta);
  test_assert(gs.channelType(sess) == AttrType::FLOAT4);
  /* Level 2 is a 2x2 cell grid: 9 verts, 4 faces. */
  test_assert(gs.channelElemsPerGrid(2, 0) == 9 && gs.channelElemsPerGrid(2, fset) == 4);

  /* An Authored channel costs nothing until touched -- persistent or not; a
   * Delta channel is allocated up front. */
  for (int l = 1; l <= nLevels; l++) {
    test_assert(gs.chunkCount(l, dlt) > 0);
    test_assert(gs.chunkCount(l, fset) == 0);
    test_assert(gs.chunkCount(l, sess) == 0 && gs.chunkCount(l, sessF) == 0);
  }

  auto fill = [](GridsStore &g, int ch, int level) {
    const int w = GridsStore::elemWidth(level, g.channelDomain(ch));
    for (int grid = 0; grid < g.gridCount(); grid++) {
      for (int v = 0; v < w; v++) {
        for (int u = 0; u < w; u++) {
          float *e = g.elem(level, ch, grid, u, v);
          for (int k = 0; k < g.channelElemSize(ch); k++) {
            e[k] = fillValue(ch, level, grid, u, v, k, w);
          }
        }
      }
    }
  };
  auto countDiffs = [](GridsStore &g, int ch, int level) {
    const int w = GridsStore::elemWidth(level, g.channelDomain(ch));
    int diffs = 0;
    for (int grid = 0; grid < g.gridCount(); grid++) {
      for (int v = 0; v < w; v++) {
        for (int u = 0; u < w; u++) {
          const float *e = g.elem(level, ch, grid, u, v);
          for (int k = 0; k < g.channelElemSize(ch); k++) {
            const float expect = fillValue(ch, level, grid, u, v, k, w);
            if (std::memcmp(&e[k], &expect, sizeof(float)) != 0) {
              diffs++;
            }
          }
        }
      }
    }
    return diffs;
  };

  for (int ch = 0; ch < gs.channelCount(); ch++) {
    for (int l = 1; l <= nLevels; l++) {
      fill(gs, ch, l);
    }
  }
  test_assert(gs.chunkCount(1, sess) > 0); /* the lazy channel materialized */

  /* Serialize -> fresh store: metadata and every element survive. */
  std::stringstream blob(std::ios::in | std::ios::out | std::ios::binary);
  test_assert(gs.write(blob));
  GridsStore gs2;
  test_assert(gs2.read(blob));
  test_assert(gs2.channelCount() == gs.channelCount());
  int diffs = 0;
  for (int ch = 0; ch < gs.channelCount(); ch++) {
    test_assert(gs2.channelDomain(ch) == gs.channelDomain(ch));
    test_assert(gs2.channelType(ch) == gs.channelType(ch));
    test_assert(gs2.channelPersist(ch) == gs.channelPersist(ch));
    test_assert(gs2.channelLevelRule(ch) == gs.channelLevelRule(ch));
    test_assert(gs2.channelElemSize(ch) == gs.channelElemSize(ch));
    for (int l = 1; l <= nLevels; l++) {
      diffs += countDiffs(gs2, ch, l);
    }
  }
  fprintf(stderr, "domains: serialize diffs=%d\n", diffs);
  test_assert(diffs == 0);

  /* Eviction cycle: the round trip has to reproduce every domain's sizing. */
  for (int l = 1; l <= nLevels; l++) {
    gs2.evictLevel(l);
    test_assert(!gs2.levelResident(l));
  }
  test_assert(gs2.residentBytes() == 0 && gs2.evictedBytes() > 0);
  /* A channel joining a fully-evicted store must not land resident under it. */
  /* Delta, so it allocates eagerly -- an Authored channel would pass by being
   * lazy and never exercise the join-an-evicted-level path at all. */
  gs2.addChannel(util::string("late"), 2, GridElemDomain::Vertex, AttrType::FLOAT, true,
                 subdiv::GridLevelRule::Delta);
  test_assert(gs2.residentBytes() == 0);
  for (int ch = 0; ch < gs.channelCount(); ch++) {
    for (int l = 1; l <= nLevels; l++) {
      diffs += countDiffs(gs2, ch, l);
    }
  }
  fprintf(stderr, "domains: evict/rehydrate diffs=%d\n", diffs);
  test_assert(diffs == 0);

  /* addLevel: a Delta channel zero-fills, an Authored one is seeded from the
   * level below -- and `fset` proves persistence has no say in that. */
  gs.addLevel();
  const int fine = nLevels + 1;
  const int Sc = GridsStore::sideForLevel(nLevels);
  for (int l = 1; l <= nLevels; l++) {
    test_assert(countDiffs(gs, sess, l) == 0 && countDiffs(gs, sessF, l) == 0);
  }
  for (int grid = 0; grid < gs.gridCount(); grid++) {
    for (int v = 0; v < Sc * 2; v++) {
      for (int u = 0; u < Sc * 2; u++) {
        /* Both face channels: each coarse cell replicated into its four. */
        const float expectF = fillValue(fset, nLevels, grid, u >> 1, v >> 1, 0, Sc);
        test_assert(*gs.elem(fine, fset, grid, u, v) == expectF);
        const float expect = fillValue(sessF, nLevels, grid, u >> 1, v >> 1, 0, Sc);
        test_assert(*gs.elem(fine, sessF, grid, u, v) == expect);
      }
    }
    for (int v = 0; v <= Sc * 2; v++) {
      for (int u = 0; u <= Sc * 2; u++) {
        const float *e = gs.elem(fine, sess, grid, u, v);
        const int cu = u >> 1, cv = v >> 1;
        for (int k = 0; k < 4; k++) {
          /* Coincident lattice sites keep the coarse value exactly; every other
           * sample lands inside the range its coarse taps span (and so is not
           * the zero a blanked level would hold). */
          const float c00 = fillValue(sess, nLevels, grid, cu, cv, k, Sc + 1);
          if (!(u & 1) && !(v & 1)) {
            test_assert(e[k] == c00);
            continue;
          }
          const float c11 =
              fillValue(sess, nLevels, grid, cu + (u & 1), cv + (v & 1), k, Sc + 1);
          const float lo = c00 < c11 ? c00 : c11, hi = c00 < c11 ? c11 : c00;
          test_assert(e[k] >= lo && e[k] <= hi && e[k] > 0.0f);
        }
      }
    }
  }
  for (int grid = 0; grid < gs.gridCount(); grid++) {
    for (int v = 0; v <= Sc * 2; v++) {
      for (int u = 0; u <= Sc * 2; u++) {
        test_assert(*gs.elem(fine, dlt, grid, u, v) == 0.0f); /* Delta: blank */
      }
    }
  }
  fprintf(stderr, "domains: seeding held over %d grids\n", gs.gridCount());

  /* Restriction is the exact left inverse of that prolongation, so the round
   * trip loses nothing an Authored channel had -- which is the whole reason
   * dropTopLevel may not simply pop. A Delta channel is unchanged for the
   * opposite reason: nothing is carried down at all. */
  gs.dropTopLevel();
  test_assert(gs.levelCount() == nLevels);
  int rtDiffs = 0;
  for (int ch = 0; ch < gs.channelCount(); ch++) {
    for (int l = 1; l <= nLevels; l++) {
      rtDiffs += countDiffs(gs, ch, l);
    }
  }
  fprintf(stderr, "domains: addLevel/dropTopLevel diffs=%d\n", rtDiffs);
  test_assert(rtDiffs == 0);

  /* And it is a restriction, not a pop: paint authored only at the fine level
   * survives onto the level it lands on. */
  gs.addLevel();
  for (int grid = 0; grid < gs.gridCount(); grid++) {
    *gs.elem(fine, fset, grid, 0, 0) = 7.0f;
    *gs.elem(fine, sess, grid, 0, 0) = 7.0f;
    *gs.elem(fine, dlt, grid, 0, 0) = 7.0f;
  }
  gs.dropTopLevel();
  for (int grid = 0; grid < gs.gridCount(); grid++) {
    test_assert(*gs.elem(nLevels, fset, grid, 0, 0) == 7.0f);
    test_assert(*gs.elem(nLevels, sess, grid, 0, 0) == 7.0f);
    test_assert(*gs.elem(nLevels, dlt, grid, 0, 0) != 7.0f); /* Delta: dropped */
  }
  fprintf(stderr, "domains: dropTopLevel restricted authored paint down\n");

  alloc::Delete(cage);
}

/* C4: full-weighting restriction of an authored channel onto the level below
 * (GridsStore::restrictChannelDown) — the transpose of the prolongation
 * addLevel runs, and the operator a fine paint edit travels down through on a
 * downward level switch.
 *
 * The checks are the invariants an averaging operator owes rather than a
 * snapshot of it: a constant field is a fixed point everywhere (true only if
 * the per-grid partial sums are merged across seams and the weights are
 * normalized rather than assumed), no coarse value leaves the range of the fine
 * values it summarizes, every replica of a seam coord lands on the same number,
 * a face cell is exactly the mean of its four children, and a typed channel
 * moves by injection because averaging a face-set id is meaningless. The fan
 * and pentagon fixtures carry extraordinary verts, where the stencil is most
 * lopsided. */
static void gateRestrictDown(Mesh *(*build)(), int nLevels, const char *name)
{
  using subdiv::GridElemDomain;

  Mesh *cage = build();
  GridsStore gs;
  gs.buildFromCage(*cage);
  const int col =
      gs.addChannel(util::string("col"), 4, GridElemDomain::Vertex, AttrType::FLOAT4, true);
  const int cell =
      gs.addChannel(util::string("cell"), 1, GridElemDomain::Face, AttrType::FLOAT, true);
  const int fset =
      gs.addChannel(util::string("fset"), 1, GridElemDomain::Face, AttrType::INT, true);
  for (int i = 0; i < nLevels; i++) {
    gs.addLevel();
  }
  const int fine = nLevels, coarse = nLevels - 1;
  const int wf = GridsStore::elemWidth(fine, GridElemDomain::Vertex);
  const int w = GridsStore::elemWidth(coarse, GridElemDomain::Vertex);
  const int sf = GridsStore::elemWidth(fine, GridElemDomain::Face);
  const int s = GridsStore::elemWidth(coarse, GridElemDomain::Face);

  /* Nothing authored up there is nothing to carry down — and no write. */
  test_assert(!gs.restrictChannelDown(col, fine));
  test_assert(!gs.restrictChannelDown(col, 1)); /* level 1 has no level below */

  /* 1. A constant is a fixed point: seams, mesh boundary and extraordinary
   *    corners included. */
  for (int g = 0; g < gs.gridCount(); g++) {
    for (int v = 0; v < wf; v++) {
      for (int u = 0; u < wf; u++) {
        float *e = gs.elem(fine, col, g, u, v);
        for (int k = 0; k < 4; k++) {
          e[k] = 0.25f * float(k + 1);
        }
      }
    }
  }
  test_assert(gs.restrictChannelDown(col, fine));
  for (int g = 0; g < gs.gridCount(); g++) {
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        const float *e = gs.elem(coarse, col, g, u, v);
        for (int k = 0; k < 4; k++) {
          test_assert(std::fabs(e[k] - 0.25f * float(k + 1)) < 1e-6f);
        }
      }
    }
  }

  /* 2. A varying field: no coarse value leaves the fine range (an averaging
   *    operator cannot overshoot what it summarizes), and every replica of a
   *    seam coord agrees, so the restricted level is still C0 across grids. */
  float lo = 1e30f, hi = -1e30f;
  for (int g = 0; g < gs.gridCount(); g++) {
    for (int v = 0; v < wf; v++) {
      for (int u = 0; u < wf; u++) {
        float *e = gs.elem(fine, col, g, u, v);
        for (int k = 0; k < 4; k++) {
          e[k] = std::sin(float(g) * 0.7f + float(u) * 0.31f + float(v) * 0.13f + float(k));
          lo = e[k] < lo ? e[k] : lo;
          hi = e[k] > hi ? e[k] : hi;
        }
      }
    }
  }
  test_assert(gs.restrictChannelDown(col, fine));
  Vector<GridCoord> mates;
  for (int g = 0; g < gs.gridCount(); g++) {
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        const float *e = gs.elem(coarse, col, g, u, v);
        for (int k = 0; k < 4; k++) {
          test_assert(e[k] >= lo - 1e-6f && e[k] <= hi + 1e-6f);
        }
        gs.seamMates(coarse, GridCoord{g, u, v}, mates);
        for (const GridCoord &m : mates) {
          const float *o = gs.elem(coarse, col, m.grid, m.u, m.v);
          for (int k = 0; k < 4; k++) {
            /* Merged from the same partial sums, so equal to within the order
             * the additions happened to run in. */
            test_assert(std::fabs(e[k] - o[k]) < 1e-6f);
          }
        }
      }
    }
  }

  /* 3. A face cell is exactly the mean of its four children — cells belong to
   *    one grid each, so there is no seam term to merge. */
  for (int g = 0; g < gs.gridCount(); g++) {
    for (int v = 0; v < sf; v++) {
      for (int u = 0; u < sf; u++) {
        *gs.elem(fine, cell, g, u, v) = float(g) + 0.125f * float(v * sf + u);
        *gs.elem(fine, fset, g, u, v) = float(1 + ((g + u + v) % 5));
      }
    }
  }
  test_assert(gs.restrictChannelDown(cell, fine));
  test_assert(gs.restrictChannelDown(fset, fine));
  for (int g = 0; g < gs.gridCount(); g++) {
    for (int v = 0; v < s; v++) {
      for (int u = 0; u < s; u++) {
        const float mean = 0.25f * (*gs.elem(fine, cell, g, u * 2, v * 2) +
                                    *gs.elem(fine, cell, g, u * 2 + 1, v * 2) +
                                    *gs.elem(fine, cell, g, u * 2, v * 2 + 1) +
                                    *gs.elem(fine, cell, g, u * 2 + 1, v * 2 + 1));
        test_assert(std::fabs(*gs.elem(coarse, cell, g, u, v) - mean) < 1e-5f);
        /* 4. Typed: injection, so the id stays an id. */
        test_assert(*gs.elem(coarse, fset, g, u, v) == *gs.elem(fine, fset, g, u * 2, v * 2));
      }
    }
  }

  printf("restrictChannelDown %s: %d grids, level %d -> %d ok\n", name, gs.gridCount(), fine,
         coarse);
  alloc::Delete(cage);
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  gateDomainsAndSession();
  gateRestrictDown(buildCube, 3, "cube");
  gateRestrictDown(buildFan, 3, "fan");
  gateRestrictDown(buildPentagon, 3, "pentagon");
  checkFixture(buildCube, 3, "cube");
  checkFixture(buildFan, 2, "fan");
  checkFixture(buildPentagon, 2, "pentagon");

  /* Skip test_end(): attr name strings stay live in the alloc tracker
   * (mirrors the other spatial/mesh tests). */
  return retval;
}
