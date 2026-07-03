/* Multires level materialization + LRU (displacementAndSubSurf plan, S3
 * gate). On a cube cage with nonzero displacement injected at every level:
 * level-switch round-trips are lossless — an edit-free writeback leaves the
 * store byte-identical, and rematerializing a level (LRU-resident, evicted,
 * or fully re-derived from cage + store) reproduces positions bit-exactly.
 * Also: LRU budget/eviction behavior, spatial-tree castRay sanity on a
 * materialized level, and the single-edited-vert writeback path (store delta
 * written, finer level rides along, re-derivation lands within float-drift
 * epsilon). `test_multires.cc_out bench` runs the bulk build measurement
 * (plan risk #1) instead of the gate. */
#include "test_util.h"

#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/mesh_shapes.h"
#include "spatial/spatial.h"
#include "spatial/spatial_base.h"
#include "subdiv/grids.h"
#include "subdiv/multires.h"
#include "subdiv/subdiv.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <chrono>
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
using subdiv::Multires;
using subdiv::MultiresSlot;

static void snapshotCo(Mesh &m, Vector<float3> &out)
{
  out.clear();
  for (int i = 0; i < m.v.count; i++) {
    out.append(m.v.co[i]);
  }
}

static bool sameBits(const Vector<float3> &a, const Vector<float3> &b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (int i = 0; i < int(a.size()); i++) {
    if (std::memcmp(&a[i], &b[i], sizeof(float3)) != 0) {
      return false;
    }
  }
  return true;
}

static std::string storeBlob(subdiv::GridsStore &store)
{
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  store.write(ss);
  return ss.str();
}

/* Deterministic per-VERT displacement so seam replicas stay consistent. */
static void injectDisp(Multires &mr)
{
  for (int level = 1; level <= mr.maxLevel(); level++) {
    subdiv::SubdivLevel &lvl = mr.refiner.levels[level - 1];
    int S = lvl.gridSide, w = S + 1;
    for (int g = 0; g < mr.store.gridCount(); g++) {
      const int *gv = &lvl.gridVerts[g * w * w];
      for (int v = 0; v < w; v++) {
        for (int u = 0; u < w; u++) {
          int vid = gv[v * w + u];
          float *d = mr.store.elem(level, 0, g, u, v);
          d[0] = float((vid + level) % 5) * 0.015625f;
          d[1] = float((vid + 2 * level) % 3) * 0.03125f;
          d[2] = float(vid % 7) * 0.0078125f;
        }
      }
    }
  }
}

static void gateCube()
{
  Mesh *cage = createCube(2, 1.0f);
  Multires mr;
  mr.init(*cage, 3);
  test_assert(mr.maxLevel() == 3);
  injectDisp(mr);

  /* Materialize the finest level; tree answers rays. */
  MultiresSlot *s3 = mr.setActiveLevel(3);
  test_assert(s3 && s3->mesh && s3->tree);
  test_assert(mr.activeLevel() == 3);
  test_assert(s3->mesh->v.count == mr.refiner.levels[2].vertCount);

  spatial::CastRayIsect isect;
  test_assert(s3->tree->castRay(float3(0, 0, 2.0f), float3(0, 0, -1.0f), isect));

  Vector<float3> p3, tmp;
  snapshotCo(*s3->mesh, p3);

  /* Edit-free writeback: nothing changed, store byte-identical. */
  std::string before = storeBlob(mr.store);
  test_assert(mr.writeback(3) == 0);
  test_assert(storeBlob(mr.store) == before);

  /* Switch round-trip, LRU-resident path. */
  MultiresSlot *s2 = mr.setActiveLevel(2);
  test_assert(s2->mesh->v.count == mr.refiner.levels[1].vertCount);
  s3 = mr.setActiveLevel(3);
  snapshotCo(*s3->mesh, tmp);
  test_assert(sameBits(tmp, p3));
  test_assert(storeBlob(mr.store) == before);

  /* Evicted path: budget 1 forces L3 out, cached chain rebuilds it bitwise. */
  mr.lruBudget = 1;
  mr.setActiveLevel(1);
  test_assert(mr.findSlot(3) == nullptr && mr.findSlot(2) == nullptr);
  s3 = mr.setActiveLevel(3);
  snapshotCo(*s3->mesh, tmp);
  test_assert(sameBits(tmp, p3));

  /* Full re-derivation from cage + store (determinism gate). */
  mr.invalidateAll();
  s3 = mr.setActiveLevel(3);
  snapshotCo(*s3->mesh, tmp);
  test_assert(sameBits(tmp, p3));
  test_assert(storeBlob(mr.store) == before);

  /* LRU budget honored; the active level is never evicted. */
  mr.lruBudget = 2;
  mr.materialize(1);
  mr.materialize(2);
  test_assert(mr.findSlot(3) != nullptr); /* active survives */
  int resident = 0;
  for (int l = 1; l <= 3; l++) {
    resident += mr.findSlot(l) ? 1 : 0;
  }
  test_assert(resident == 2);

  fprintf(stderr, "cube: no-edit round-trips bit-stable, LRU ok\n");

  /* Edited-vert writeback. */
  Vector<float3> p2;
  s2 = mr.setActiveLevel(2);
  snapshotCo(*s2->mesh, p2);
  int editVid = 5;
  float3 edited = s2->mesh->v.co[editVid] + float3(0.125f, 0.0f, 0.0625f);
  s2->mesh->v.co[editVid] = edited;

  std::string preEdit = storeBlob(mr.store);
  test_assert(mr.writeback(2) == 1);
  test_assert(storeBlob(mr.store) != preEdit); /* delta landed in the store */

  /* Finer level rides along (rebuilt from the edited chain). */
  s3 = mr.setActiveLevel(3);
  snapshotCo(*s3->mesh, tmp);
  test_assert(!sameBits(tmp, p3));

  /* Cached-baseline rematerialize reproduces the edit bitwise. */
  s2 = mr.setActiveLevel(2);
  test_assert(std::memcmp(&s2->mesh->v.co[editVid], &edited, sizeof(float3)) == 0);

  /* Full re-derivation: the edit survives the frame-projection round-trip to
   * within float drift; untouched verts stay bit-exact. */
  mr.invalidateAll();
  s2 = mr.setActiveLevel(2);
  float3 rederived = s2->mesh->v.co[editVid];
  test_assert((rederived - edited).length() < 1e-5f);
  for (int i = 0; i < s2->mesh->v.count; i++) {
    if (i == editVid) {
      continue;
    }
    test_assert(std::memcmp(&s2->mesh->v.co[i], &p2[i], sizeof(float3)) == 0);
  }

  fprintf(stderr, "cube: edited-vert writeback ok (drift=%g)\n",
          (rederived - edited).length());

  alloc::Delete(cage);
}

