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
#include "mesh/mesh_proxy.h"
#include "mesh/mesh_shapes.h"
#include "spatial/spatial.h"
#include "spatial/spatial_base.h"
#include "displace/frames.h"
#include "subdiv/grids.h"
#include "subdiv/multires.h"
#include "subdiv/subdiv.h"
#include "vdm/vdm_promote.h"
#include "vdm/vdm_splat.h"
#include "vdm/vdm_store.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <algorithm>
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

static double maxResidual(const Vector<float3> &a, const Vector<float3> &b)
{
  double m = 0.0;
  for (int i = 0; i < int(a.size()); i++) {
    m = std::max(m, double((a[i] - b[i]).length()));
  }
  return m;
}

/* L2 misfit of the coarse level's subdivided surface vs the fine target. */
static double stencilResidual(Multires &mr, int level, const Vector<float3> &fine)
{
  MultiresSlot *slot = mr.materialize(level - 1);
  Vector<float3> coarse, up;
  snapshotCo(*slot->mesh, coarse);
  mr.refiner.levels[level - 1].stencil.eval(coarse, up);
  double s = 0.0;
  for (int i = 0; i < int(up.size()); i++) {
    s += double((up[i] - fine[i]).lengthSqr());
  }
  return std::sqrt(s);
}

static void level1DispBlob(Multires &mr, std::string &out)
{
  out.clear();
  for (int g = 0; g < mr.store.gridCount(); g++) {
    for (int v = 0; v < 2; v++) {
      for (int u = 0; u < 2; u++) {
        const float *d = mr.store.elem(1, 0, g, u, v);
        out.append(reinterpret_cast<const char *>(d), 3 * sizeof(float));
      }
    }
  }
}

/* Down-refit gate (S app-wiring pass): a smooth edit at the finest level is
 * least-squares-absorbed into the level below — the coarse subdivided surface
 * tracks the edit, the fine surface is preserved, level 1 stays untouched. */
static void gateDownRefit()
{
  Mesh *cage = createCube(2, 1.0f);
  Multires mr;
  mr.init(*cage, 3);

  test_assert(mr.downRefit(1) == 0); /* guard: nothing below level 1 */

  MultiresSlot *s3 = mr.setActiveLevel(3);
  int edits = 0;
  for (int i = 0; i < s3->mesh->v.count; i++) {
    float3 &p = s3->mesh->v.co[i];
    if (p[2] > 0.4f) {
      p[2] += 0.3f * (p[2] - 0.4f);
      edits++;
    }
  }
  test_assert(edits > 0);
  test_assert(mr.writeback(3) == edits);

  Vector<float3> p3, tmp;
  snapshotCo(*mr.findSlot(3)->mesh, p3);

  std::string l1Before, l1After;
  level1DispBlob(mr, l1Before);

  double resBefore = stencilResidual(mr, 3, p3);
  int changed = mr.downRefit(3);
  test_assert(changed > 0);
  double resAfter = stencilResidual(mr, 3, p3);

  fprintf(stderr, "downRefit: changed=%d residual %.6f -> %.6f\n", changed,
          resBefore, resAfter);
  test_assert(resAfter < resBefore * 0.5);

  /* Fine surface preserved: the resident slot bitwise, re-derivation within
   * frame-projection drift. */
  test_assert(mr.activeLevel() == 3);
  snapshotCo(*mr.findSlot(3)->mesh, tmp);
  test_assert(sameBits(tmp, p3));
  mr.invalidateAll();
  s3 = mr.setActiveLevel(3);
  snapshotCo(*s3->mesh, tmp);
  test_assert(maxResidual(tmp, p3) < 1e-5);

  /* Level 1 (below the refit target) untouched, bit for bit. */
  level1DispBlob(mr, l1After);
  test_assert(l1After == l1Before);

  fprintf(stderr, "downRefit: fine preserved (drift=%g), level 1 untouched\n",
          maxResidual(tmp, p3));

  alloc::Delete(cage);
}

/* Gather a materialized level's corner UVs keyed (grid, latticeU, latticeV)
 * via the same face-major walk assignGridUVs uses; asserts every corner
 * matches a lattice point and replicated lattice points agree bitwise. */
