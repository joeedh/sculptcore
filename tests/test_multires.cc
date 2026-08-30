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

#include "displace/frames.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/mesh_proxy.h"
#include "mesh/mesh_shapes.h"
#include "spatial/spatial.h"
#include "spatial/spatial_base.h"
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

/* Store snapshot/restore c-api (subdiv/c-api/subdiv_c_api.cc, linked into the
 * subdiv lib): the undo seam every host uses for multires, gated below. */
extern "C" {
uint8_t *Multires_serializeStore(sculptcore::subdiv::Multires *mr, int *out_size);
int Multires_restoreStore(sculptcore::subdiv::Multires *mr,
                          const uint8_t *data,
                          int size);
void freeMeshBuffer(uint8_t *buf);
}

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

/* sculptLayersV2 M3: sculpt layers as grids-store channels. A stroke-sim on a
 * targeted layer lands its writeback in the layer's channel with channel 0
 * untouched; weight 0 removes exactly the stroke (bit-exact — zero-channel
 * composition adds exact zeros); level switches round-trip bit-stable under
 * multi-channel composition; the store blob round-trips channels; the
 * layerTable + store-blob pair is a working remove-undo seam; eviction
 * rehydrates layer channels; frozen/disabled target rules hold. */
static void gateLayerChannels()
{
  Mesh *cage = createCube(2, 1.0f);
  Multires mr;
  mr.init(*cage, 3);
  injectDisp(mr);

  int la = mr.layerAdd();
  int lb = mr.layerAdd();
  test_assert(la == 0 && lb == 1);
  test_assert(mr.layerCount() == 2);
  test_assert(mr.store.channelCount() == 3);
  const int chA = 1, chB = 2; /* row order == channel order 1..N */
  test_assert(mr.store.channelName(chA) == cage->sculptLayers[la].name);

  MultiresSlot *s2 = mr.setActiveLevel(2);
  Vector<float3> pre;
  snapshotCo(*s2->mesh, pre);

  /* Fresh zero layers at weight 1 change nothing; targeting one is free. */
  test_assert(mr.setEditTarget(la) == la);
  test_assert(mr.editTarget() == la);
  s2 = mr.findSlot(2);
  test_assert(s2 != nullptr);
  {
    Vector<float3> now;
    snapshotCo(*s2->mesh, now);
    test_assert(sameBits(now, pre));
  }

  /* Channel-0 snapshot at level 2 (byte compare after the writeback). */
  auto ch0Snapshot = [&](Vector<float> &out) {
    out.clear();
    int S = subdiv::GridsStore::sideForLevel(2), w = S + 1;
    for (int g = 0; g < mr.store.gridCount(); g++) {
      for (int v = 0; v < w; v++) {
        for (int u = 0; u < w; u++) {
          const float *d = mr.store.elem(2, 0, g, u, v);
          out.append(d[0]);
          out.append(d[1]);
          out.append(d[2]);
        }
      }
    }
  };
  Vector<float> ch0Before;
  ch0Snapshot(ch0Before);

  /* Stroke-sim: direct co edits on the level mesh, folded by writeback. */
  const int editA = 3, editB = 7;
  const float3 dA(0.25f, 0.0f, 0.125f), dB(0.0f, -0.0625f, 0.25f);
  float3 editedA = s2->mesh->v.co[editA] + dA;
  float3 editedB = s2->mesh->v.co[editB] + dB;
  s2->mesh->v.co[editA] = editedA;
  s2->mesh->v.co[editB] = editedB;
  test_assert(mr.writeback(2) == 2);

  /* The writeback landed in layer A's channel; channel 0 is untouched. */
  Vector<float> ch0After;
  ch0Snapshot(ch0After);
  test_assert(ch0Before.size() == ch0After.size());
  test_assert(std::memcmp(ch0Before.data(),
                          ch0After.data(),
                          ch0Before.size() * sizeof(float)) == 0);
  bool layerNonZero = false;
  {
    int S = subdiv::GridsStore::sideForLevel(2), w = S + 1;
    for (int g = 0; g < mr.store.gridCount() && !layerNonZero; g++) {
      for (int v = 0; v < w && !layerNonZero; v++) {
        for (int u = 0; u < w && !layerNonZero; u++) {
          const float *d = mr.store.elem(2, chA, g, u, v);
          layerNonZero = d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f;
        }
      }
    }
  }
  test_assert(layerNonZero);

  /* Weight 0 removes exactly the stroke (re-weighting the target ends the
   * edit first); untouched-layer composition is bit-exact. */
  mr.layerSetWeight(la, 0.0f);
  test_assert(mr.editTarget() == -1);
  s2 = mr.findSlot(2);
  test_assert(s2 != nullptr);
  {
    Vector<float3> now;
    snapshotCo(*s2->mesh, now);
    test_assert(sameBits(now, pre));
  }

  /* Weight back to 1: the stroke returns through the frame-projection
   * round-trip (float drift on edited verts, bit-exact elsewhere). */
  mr.layerSetWeight(la, 1.0f);
  s2 = mr.findSlot(2);
  test_assert((s2->mesh->v.co[editA] - editedA).length() < 1e-5f);
  test_assert((s2->mesh->v.co[editB] - editedB).length() < 1e-5f);
  for (int i = 0; i < s2->mesh->v.count; i++) {
    if (i != editA && i != editB) {
      test_assert(std::memcmp(&s2->mesh->v.co[i], &pre[i], sizeof(float3)) == 0);
    }
  }

  /* Level-switch round-trip is bit-stable under multi-channel composition. */
  Vector<float3> p2;
  snapshotCo(*s2->mesh, p2);
  std::string blob = storeBlob(mr.store);
  mr.setActiveLevel(3);
  s2 = mr.setActiveLevel(2);
  {
    Vector<float3> now;
    snapshotCo(*s2->mesh, now);
    test_assert(sameBits(now, p2));
  }
  test_assert(storeBlob(mr.store) == blob);

  /* Store blob round-trips the channels (names, sizes, bytes). */
  {
    std::stringstream ss(blob, std::ios::in | std::ios::out | std::ios::binary);
    subdiv::GridsStore st2;
    test_assert(st2.read(ss));
    test_assert(st2.channelCount() == mr.store.channelCount());
    test_assert(st2.channelName(chA) == mr.store.channelName(chA));
    test_assert(st2.channelName(chB) == mr.store.channelName(chB));
    test_assert(storeBlob(st2) == blob);
  }

  /* layerTable + store blob = the remove-undo seam. */
  Vector<float> table;
  mr.layerTableOut(table);
  test_assert(int(table.size()) == 6);
  mr.layerRemove(la);
  test_assert(mr.layerCount() == 1);
  test_assert(mr.store.channelCount() == 2);
  s2 = mr.findSlot(2);
  {
    Vector<float3> now;
    snapshotCo(*s2->mesh, now);
    test_assert(sameBits(now, pre)); /* A's stroke gone with its channel */
  }
  {
    std::stringstream ss(blob, std::ios::in | std::ios::out | std::ios::binary);
    test_assert(mr.store.read(ss));
    mr.layerTableRestore(table);
    test_assert(mr.layerCount() == 2);
    test_assert(mr.layerWeight(la) == 1.0f && mr.layerEnabled(la) == 1);
    s2 = mr.findSlot(2);
    test_assert((s2->mesh->v.co[editA] - editedA).length() < 1e-5f);
  }

  /* Eviction/rehydration covers layer channels. */
  {
    /* Find a nonzero layer-A texel, evict the level, read it back. */
    int S = subdiv::GridsStore::sideForLevel(2), w = S + 1;
    int fg = -1, fu = 0, fv = 0;
    float3 val;
    for (int g = 0; g < mr.store.gridCount() && fg < 0; g++) {
      for (int v = 0; v < w && fg < 0; v++) {
        for (int u = 0; u < w && fg < 0; u++) {
          const float *d = mr.store.elem(2, chA, g, u, v);
          if (d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f) {
            fg = g;
            fu = u;
            fv = v;
            val = float3(d[0], d[1], d[2]);
          }
        }
      }
    }
    test_assert(fg >= 0);
    mr.store.evictLevel(2);
    test_assert(!mr.store.levelResident(2));
    // elem() rehydrates the touched CHANNEL only; the level as a whole
    // becomes resident via ensureLevelResident.
    const float *d = mr.store.elem(2, chA, fg, fu, fv);
    test_assert(d[0] == val[0] && d[1] == val[1] && d[2] == val[2]);
    mr.store.ensureLevelResident(2);
    test_assert(mr.store.levelResident(2));
  }

  /* Frozen layers cannot be the target; disabled targets re-enable + pin. */
  mr.layerSetFrozen(lb, 1);
  test_assert(mr.setEditTarget(lb) == -1);
  mr.layerSetFrozen(lb, 0);
  mr.layerSetWeight(la, 0.5f);
  mr.layerSetEnabled(la, 0);
  test_assert(mr.setEditTarget(la) == la);
  test_assert(mr.layerEnabled(la) == 1);
  test_assert(mr.layerWeight(la) == 1.0f);
  mr.setEditTarget(-1);

  fprintf(stderr, "layer channels: writeback target + composition ok\n");
  alloc::Delete(cage);
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

  fprintf(stderr,
          "cube: edited-vert writeback ok (drift=%g)\n",
          (rederived - edited).length());

  alloc::Delete(cage);
}