/* Open-boundary + n-gon cage smoke: materialize/writeback on a fan. */
static void gateFan()
{
  Mesh *cage = alloc::New<Mesh>("multires fan");
  float co[6][3] = {{0, 0, 0},           {1, 0, 0},  {0.75f, 0.75f, 0},
                    {0, 1, 0},           {-0.75f, 0.75f, 0}, {-1, 0, 0}};
  int ids[6];
  for (int i = 0; i < 6; i++) {
    ids[i] = cage->make_vertex(float3(co[i][0], co[i][1], co[i][2]));
  }
  for (int i = 0; i < 4; i++) {
    int tri[3] = {ids[0], ids[i + 1], ids[i + 2]};
    cage->make_face(std::span<int>(tri, 3));
  }

  Multires mr;
  mr.init(*cage, 2);
  injectDisp(mr);

  MultiresSlot *s2 = mr.setActiveLevel(2);
  Vector<float3> p2, tmp;
  snapshotCo(*s2->mesh, p2);

  test_assert(mr.writeback(2) == 0);
  mr.invalidateAll();
  s2 = mr.setActiveLevel(2);
  snapshotCo(*s2->mesh, tmp);
  test_assert(sameBits(tmp, p2));
  fprintf(stderr, "fan: boundary cage bit-stable\n");

  alloc::Delete(cage);
}

/* Bulk-build measurement (plan risk #1): time each materialization phase at
 * target densities. Run manually: test_multires.cc_out bench */
static void bench()
{
  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };

  for (int maxLevel = 5; maxLevel <= 7; maxLevel++) {
    Mesh *cage = createCube(8, 1.0f);

    auto t0 = Clock::now();
    Multires mr;
    mr.init(*cage, maxLevel);
    auto t1 = Clock::now();

    /* Phase split of one cold materialization at the finest level. */
    subdiv::SubdivLevel &lvl = mr.refiner.levels[maxLevel - 1];
    MultiresSlot *slot = mr.setActiveLevel(maxLevel);
    auto t2 = Clock::now();

    spatial::CastRayIsect isect;
    bool hit = slot->tree->castRay(float3(0, 0, 4.0f), float3(0, 0, -1.0f), isect);
    auto t3 = Clock::now();

    /* Warm switch: coarse and back (the interactive path). */
    mr.setActiveLevel(maxLevel - 1);
    auto t4 = Clock::now();
    mr.setActiveLevel(maxLevel);
    auto t5 = Clock::now();

    printf("L%d: verts=%d faces=%d | init(refine)=%.0fms materialize+tree=%.0fms "
           "castRay=%.2fms(hit=%d) switchDown=%.0fms switchBackWarm=%.0fms\n",
           maxLevel, lvl.vertCount, slot->mesh->f.count, ms(t0, t1), ms(t1, t2),
           ms(t2, t3), int(hit), ms(t3, t4), ms(t4, t5));
    fflush(stdout);

    alloc::Delete(cage);
  }
}

int main(int argc, char **argv)
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  if (argc > 1 && std::strcmp(argv[1], "bench") == 0) {
    bench();
    return 0;
  }

  gateCube();
  gateFan();

  /* Skip test_end(): attr name strings stay live in the alloc tracker
   * (mirrors the other spatial/mesh tests). */
  return retval;
}
