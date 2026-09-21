/* The c-api session and the batch-program entry points (S4 grids, S5 mesh)
 * against their per-dab equivalents. */

#include "test_grid_stroke_common.h"

void gridStrokeBatch(GridStrokeFixture &fx)
{
  Multires &mr = fx.mr;
  const std::string &s0 = fx.s0;
  const DabBattery &dabs = fx.dabs;

  /* c-api session smoke: supported-tool dispatch, a stroke through the
   * session, undo/redo, and the domain raycast. */
  {
    Brush brush;
    setupBrush(brush, 0.25f, 0.5f);
    restoreStore(mr, s0);

    TASSERT(GridStroke_supported(&mr, int(SculptBrushes::DRAW)) == 1);
    TASSERT(GridStroke_supported(&mr, int(SculptBrushes::BSMOOTH)) == 1);

    float out10[10];
    int nearest = -1;
    int hit = GridTree_castRay(
        &mr, kLevel, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f, -1.0f, out10, &nearest);
    TASSERT(hit == 1);
    TASSERT(nearest >= 0);
    TASSERT(out10[6] > 0.0f);

    GridStrokeSession *s = GridStroke_new(&mr, kLevel, &brush);
    TASSERT(s != nullptr);
    GridLevelDomain *d = mr.gridDomain(kLevel);
    Vector<float3> pre;
    pre.resize(d->vertCount());
    for (int v = 0; v < d->vertCount(); v++) {
      pre[v] = d->pos()[v];
    }
    TASSERT(GridStroke_begin(s) == 1);
    int moved = GridStroke_dab(s, int(SculptBrushes::DRAW), 0, 0, 0.5f, 0, 0, 1, 0);
    GridStroke_end(s);
    TASSERT(moved > 0);
    TASSERT(GridStroke_undoBytes(s) > 0.0);
    TASSERT(GridStroke_undo(s) == 1);
    Vector<float3> cur;
    cur.resize(d->vertCount());
    for (int v = 0; v < d->vertCount(); v++) {
      cur[v] = d->pos()[v];
    }
    TASSERT(samePosBits(cur, pre));
    TASSERT(GridStroke_redo(s) == 1);
    GridStroke_free(s);
  }

  /* S4 batch-program equivalence: GridStroke_dabBatchProgram must reproduce
   * the host loop it replaces — per-dab prop rewrite + device refill +
   * GridStroke_dabProgram per symmetry image — bit-exact, pressure dynamics
   * and one mirror image included. */
  {
    Brush brush;
    setupBrush(brush, 0.3f, 0.5f);
    brush.addPropDynamicByName(util::string("strength"),
                               int(props::DeviceType::PRESSURE),
                               int(BasicMix::MULTIPLY),
                               1.0f);

    BrushProgram prog;
    prog.addCommand(int(SculptBrushes::DRAW));
    int smoothIdx = prog.addCommand(int(SculptBrushes::BSMOOTH));
    prog.setCommandFloatByName(smoothIdx, util::string("strength"), 0.25f);

    const float kStrength = 0.5f, kPressure = 0.7f;
    const int n = int(dabs.origins.size());
    Vector<float> flat;
    for (int i = 0; i < n; i++) {
      for (int k = 0; k < 3; k++) {
        flat.append(dabs.origins[i][k]);
      }
      for (int k = 0; k < 3; k++) {
        flat.append(dabs.normals[i][k]);
      }
      flat.append(0.3f);
    }
    const float signs[3] = {-1.0f, 1.0f, 1.0f};

    restoreStore(mr, s0);
    GridStrokeSession *sa = GridStroke_new(&mr, kLevel, &brush);
    TASSERT(sa != nullptr);
    TASSERT(GridStroke_begin(sa) == 1);
    int movedA = GridStroke_dabBatchProgram(
        sa, &prog, n, flat.data(), kStrength, 0, kPressure, 1, signs, 1);
    GridStroke_end(sa);
    GridLevelDomain *d = mr.gridDomain(kLevel);
    Vector<float3> posA;
    posA.resize(d->vertCount());
    for (int v = 0; v < d->vertCount(); v++) {
      posA[v] = d->pos()[v];
    }
    GridStroke_free(sa);

    restoreStore(mr, s0);
    GridStrokeSession *sb = GridStroke_new(&mr, kLevel, &brush);
    TASSERT(sb != nullptr);
    TASSERT(GridStroke_begin(sb) == 1);
    int movedB = 0;
    for (int i = 0; i < n; i++) {
      const float *dd = flat.data() + i * 7;
      brush.strength = kStrength;
      brush.radius = dd[6];
      brush.invert = false;
      brush.writeProps();
      brush.clearDeviceInputs();
      brush.pushDeviceInput(int(props::DeviceType::PRESSURE), kPressure);
      movedB +=
          GridStroke_dabProgram(sb, &prog, dd[0], dd[1], dd[2], dd[3], dd[4], dd[5]);
      movedB += GridStroke_dabProgram(sb,
                                      &prog,
                                      dd[0] * signs[0],
                                      dd[1] * signs[1],
                                      dd[2] * signs[2],
                                      dd[3] * signs[0],
                                      dd[4] * signs[1],
                                      dd[5] * signs[2]);
    }
    GridStroke_end(sb);
    d = mr.gridDomain(kLevel);
    Vector<float3> posB;
    posB.resize(d->vertCount());
    for (int v = 0; v < d->vertCount(); v++) {
      posB[v] = d->pos()[v];
    }
    GridStroke_free(sb);

    fprintf(stderr, "S4 batch-program: moved %d (loop %d)\n", movedA, movedB);
    TASSERT(movedA > 0);
    TASSERT(movedA == movedB);
    TASSERT(samePosBits(posA, posB));
  }

  /* S5 mesh batch-program equivalence: MeshStroke_dabBatchProgram must
   * reproduce the host loop it replaces (apply_dab_program's filterNodes /
   * execProgram / clearIsFirstOfStep / updateQueries cycle plus the batch
   * loop's per-dab prop rewrite + device refill) bit-exact on the
   * materialized-mesh path, pressure and one mirror image included. */
  {
    Brush brush;
    setupBrush(brush, 0.3f, 0.5f);
    brush.addPropDynamicByName(util::string("strength"),
                               int(props::DeviceType::PRESSURE),
                               int(BasicMix::MULTIPLY),
                               1.0f);

    BrushProgram prog;
    prog.addCommand(int(SculptBrushes::DRAW));
    int smoothIdx = prog.addCommand(int(SculptBrushes::BSMOOTH));
    prog.setCommandFloatByName(smoothIdx, util::string("strength"), 0.25f);

    const float kStrength = 0.5f, kPressure = 0.7f;
    const int n = int(dabs.origins.size());
    Vector<float> flat;
    for (int i = 0; i < n; i++) {
      for (int k = 0; k < 3; k++) {
        flat.append(dabs.origins[i][k]);
      }
      for (int k = 0; k < 3; k++) {
        flat.append(dabs.normals[i][k]);
      }
      flat.append(0.3f);
    }
    const float signs[3] = {-1.0f, 1.0f, 1.0f};

    restoreStore(mr, s0);
    MultiresSlot *slot = mr.setActiveLevel(kLevel);
    TASSERT(slot && slot->mesh && slot->tree);
    Vector<float3> posA, posB;
    int totalA, totalB = 0;
    {
      CommandExecutor ex(slot->tree, &brush);
      ex.setStrokeGen(1);
      ex.beginStep(false);
      totalA = MeshStroke_dabBatchProgram(&ex,
                                          slot->tree,
                                          slot->mesh,
                                          &brush,
                                          &prog,
                                          n,
                                          flat.data(),
                                          kStrength,
                                          0,
                                          kPressure,
                                          1,
                                          1.0f,
                                          signs,
                                          1);
      ex.endStep();
      posA.resize(slot->mesh->v.count);
      for (int v = 0; v < slot->mesh->v.count; v++) {
        posA[v] = slot->mesh->v.co[v];
      }
    }

    restoreStore(mr, s0);
    slot = mr.setActiveLevel(kLevel);
    TASSERT(slot && slot->mesh && slot->tree);
    {
      CommandExecutor ex(slot->tree, &brush);
      ex.setStrokeGen(1);
      ex.beginStep(false);
      auto oneImage = [&](const float3 &center, const float3 &normal, float radius) {
        Vector<spatial::SpatialNode *> nodes;
        if (!slot->tree->filterNodes(center, radius, nodes)) {
          return 0;
        }
        ex.setGrabAccumAdd(false);
        ex.execProgram(&prog, &nodes, center, normal);
        ex.clearIsFirstOfStep();
        int count = int(nodes.size());
        slot->tree->updateQueries();
        return count;
      };
      for (int i = 0; i < n; i++) {
        const float *dd = flat.data() + i * 7;
        brush.strength = kStrength;
        brush.radius = dd[6];
        brush.invert = false;
        brush.writeProps();
        brush.clearDeviceInputs();
        brush.pushDeviceInput(int(props::DeviceType::PRESSURE), kPressure);
        totalB +=
            oneImage(float3(dd[0], dd[1], dd[2]), float3(dd[3], dd[4], dd[5]), dd[6]);
        totalB += oneImage(float3(dd[0] * signs[0], dd[1] * signs[1], dd[2] * signs[2]),
                           float3(dd[3] * signs[0], dd[4] * signs[1], dd[5] * signs[2]),
                           dd[6]);
      }
      ex.endStep();
      posB.resize(slot->mesh->v.count);
      for (int v = 0; v < slot->mesh->v.count; v++) {
        posB[v] = slot->mesh->v.co[v];
      }
    }
    restoreStore(mr, s0);

    fprintf(stderr, "S5 mesh batch-program: nodes %d (loop %d)\n", totalA, totalB);
    TASSERT(totalA > 0);
    TASSERT(totalA == totalB);
    TASSERT(samePosBits(posA, posB));
  }
}
