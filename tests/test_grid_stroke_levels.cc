/* Level seams: the zero-displacement posIsBase hazard, seedLevelPositions and
 * GpuNormalTopology::buildFromArrays. */

#include "test_grid_stroke_common.h"

void gridStrokeLevels(GridStrokeFixture &fx)
{
  Multires &mr = fx.mr;
  const std::string &s0 = fx.s0;
  Mesh *cage = fx.cage;

  /* Zero-disp level (posIsBase hazard): the first grids edit on a level with
   * no displacement must not poison the lazily-materialized base — the
   * writeback must land real deltas and survive a rematerialization. */
  {
    Mesh *cage2 = createCube(2, 1.0f);
    Multires mr2;
    mr2.init(*cage2, 3); // zero displacement everywhere
    Brush brush;
    setupBrush(brush, 0.25f, 0.5f);
    GridLevelDomain *d = mr2.gridDomain(kLevel);
    GridBrushExecutor ex(d, &brush, nullptr);
    ex.beginStep();
    int moved = ex.applyDab(SculptBrushes::DRAW, float3(0, 0, 0.5f), float3(0, 0, 1));
    ex.endStep();
    TASSERT(moved > 0);
    Vector<float3> post;
    post.resize(d->vertCount());
    for (int v = 0; v < d->vertCount(); v++) {
      post[v] = d->pos()[v];
    }
    // Re-derive the level from cage + store: the edit must survive.
    mr2.invalidateAll();
    Vector<float3> &re = mr2.levelPositions(kLevel);
    float maxd = 0.0f;
    for (int v = 0; v < int(post.size()); v++) {
      for (int k = 0; k < 3; k++) {
        float dd = std::fabs(post[v][k] - re[v][k]);
        maxd = dd > maxd ? dd : maxd;
      }
    }
    fprintf(stderr, "zero-disp round trip: max diff %.8f\n", maxd);
    TASSERT(maxd <= 1e-5f);
  }

  /* seedLevelPositions (W4, the fast mode-enter seam): seeding through the
   * chain must reproduce fromLevelPositions' surface at the seeded level,
   * and the deferred down-prop debt must yield the same coarser surface once
   * a downward switch settles it. */
  {
    // Reference: the materializing path on a fresh stack.
    Mesh *cageA = createCube(2, 1.0f);
    Multires mrA;
    mrA.init(*cageA, 3);
    // A deterministic displaced top-level sample field.
    subdiv::SubdivLevel &lvlA = mrA.refiner.levels[kLevel - 1];
    Vector<float3> base = mrA.levelPositions(kLevel);
    int sampleNum = int(lvlA.gridVerts.size());
    Vector<float> samples;
    samples.resize(size_t(sampleNum) * 3);
    for (int i = 0; i < sampleNum; i++) {
      const float3 &p = base[lvlA.gridVerts[i]];
      samples[size_t(i) * 3 + 0] = p[0] + 0.03f * std::sin(3.0f * p[1]);
      samples[size_t(i) * 3 + 1] = p[1];
      samples[size_t(i) * 3 + 2] = p[2] + 0.04f * std::cos(2.0f * p[0]);
    }
    // Reference path: scatter into the materialized mesh + writeback +
    // cascade (what Multires_fromLevelPositions does).
    MultiresSlot *slotA = mrA.setActiveLevel(kLevel);
    for (int i = 0; i < sampleNum; i++) {
      int vid = lvlA.gridVerts[i];
      slotA->mesh->v.co[vid] = float3(
          samples[size_t(i) * 3], samples[size_t(i) * 3 + 1], samples[size_t(i) * 3 + 2]);
    }
    mrA.writeback(kLevel);
    for (int l = kLevel; l >= 2; l--) {
      mrA.propagateDown(l);
    }
    mrA.invalidateAbove(kLevel - 1);
    mrA.setActiveLevel(kLevel);

    // Seeding path on an identical fresh stack.
    Mesh *cageB = createCube(2, 1.0f);
    Multires mrB;
    mrB.init(*cageB, 3);
    int got = mrB.seedLevelPositions(
        kLevel, reinterpret_cast<const float (*)[3]>(samples.data()), sampleNum);
    TASSERT(got == sampleNum);
    TASSERT(mrB.downPropDebt(kLevel));

    Vector<float3> &posA = mrA.levelPositions(kLevel);
    Vector<float3> &posB = mrB.levelPositions(kLevel);
    float maxd = 0.0f;
    for (int v = 0; v < int(posB.size()); v++) {
      for (int k = 0; k < 3; k++) {
        float dd = std::fabs(posA[v][k] - posB[v][k]);
        maxd = dd > maxd ? dd : maxd;
      }
    }
    fprintf(stderr, "seed vs materialize path: top max diff %.8f\n", maxd);
    TASSERT(maxd <= 1e-5f);

    // Settle the debt (a downward switch) and compare the coarse surface the
    // reference path cascaded eagerly.
    mrB.setActiveLevel(kLevel);
    mrB.setActiveLevel(kLevel - 1);
    mrA.setActiveLevel(kLevel - 1);
    Vector<float3> &cA = mrA.levelPositions(kLevel - 1);
    Vector<float3> &cB = mrB.levelPositions(kLevel - 1);
    maxd = 0.0f;
    for (int v = 0; v < int(cB.size()); v++) {
      for (int k = 0; k < 3; k++) {
        float dd = std::fabs(cA[v][k] - cB[v][k]);
        maxd = dd > maxd ? dd : maxd;
      }
    }
    fprintf(stderr, "seed vs materialize path: settled L-1 max diff %.8f\n", maxd);
    TASSERT(maxd <= 1e-5f);
  }

  /* GpuNormalTopology::buildFromArrays (G4): the grids entry fed by
   * levelTriIndicesOut must emit tables identical to build() on the
   * materialized level mesh (same fan order by construction). */
  {
    restoreStore(mr, s0);
    MultiresSlot *slot = mr.setActiveLevel(kLevel);
    TASSERT(slot && slot->mesh);
    GpuNormalTopology a;
    a.build(*slot->mesh);
    Vector<int> tris;
    mr.levelTriIndicesOut(kLevel, tris);
    Vector<uint32_t> trisU;
    trisU.resize(tris.size());
    for (int i = 0; i < int(tris.size()); i++) {
      trisU[i] = uint32_t(tris[i]);
    }
    GpuNormalTopology b;
    b.buildFromArrays(trisU.data(), int(tris.size() / 3), slot->mesh->v.count);
    TASSERT(a.triCount == b.triCount);
    TASSERT(a.vcount == b.vcount);
    bool same = a.triVerts.size() == b.triVerts.size() &&
                a.meta.size() == b.meta.size() && a.list.size() == b.list.size();
    for (size_t i = 0; same && i < a.triVerts.size(); i++) {
      same = a.triVerts[int(i)] == b.triVerts[int(i)];
    }
    for (size_t i = 0; same && i < a.meta.size(); i++) {
      same = a.meta[int(i)] == b.meta[int(i)];
    }
    for (size_t i = 0; same && i < a.list.size(); i++) {
      same = a.list[int(i)] == b.list[int(i)];
    }
    fprintf(stderr,
            "normal topology arrays parity: tris=%d same=%d\n",
            a.triCount,
            int(same));
    TASSERT(same);
  }
}
