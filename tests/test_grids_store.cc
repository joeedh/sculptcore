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

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  checkFixture(buildCube, 3, "cube");
  checkFixture(buildFan, 2, "fan");
  checkFixture(buildPentagon, 2, "pentagon");

  /* Skip test_end(): attr name strings stay live in the alloc tracker
   * (mirrors the other spatial/mesh tests). */
  return retval;
}