static void gatherGridUVs(Multires &mr,
                          int level,
                          Mesh &m,
                          Vector<float2> &out /* [g*(S+1)^2 + v*(S+1) + u] */)
{
  subdiv::SubdivLevel &lvl = mr.refiner.levels[level - 1];
  int S = lvl.gridSide, w = S + 1;
  AttrRef uvRef = m.c.attrs.find_attribute(AttrType::FLOAT2, "uv");
  test_assert(uvRef.exists());
  test_assert(int(uvRef.use & AttrUse::UV) != 0);
  auto *uv = static_cast<AttrData<float2> *>(uvRef.data);

  out.resize(mr.store.gridCount() * w * w);
  Vector<bool> seen;
  seen.resize(out.size());
  static const int du[4] = {0, 1, 1, 0};
  static const int dv[4] = {0, 0, 1, 1};
  int f = 0;
  for (int g = 0; g < mr.store.gridCount(); g++) {
    const int *gv = &lvl.gridVerts[g * w * w];
    for (int v = 0; v < S; v++) {
      for (int u = 0; u < S; u++, f++) {
        mesh::FaceProxy face(&m, f);
        for (auto list : face.lists()) {
          for (auto c : list) {
            int j = 0;
            while (j < 4 && gv[(v + dv[j]) * w + (u + du[j])] != c.v()) {
              j++;
            }
            test_assert(j < 4);
            int idx = g * w * w + (v + dv[j]) * w + (u + du[j]);
            float2 val = (*uv)[c.i];
            if (seen[idx]) {
              test_assert(std::memcmp(&out[idx], &val, sizeof(float2)) == 0);
            }
            out[idx] = val;
            seen[idx] = true;
          }
        }
      }
    }
  }
  for (int i = 0; i < int(seen.size()); i++) {
    test_assert(seen[i]);
  }
}

/* X1 gate: grid-chart UVs on materialized level meshes — charts sit inside
 * disjoint inset cells, and a grid's chart mapping is identical at every
 * level (same grid + same param t → bitwise-same uv), the property that lets
 * finest-level VDM texels sample correctly from any level. */
static void gateGridUVs()
{
  Mesh *cage = createCube(2, 1.0f);
  Multires mr;
  mr.init(*cage, 2);

  MultiresSlot *s1 = mr.materialize(1);
  MultiresSlot *s2 = mr.materialize(2);

  Vector<float2> uv1, uv2;
  gatherGridUVs(mr, 1, *s1->mesh, uv1);
  gatherGridUVs(mr, 2, *s2->mesh, uv2);

  int G = mr.store.gridCount();
  int cpr = 1;
  while (cpr * cpr < G) {
    cpr++;
  }
  float cell = 1.0f / float(cpr);

  /* Every lattice uv sits strictly inside its grid's cell (inset gutter). */
  auto inCell = [&](int g, const float2 &p) {
    float ox = float(g % cpr) * cell, oy = float(g / cpr) * cell;
    return p[0] > ox && p[0] < ox + cell && p[1] > oy && p[1] < oy + cell;
  };
  for (int level = 1; level <= 2; level++) {
    Vector<float2> &uvs = level == 1 ? uv1 : uv2;
    int w = subdiv::GridsStore::sideForLevel(level) + 1;
    for (int g = 0; g < G; g++) {
      for (int i = 0; i < w * w; i++) {
        test_assert(inCell(g, uvs[g * w * w + i]));
      }
    }
  }

  /* Level consistency: the four grid corners exist at both levels and must
   * map to bitwise-identical uvs (param t = 0/1 exactly). */
  int w1 = 2, w2 = 3;
  for (int g = 0; g < G; g++) {
    int corners1[4] = {0, 1, w1 * 1, w1 * 1 + 1};
    int corners2[4] = {0, 2, w2 * 2, w2 * 2 + 2};
    for (int k = 0; k < 4; k++) {
      float2 a = uv1[g * w1 * w1 + corners1[k]];
      float2 b = uv2[g * w2 * w2 + corners2[k]];
      test_assert(std::memcmp(&a, &b, sizeof(float2)) == 0);
    }
  }

  fprintf(stderr, "gridUVs: %d charts (cpr=%d), in-cell + level-consistent\n", G,
          cpr);
  alloc::Delete(cage);
}

/* X1 gate: VDM on a multires finest level — the level mesh is topology-locked,
 * the grid-chart UVs drive the splatter end-to-end, the clamp signal fires at
 * a tiny ceiling, and the lock keeps promotion off even under force. */