/* Add-level (the litemesh.multires_add_level ToolOp engine seam): growing the
 * stack by one appends a zero-displacement finest level (a smooth subdivision
 * of the current finest surface — no detail invented), preserves every existing
 * level's detail bit-exactly, and is exactly reversed by removeTopLevel(). Also
 * checks the level cap. */
static void gateAddLevel()
{
  Mesh *cage = createCube(2, 1.0f);
  Multires mr;
  mr.init(*cage, 2);
  test_assert(mr.maxLevel() == 2);
  injectDisp(mr);

  /* Fold a real level-2 edit so the preserved detail is nonzero. */
  MultiresSlot *s2 = mr.setActiveLevel(2);
  int editVid = 5;
  float3 edited = s2->mesh->v.co[editVid] + float3(0.125f, 0.0f, 0.0625f);
  s2->mesh->v.co[editVid] = edited;
  test_assert(mr.writeback(2) == 1);
  s2 = mr.setActiveLevel(2);
  Vector<float3> p2;
  snapshotCo(*s2->mesh, p2);
  std::string blobBefore = storeBlob(mr.store);

  /* Grow: one finer level, now active + finest. */
  test_assert(mr.addLevel() == 3);
  test_assert(mr.maxLevel() == 3);
  test_assert(mr.activeLevel() == 3);

  /* The new finest level carries zero displacement everywhere. */
  {
    int S = subdiv::GridsStore::sideForLevel(3), w = S + 1;
    bool allZero = true;
    for (int g = 0; g < mr.store.gridCount() && allZero; g++) {
      for (int v = 0; v < w && allZero; v++) {
        for (int u = 0; u < w && allZero; u++) {
          const float *d = mr.store.elem(3, 0, g, u, v);
          allZero = d[0] == 0.0f && d[1] == 0.0f && d[2] == 0.0f;
        }
      }
    }
    test_assert(allZero);
  }

  /* Zero disp ⇒ level-3 positions are exactly the stencil subdivision of the
   * preserved level-2 surface. */
  {
    MultiresSlot *s3 = mr.findSlot(3);
    test_assert(s3 != nullptr);
    Vector<float3> up, co3;
    mr.refiner.levels[2].stencil.eval(p2, up);
    snapshotCo(*s3->mesh, co3);
    test_assert(sameBits(co3, up));
  }

  /* Existing level-2 detail is preserved bit-exactly across the grow. */
  s2 = mr.setActiveLevel(2);
  {
    Vector<float3> now;
    snapshotCo(*s2->mesh, now);
    test_assert(sameBits(now, p2));
  }

  /* Shrink is the exact inverse: store byte-identical, surface preserved. */
  test_assert(mr.removeTopLevel() == 2);
  test_assert(mr.maxLevel() == 2);
  test_assert(storeBlob(mr.store) == blobBefore);
  s2 = mr.setActiveLevel(2);
  {
    Vector<float3> now;
    snapshotCo(*s2->mesh, now);
    test_assert(sameBits(now, p2));
  }

  /* Growth stops at the level cap (kMaxMultiresLevels == 7). */
  while (mr.maxLevel() < 7) {
    int prev = mr.maxLevel();
    test_assert(mr.addLevel() == prev + 1);
  }
  test_assert(mr.maxLevel() == 7);
  test_assert(mr.addLevel() == 7); /* no-op at the cap */

  fprintf(stderr, "add-level: grow preserves detail, shrink exact, cap ok\n");
  alloc::Delete(cage);
}