static void gateSubsurfVdm()
{
  Mesh *cage = createCube(2, 1.0f);
  Multires mr;
  mr.init(*cage, 2);
  MultiresSlot *slot = mr.setActiveLevel(2);
  Mesh &m = *slot->mesh;
  test_assert(m.isTopoLocked() == 1);

  // Frames + whole-mesh VDM carrier (the harness stopgap fill).
  displace::FrameProviderParams fp;
  displace::updateFramesAll(m, fp);
  for (int f : m.f) {
    slot->tree->treeMesh.f.carrier.get_data()->materialize(f);
    slot->tree->treeMesh.f.carrier[f] = int(spatial::DetailCarrier::VDM);
  }

  vdm::VdmStoreParams vp;
  vp.resolution = 512;
  vdm::VdmStore store(vp);

  // Splat at the +Z pole; the level mesh's synthesized grid UVs must route
  // the footprint into tiles.
  float3 center(0, 0, 0);
  for (int v : m.v) {
    if (m.v.co[v][2] > center[2]) {
      center = m.v.co[v];
    }
  }
  vdm::VdmSplatParams sp;
  sp.center = center;
  sp.normal = float3(0, 0, 1);
  sp.radius = 0.5f * center[2] + 0.1f;
  sp.strength = 1.0f;
  sp.alpha = 0.5f;
  vdm::VdmSplatStats st = vdm::splatDab(m, *slot->tree, store, sp);
  fprintf(stderr, "subsurfVdm: touched=%d clamped=%d\n", st.texelsTouched,
          st.texelsClamped);
  test_assert(st.texelsTouched > 0);

  // A near-zero α makes the fold ceiling tiny — the clamp (prompt) signal
  // must saturate nearly the whole footprint.
  sp.alpha = 1e-8f;
  st = vdm::splatDab(m, *slot->tree, store, sp);
  test_assert(st.texelsClamped > st.texelsTouched / 2);

  // Promotion is gated off on the locked base, even with force=1.
  Vector<int> faces, candidates;
  for (int f : m.f) {
    faces.append(f);
  }
  vdm::VdmPromoteParams pp;
  pp.force = true;
  vdm::collectPromotionCandidates(
      m, *slot->tree, store, std::span<const int>(faces.data(), faces.size()), pp,
      candidates);
  test_assert(candidates.size() == 0);
  vdm::VdmPromoteStats ps = vdm::promoteRegion(
      m, *slot->tree, store, std::span<const int>(faces.data(), faces.size()), pp,
      nullptr, nullptr);
  test_assert(ps.promoted == 0 && ps.seededVerts == 0);

  fprintf(stderr, "subsurfVdm: lock holds (no promotion under force)\n");
  alloc::Delete(cage);
}

/* X2 stage 2 gate: Ptex splat on a multires finest level — per-grid
 * rasterization through the exact (grid, localUV) corner attrs, cross-grid
 * skirt sync through the S2 adjacency links, and seam-continuous sampling
 * (both sides of every linked seam blend the same payload+guard pair). */
static void gatePtexSplat()
{
  Mesh *cage = createCube(2, 1.0f);
  Multires mr;
  mr.init(*cage, 2);
  MultiresSlot *slot = mr.setActiveLevel(2);
  Mesh &m = *slot->mesh;

  displace::FrameProviderParams fp;
  displace::updateFramesAll(m, fp);
  for (int f : m.f) {
    slot->tree->treeMesh.f.carrier.get_data()->materialize(f);
    slot->tree->treeMesh.f.carrier[f] = int(spatial::DetailCarrier::VDM);
  }

  int G = mr.store.gridCount();
  vdm::VdmStoreParams vp;
  vp.tile_size = 8;
  vp.resolution = 16;
  vp.backend = vdm::VdmBackend::PTEX;
  vdm::VdmStore store(vp);
  store.setPtexGridCount(G);
  Vector<int> adj;
  adj.resize(G * 8);
  for (int g = 0; g < G; g++) {
    for (int side = 0; side < 4; side++) {
      const subdiv::GridLink &l = mr.store.link(g, side);
      adj[g * 8 + side * 2] = l.grid;
      adj[g * 8 + side * 2 + 1] = l.side;
    }
  }
  store.setPtexAdjacency(std::span<const int>(adj.data(), adj.size()));

  float3 center(0, 0, 0);
  for (int v : m.v) {
    if (m.v.co[v][2] > center[2]) {
      center = m.v.co[v];
    }
  }
  vdm::VdmSplatParams sp;
  sp.center = center;
  sp.normal = float3(0, 0, 1);
  sp.radius = 0.9f * center[2]; // wide dab: guarantees cross-grid coverage
  sp.strength = 1.0f;
  sp.alpha = 0.5f;
  vdm::VdmSplatStats st = vdm::splatDab(m, *slot->tree, store, sp);
  fprintf(stderr, "ptexSplat: touched=%d faces=%d\n", st.texelsTouched,
          st.facesTouched);
  test_assert(st.texelsTouched > 0);
  test_assert(st.facesTouched > 1);

  // Seam continuity across every linked pair (transpose mapping, t
  // preserved): both sides must blend the identical payload+guard pair.
  auto seamSample = [&](int g, int side, float t) {
    switch (side) {
    case subdiv::GRID_SIDE_LEFT:
      return store.sample(g, 0.0f, t);
    case subdiv::GRID_SIDE_RIGHT:
      return store.sample(g, 1.0f, t);
    case subdiv::GRID_SIDE_BOTTOM:
      return store.sample(g, t, 0.0f);
    default:
      return store.sample(g, t, 1.0f);
    }
  };
  int checked = 0, active = 0;
  double worst = 0.0;
  for (int g = 0; g < G; g++) {
    for (int side = 0; side < 4; side++) {
      const subdiv::GridLink &l = mr.store.link(g, side);
      if (l.grid < 0) {
        continue;
      }
      for (int i = 0; i < 8; i++) {
        float t = (float(i) + 0.5f) / 8.0f;
        float3 a = seamSample(g, side, t);
        float3 b = seamSample(l.grid, l.side, t);
        double d = double((a - b).length());
        worst = d > worst ? d : worst;
        checked++;
        active += a.length() > 1e-4f ? 1 : 0;
      }
    }
  }
  fprintf(stderr, "ptexSplat: seams checked=%d active=%d worst=%g\n", checked,
          active, worst);
  test_assert(active > 0); // the dab actually reached seams
  test_assert(worst < 1e-5);

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

/* X5: compressed level eviction — correctness through evict/rehydrate cycles
 * (checksums vs an un-evicted control), serialize self-healing, and the
 * budget policy (finest-first, never the active level). */
/* X5: compressed level eviction. Single-instance: gateCube already proves
 * invalidateAll + rematerialize is bit-stable WITHOUT eviction, so equality
 * through evict/rehydrate cycles isolates eviction itself. (A twin-instance
 * A/B was tried first and diverged even with eviction disabled - two
 * Multires instances in one process are not bit-identical to each other,
 * an address-order quirk outside X5's scope.) */
static void gateStoreEviction()
{
  Mesh *cage = createCube(4, 1.0f);
  Multires mr;
  mr.init(*cage, 3);
  injectDisp(mr);

  MultiresSlot *slot = mr.setActiveLevel(3);
  Vector<float3> p3, tmp;
  snapshotCo(*slot->mesh, p3);
  std::string blobBefore = storeBlob(mr.store);

  size_t residentBefore = mr.store.residentBytes();
  test_assert(residentBefore > 0);

  /* Evict the coarse levels; full re-derivation reads every level's disp
   * through elem() and must rehydrate to bit-identical positions. */
  mr.store.evictLevel(1);
  mr.store.evictLevel(2);
  test_assert(!mr.store.levelResident(1));
  test_assert(!mr.store.levelResident(2));
  test_assert(mr.store.levelResident(3));
  test_assert(mr.store.residentBytes() < residentBefore);
  test_assert(mr.store.evictedBytes() > 0);

  mr.invalidateAll();
  slot = mr.setActiveLevel(3);
  snapshotCo(*slot->mesh, tmp);
  test_assert(sameBits(tmp, p3));

  /* Evict the finest level too (while inactive), then come back to it. */
  mr.setActiveLevel(1);
  mr.store.evictLevel(3);
  test_assert(!mr.store.levelResident(3));
  slot = mr.setActiveLevel(3);
  snapshotCo(*slot->mesh, tmp);
  test_assert(sameBits(tmp, p3));

  /* Serialization self-heals eviction and stays byte-identical. */
  mr.store.evictLevel(1);
  mr.store.evictLevel(2);
  test_assert(storeBlob(mr.store) == blobBefore);

  /* Budget policy: a tiny budget evicts finest-first, never the active. */
  mr.setActiveLevel(1);
  mr.storeBudgetBytes = 1;
  mr.enforceStoreBudget();
  test_assert(mr.store.levelResident(1));
  test_assert(!mr.store.levelResident(3));

  alloc::Delete(cage);
  fprintf(stderr,
          "store eviction (X5): evict/rehydrate bit-stable, serialize "
          "self-heals, budget ok\n");
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
  gateDownRefit();
  gateGridUVs();
  gateSubsurfVdm();
  gatePtexSplat();
  gateStoreEviction();

  /* Skip test_end(): attr name strings stay live in the alloc tracker
   * (mirrors the other spatial/mesh tests). */
  return retval;
}