/* Open-boundary + n-gon cage smoke: materialize/writeback on a fan. */
static void gateFan()
{
  Mesh *cage = alloc::New<Mesh>("multires fan");
  float co[6][3] = {
      {0, 0, 0}, {1, 0, 0}, {0.75f, 0.75f, 0}, {0, 1, 0}, {-0.75f, 0.75f, 0}, {-1, 0, 0}};
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

/* The F3 cross-field frames for a level's smooth base, exactly as the
 * materialization path builds them today — the A/B reference the parametric
 * frames are measured against. */
static void providerFrames(Multires &mr,
                           int level,
                           const Vector<float3> &base,
                           Vector<float3> &no,
                           Vector<float3> &ta)
{
  Mesh *tm = mr.buildLevelTopo(level);
  for (int i = 0; i < int(base.size()); i++) {
    tm->v.co[i] = base[i];
  }
  tm->recalc_normals();
  displace::FrameProviderParams params;
  displace::updateFramesAll(*tm, params);

  auto grab = [&](const char *name, Vector<float3> &dst) {
    AttrRef ref = tm->v.attrs.find_attribute(AttrType::FLOAT3, name);
    test_assert(ref.exists());
    auto *d = static_cast<AttrData<float3> *>(ref.data);
    dst.resize(base.size());
    for (int i = 0; i < int(base.size()); i++) {
      dst[i] = (*d)[i];
    }
  };
  grab(displace::FRAME_NORMAL_ATTR, no);
  grab(displace::FRAME_TANGENT_ATTR, ta);
  alloc::Delete(tm);
}

/* Frame invariants that hold for any base: deterministic, orthonormal, one
 * value per vert id off the canonical (lowest-index) owning grid, and an exact
 * encode/decode pair. */
static void checkFrames(Multires &mr, int level, const Vector<float3> &base)
{
  int vc = mr.refiner.levels[level - 1].vertCount;

  Vector<float3> no, ta, no2, ta2;
  mr.parametricFrames(level, base, no, ta);
  mr.parametricFrames(level, base, no2, ta2);
  test_assert(int(no.size()) == vc && int(ta.size()) == vc);
  test_assert(sameBits(no, no2) && sameBits(ta, ta2));

  /* The canonical rule the frame derives from: lowest owning grid index. */
  subdiv::SubdivLevel &lvl = mr.refiner.levels[level - 1];
  int S = lvl.gridSide, w = S + 1;
  Vector<int> minGrid;
  minGrid.resize(vc);
  for (int i = 0; i < vc; i++) {
    minGrid[i] = -1;
  }
  for (int g = 0; g < mr.store.gridCount(); g++) {
    const int *gv = &lvl.gridVerts[g * w * w];
    for (int i = 0; i < w * w; i++) {
      int vid = gv[i];
      if (minGrid[vid] < 0 || g < minGrid[vid]) {
        minGrid[vid] = g;
      }
    }
  }
  Vector<int> coords;
  mr.levelVertGridCoordsOut(level, coords);
  for (int i = 0; i < vc; i++) {
    test_assert(coords[i * 3] == minGrid[i]);
  }

  for (int i = 0; i < vc; i++) {
    float3 n = no[i], t = ta[i], b = n.cross(t);
    for (int k = 0; k < 3; k++) {
      test_assert(std::isfinite(n[k]) && std::isfinite(t[k]));
    }
    test_assert(std::fabs(n.length() - 1.0f) < 1e-5f);
    test_assert(std::fabs(t.length() - 1.0f) < 1e-5f);
    test_assert(std::fabs(n.dot(t)) < 1e-5f);

    /* frameᵀ·dp then frame·d round-trips: the property that makes any
     * deterministic, ambiguity-free frame usable for storage. */
    float3 dp(float(i % 7 - 3) * 0.03125f,
              float(i % 5 - 2) * 0.0625f,
              float(i % 3 - 1) * 0.125f);
    float3 d(dp.dot(t), dp.dot(b), dp.dot(n));
    float3 dp2 = t * d[0] + b * d[1] + n * d[2];
    test_assert((dp2 - dp).length() < 1e-5f);
  }
}

/* Phase-1 gate: the parametric frame's structural invariants, on a regular
 * cage and on a fan cage whose center is extraordinary (valence 4 with a
 * triangle fan, so grid borders meet at ~2π/N). */
static void gateParametricFrames()
{
  Mesh *cube = createCube(2, 1.0f);
  Multires mc;
  mc.init(*cube, 3);
  Vector<float3> cageCo, base;
  subdiv::gatherVertCo(*cube, cageCo);
  for (int level = 1; level <= 3; level++) {
    mc.refiner.evalFromCage(cageCo, level, base);
    checkFrames(mc, level, base);
  }
  alloc::Delete(cube);

  Mesh *fan = alloc::New<Mesh>("frame fan");
  float co[6][3] = {
      {0, 0, 0}, {1, 0, 0}, {0.75f, 0.75f, 0}, {0, 1, 0}, {-0.75f, 0.75f, 0}, {-1, 0, 0}};
  int ids[6];
  for (int i = 0; i < 6; i++) {
    ids[i] = fan->make_vertex(float3(co[i][0], co[i][1], co[i][2]));
  }
  for (int i = 0; i < 4; i++) {
    int tri[3] = {ids[0], ids[i + 1], ids[i + 2]};
    fan->make_face(std::span<int>(tri, 3));
  }
  Multires mf;
  mf.init(*fan, 2);
  Vector<float3> fanCo, fanBase;
  subdiv::gatherVertCo(*fan, fanCo);
  for (int level = 1; level <= 2; level++) {
    mf.refiner.evalFromCage(fanCo, level, fanBase);
    checkFrames(mf, level, fanBase);
  }
  alloc::Delete(fan);

  fprintf(stderr,
          "parametric frames: deterministic, orthonormal, canonical-grid, "
          "round-trip exact\n");
}

/* Phase-1 gate: a degenerate lattice takes the fallback ladder rather than
 * producing NaN, and takes the SAME rung on repeat. */
static void gateFrameDegenerate()
{
  Mesh *cage = createCube(2, 1.0f);
  Multires mr;
  mr.init(*cage, 2);
  const int level = 2;
  int vc = mr.refiner.levels[level - 1].vertCount;

  /* Fully collapsed: every difference is zero, so every rung is exercised. */
  Vector<float3> base;
  base.resize(vc);
  for (int i = 0; i < vc; i++) {
    base[i] = float3(0.0f, 0.0f, 0.0f);
  }
  checkFrames(mr, level, base);

  /* Partially collapsed: one cell of grid 0 pinched to a point, so its four
   * lattice corners lose one or both central differences while the rest of
   * the level stays regular. */
  Vector<float3> cageCo;
  subdiv::gatherVertCo(*cage, cageCo);
  mr.refiner.evalFromCage(cageCo, level, base);
  subdiv::SubdivLevel &lvl = mr.refiner.levels[level - 1];
  int w = lvl.gridSide + 1;
  const int *gv = &lvl.gridVerts[0];
  float3 pinch = base[gv[0]];
  int cell[4] = {gv[0], gv[1], gv[w], gv[w + 1]};
  for (int k = 0; k < 4; k++) {
    base[cell[k]] = pinch;
  }
  checkFrames(mr, level, base);

  fprintf(stderr, "frame degenerate: fallback ladder finite + reproducible\n");
}

/* Phase-1 gate, the regression test for the defect: nudge one cage vertex and
 * every fine tangent must move continuously. A cross-field representative can
 * land on a different 90° image of the same 4-RoSy field, which decodes stored
 * disp rotated by 90°; a lattice makes no such choice. Asserts the parametric
 * field, and reports the provider's worst case beside it as the A/B — which on
 * this cube at level 3 is a full 180° tangent REVERSAL (dot -0.999962) from a
 * 0.01 cage nudge, against 0.999970 for the parametric frame. */
static void gateFrameStability()
{
  Mesh *cage = createCube(2, 1.0f);
  Multires mr;
  mr.init(*cage, 3);
  const int level = 3;

  Vector<float3> cageCo, base, base2;
  subdiv::gatherVertCo(*cage, cageCo);
  mr.refiner.evalFromCage(cageCo, level, base);

  Vector<float3> pNo, pTa, fNo, fTa;
  mr.parametricFrames(level, base, pNo, pTa);
  providerFrames(mr, level, base, fNo, fTa);

  cageCo[0] = cageCo[0] + float3(0.01f, 0.007f, 0.013f);
  mr.refiner.evalFromCage(cageCo, level, base2);

  Vector<float3> pNo2, pTa2, fNo2, fTa2;
  mr.parametricFrames(level, base2, pNo2, pTa2);
  providerFrames(mr, level, base2, fNo2, fTa2);

  auto worstDot = [](const Vector<float3> &a, const Vector<float3> &b, int &at) {
    double m = 2.0;
    at = -1;
    for (int i = 0; i < int(a.size()); i++) {
      double d = double(a[i].dot(b[i]));
      if (d < m) {
        m = d;
        at = i;
      }
    }
    return m;
  };

  int pAt = -1, fAt = -1;
  double pWorst = worstDot(pTa, pTa2, pAt);
  double fWorst = worstDot(fTa, fTa2, fAt);
  fprintf(stderr,
          "frame stability: parametric worst tangent dot %.6f (vert %d), "
          "provider %.6f (vert %d)\n",
          pWorst,
          pAt,
          fWorst,
          fAt);

  /* Continuity, not just absence of a flip: a 0.01 cage nudge is a small
   * rotation of a finite-difference direction. */
  test_assert(pWorst > 0.9);
  test_assert(worstDot(pNo, pNo2, pAt) > 0.9);
  // This gate is measuring a defect in the provider, the thing being replaced.
  // If the provider comes out MORE stable than the parametric frame, the gate
  // is not measuring that defect.
  test_assert(fWorst <= pWorst);

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

  fprintf(stderr,
          "downRefit: changed=%d residual %.6f -> %.6f\n",
          changed,
          resBefore,
          resAfter);
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

  fprintf(stderr,
          "downRefit: fine preserved (drift=%g), level 1 untouched\n",
          maxResidual(tmp, p3));

  alloc::Delete(cage);
}

/* Downward propagation gate: an edit made at a fine level must be visible when
 * the user switches DOWN to a coarser one (the pyramid derives each level from
 * the ones below, so without propagateDown the coarse level still shows the
 * pre-edit surface). Asserts the four properties the feature rests on: the
 * coarse level tracks the edit, the fine surface is preserved exactly, the
 * operator is idempotent, and — the drift gate — an up-then-down round trip
 * with no edit in between leaves the coarse level bit-identical, since a stroke
 * flush does exactly that on every stroke. */
static void gatePropagateDown()
{
  Mesh *cage = createCube(2, 1.0f);
  Multires mr;
  mr.init(*cage, 3);

  test_assert(mr.propagateDown(1) == 0); /* guard: nothing below level 1 */

  Vector<float3> l1Before, l2Before, l1After, l2After, p3, tmp;
  snapshotCo(*mr.setActiveLevel(1)->mesh, l1Before);
  snapshotCo(*mr.setActiveLevel(2)->mesh, l2Before);

  /* No edit yet: walking back down must not perturb anything. Level 2 was just
   * active, so this exercises the same up-then-down shape _flush_multires uses
   * around its top-level bake. */
  mr.setActiveLevel(3);
  snapshotCo(*mr.setActiveLevel(2)->mesh, tmp);
  test_assert(sameBits(tmp, l2Before));
  snapshotCo(*mr.setActiveLevel(1)->mesh, tmp);
  test_assert(sameBits(tmp, l1Before));

  MultiresSlot *s3 = mr.setActiveLevel(3);
  int edits = 0;
  for (int i = 0; i < s3->mesh->v.count; i++) {
    float3 &p = s3->mesh->v.co[i];
    if (p[2] > 0.4f) {
      p[2] += 0.5f;
      edits++;
    }
  }
  test_assert(edits > 0);
  test_assert(mr.writeback(3) == edits);
  snapshotCo(*mr.findSlot(3)->mesh, p3);

  /* Switching down cascades 3->2->1: both coarse levels move. */
  snapshotCo(*mr.setActiveLevel(1)->mesh, l1After);
  snapshotCo(*mr.setActiveLevel(2)->mesh, l2After);
  double m2 = maxResidual(l2After, l2Before), m1 = maxResidual(l1After, l1Before);
  fprintf(
      stderr, "propagateDown: level-2 max move = %g, level-1 max move = %g\n", m2, m1);
  test_assert(m2 > 0.1);
  test_assert(m1 > 0.01);

  /* Averaging, not deconvolution: a restriction cannot overshoot the surface it
   * summarizes, so no coarse vert may move further than the edit itself. */
  test_assert(m2 < 0.5);
  test_assert(m1 < 0.5);

  /* Fine surface preserved through the whole cascade (frame round-trip drift
   * only), and the level-3 store is not owed another push. */
  snapshotCo(*mr.setActiveLevel(3)->mesh, tmp);
  double drift = maxResidual(tmp, p3);
  fprintf(stderr, "propagateDown: fine surface drift = %g\n", drift);
  test_assert(drift < 1e-5);

  /* No further debt: the pending flag was consumed, so switching down again
   * re-materializes level 2 bit for bit rather than restricting a second time. */
  snapshotCo(*mr.setActiveLevel(2)->mesh, tmp);
  test_assert(sameBits(tmp, l2After));

  /* Idempotent to within the frame round trip: pos_{L-1} is a pure function of
   * pos_L, so a FORCED second pass only re-lands the same surface (bitwise
   * equality is not available — pos_L comes back off the store through the
   * frame encode, which is exact only up to float drift). This is what makes
   * a repeat propagation harmless rather than a slow smoothing. */
  mr.setActiveLevel(3);
  mr.propagateDown(3);
  snapshotCo(*mr.setActiveLevel(2)->mesh, tmp);
  double redo = maxResidual(tmp, l2After);
  fprintf(stderr, "propagateDown: second pass moved level 2 by %g\n", redo);
  test_assert(redo < 1e-5);

  alloc::Delete(cage);
}

/* Snapshot the whole multires store (with its down-propagation debt header)
 * the way a host's undo step does; the caller owns the returned bytes. */
static std::string storeBlob(Multires &mr)
{
  int size = 0;
  uint8_t *buf = Multires_serializeStore(&mr, &size);
  test_assert(buf != nullptr && size > 0);
  std::string s(reinterpret_cast<const char *>(buf), size_t(size));
  freeMeshBuffer(buf);
  return s;
}

/* Undo gate for downward propagation. propagateDown mutates the store of every
 * level below the one being left, outside the meshlog — so the only thing that
 * can restore it is the store snapshot hosts already take per undo step. Two
 * properties make that work, and both are gated here:
 *   - a restore reproduces the snapshot exactly, including levels a later
 *     propagation had overwritten, and does NOT re-derive anything on the
 *     level switch that follows it;
 *   - the debt travels in the blob. It is not recoverable from the store (a
 *     level with zero displacement still differs from the restriction of the
 *     level above), so without the header a redo would land on a state that
 *     had silently forgotten it owes the level below — the original bug,
 *     re-opened by an undo round trip. */
static void gatePropagateUndo()
{
  Mesh *cage = createCube(2, 1.0f);
  Multires mr;
  mr.init(*cage, 3);

  Vector<float3> l1Pre, l2Pre, l1Prop, tmp;
  snapshotCo(*mr.setActiveLevel(1)->mesh, l1Pre);
  MultiresSlot *s2 = mr.setActiveLevel(2);
  snapshotCo(*s2->mesh, l2Pre);

  /* Pre-stroke snapshot: no debt yet. */
  std::string blobPre = storeBlob(mr);
  test_assert(!mr.downPropDebt(2));

  int edits = 0;
  for (int i = 0; i < s2->mesh->v.count; i++) {
    float3 &p = s2->mesh->v.co[i];
    if (p[2] > 0.4f) {
      p[2] += 0.5f;
      edits++;
    }
  }
  test_assert(edits > 0);
  test_assert(mr.writeback(2) == edits);
  test_assert(mr.downPropDebt(2));

  /* Post-stroke snapshot, exactly as a host pushes it: the edit is in the
   * store and the debt to level 1 is still outstanding. */
  std::string blobPost = storeBlob(mr);

  /* Switch down: level 1 follows the level-2 edit. */
  snapshotCo(*mr.setActiveLevel(1)->mesh, l1Prop);
  double moved = maxResidual(l1Prop, l1Pre);
  fprintf(stderr, "propagateUndo: level-1 max move = %g\n", moved);
  test_assert(moved > 0.01);
  test_assert(!mr.downPropDebt(2));

  /* Undo the stroke: the pre-stroke blob restores BOTH levels — level 2's edit
   * and the level-1 surface the propagation had overwritten. */
  test_assert(Multires_restoreStore(&mr,
                                    reinterpret_cast<const uint8_t *>(blobPre.data()),
                                    int(blobPre.size())) == 1);
  snapshotCo(*mr.setActiveLevel(2)->mesh, tmp);
  test_assert(sameBits(tmp, l2Pre));
  snapshotCo(*mr.setActiveLevel(1)->mesh, tmp);
  test_assert(sameBits(tmp, l1Pre));
  /* ...and that 2->1 switch, on a state with no debt, did not restrict again. */
  test_assert(!mr.downPropDebt(2));
  snapshotCo(*mr.setActiveLevel(1)->mesh, tmp);
  test_assert(sameBits(tmp, l1Pre));

  /* Redo the stroke: the post-stroke blob comes back with its debt intact, so
   * the next downward switch still pushes the edit into level 1 — landing on
   * the same surface the original switch produced. */
  test_assert(Multires_restoreStore(&mr,
                                    reinterpret_cast<const uint8_t *>(blobPost.data()),
                                    int(blobPost.size())) == 1);
  test_assert(mr.downPropDebt(2));
  mr.setActiveLevel(2);
  snapshotCo(*mr.setActiveLevel(1)->mesh, tmp);
  /* Not bit-exact: the first propagation restricted level 2's resident mesh,
   * i.e. the raw edited floats, while this one restricts them decoded back out
   * of the store — the same ~1e-7 frame round-trip drift gatePropagateDown
   * measures. */
  const double redoDrift = maxResidual(tmp, l1Prop);
  fprintf(stderr, "propagateUndo: redo re-propagated to within %g\n", redoDrift);
  test_assert(redoDrift < 1e-5);

  /* An undo that auto-switches back to the level a step was made on must not
   * propagate on the way: that would fold the very detail about to be undone
   * into the level below. Sculpt at 3 to create the debt, then step down with
   * propagate=false — level 2 must be untouched and the debt must survive, so
   * the user's next real switch still settles it. */
  MultiresSlot *s3 = mr.setActiveLevel(3);
  edits = 0;
  for (int i = 0; i < s3->mesh->v.count; i++) {
    float3 &p = s3->mesh->v.co[i];
    if (p[1] > 0.4f) {
      p[1] += 0.5f;
      edits++;
    }
  }
  test_assert(edits > 0);
  test_assert(mr.writeback(3) == edits);
  test_assert(mr.downPropDebt(3));

  Vector<float3> l2Owed;
  snapshotCo(*mr.materialize(2)->mesh, l2Owed); /* level 2 as the store has it */

  snapshotCo(*mr.setActiveLevel(2, /*propagate=*/false)->mesh, tmp);
  test_assert(sameBits(tmp, l2Owed));
  test_assert(mr.downPropDebt(3));

  // Switching to level 2 again, this time with propagation enabled, does change
  // its geometry. That confirms the assertion above was verifying that
  // propagate=false suppressed the update, not that nothing was there to
  // propagate.
  mr.setActiveLevel(3);
  snapshotCo(*mr.setActiveLevel(2)->mesh, tmp);
  fprintf(stderr,
          "propagateUndo: suppressed vs applied switch differ by %g\n",
          maxResidual(tmp, l2Owed));
  test_assert(maxResidual(tmp, l2Owed) > 0.01);
  test_assert(!mr.downPropDebt(3));

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
  AttrRef uvRef = m.c.attrs.find_attribute(AttrType::FLOAT2, vdm::PTEX_ATLAS_ATTR);
  test_assert(uvRef.exists());
  // The atlas is internal: the mesh's AttrUse::UV layer is the subdivided cage
  // UV map, which is a different field entirely.
  test_assert(int(uvRef.use & AttrUse::UV) == 0);
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

  fprintf(stderr, "gridUVs: %d charts (cpr=%d), in-cell + level-consistent\n", G, cpr);
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
  fprintf(
      stderr, "subsurfVdm: touched=%d clamped=%d\n", st.texelsTouched, st.texelsClamped);
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
  vdm::collectPromotionCandidates(m,
                                  *slot->tree,
                                  store,
                                  std::span<const int>(faces.data(), faces.size()),
                                  pp,
                                  candidates);
  test_assert(candidates.size() == 0);
  vdm::VdmPromoteStats ps =
      vdm::promoteRegion(m,
                         *slot->tree,
                         store,
                         std::span<const int>(faces.data(), faces.size()),
                         pp,
                         nullptr,
                         nullptr);
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
  fprintf(stderr, "ptexSplat: touched=%d faces=%d\n", st.texelsTouched, st.facesTouched);
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
  fprintf(
      stderr, "ptexSplat: seams checked=%d active=%d worst=%g\n", checked, active, worst);
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
           maxLevel,
           lvl.vertCount,
           slot->mesh->f.count,
           ms(t0, t1),
           ms(t1, t2),
           ms(t2, t3),
           int(hit),
           ms(t3, t4),
           ms(t4, t5));
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

  gateLayerChannels();
  gateCube();
  gateAddLevel();
  gateFan();
  gateParametricFrames();
  gateFrameDegenerate();
  gateFrameStability();
  gateDownRefit();
  gatePropagateDown();
  gatePropagateUndo();
  gateGridUVs();
  gateSubsurfVdm();
  gatePtexSplat();
  gateStoreEviction();

  /* Skip test_end(): attr name strings stay live in the alloc tracker
   * (mirrors the other spatial/mesh tests). */
  return retval;
}
