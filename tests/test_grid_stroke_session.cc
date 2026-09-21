/* Anchored grab, grids/mesh-path interleaving, domain generation and the
 * writeback authority of a host-mirrorless fold. */

#include "test_grid_stroke_common.h"

void gridStrokeSession(GridStrokeFixture &fx)
{
  Multires &mr = fx.mr;
  const std::string &s0 = fx.s0;

  /* Grab (anchored, from-orig) functional gate: moves verts, undoes clean. */
  {
    Brush brush;
    setupBrush(brush, 0.3f, 1.0f);
    restoreStore(mr, s0);
    GridLevelDomain *d = mr.gridDomain(kLevel);
    GridStrokeLog log;
    GridBrushExecutor ex(d, &brush, &log);

    Vector<float3> pre;
    pre.resize(d->vertCount());
    for (int v = 0; v < d->vertCount(); v++) {
      pre[v] = d->pos()[v];
    }
    const std::string blobPre = storeBlob(mr.store);

    ex.beginStep();
    for (int i = 0; i < 4; i++) {
      brush.grabTo = float3(0.0f, 0.0f, 0.05f * float(i + 1));
      ex.setGrabAccumAdd(false);
      ex.applyDab(SculptBrushes::GRAB, float3(0, 0, 0.5f), float3(0, 0, 1));
    }
    ex.endStep();

    int movedVerts = int(ex.strokeTouchedVerts().size());
    fprintf(stderr, "grab stroke: %d verts touched\n", movedVerts);
    TASSERT(movedVerts > 0);
    TASSERT(log.undo());
    Vector<float3> cur;
    cur.resize(d->vertCount());
    for (int v = 0; v < d->vertCount(); v++) {
      cur[v] = d->pos()[v];
    }
    TASSERT(samePosBits(cur, pre));
    TASSERT(storeBlob(mr.store) == blobPre);
  }

  /* Interleaving: grids stroke -> mesh-path stroke -> grids stroke, domain
   * refetched across the fold point, both views consistent. */
  {
    Brush brush;
    setupBrush(brush, 0.25f, 0.5f);
    restoreStore(mr, s0);

    GridLevelDomain *d = mr.gridDomain(kLevel);
    GridBrushExecutor ex(d, &brush, nullptr);
    ex.beginStep();
    int m1 = ex.applyDab(SculptBrushes::DRAW, float3(0, 0, 0.5f), float3(0, 0, 1));
    ex.endStep();
    TASSERT(m1 > 0);

    // Mesh-path stroke on the same level (the fallback flow) + writeback.
    MultiresSlot *slot = mr.setActiveLevel(kLevel);
    {
      CommandExecutor mex(slot->tree, &brush);
      mex.setStrokeGen(2);
      mex.beginStep(false);
      Vector<spatial::SpatialNode *> nodes;
      slot->tree->filterNodes(float3(0.3f, 0, 0.5f), brush.radius, nodes);
      mex.execBrush(slot->mesh,
                    SculptBrushes::DRAW,
                    &nodes,
                    float3(0.3f, 0, 0.5f),
                    float3(0, 0, 1));
      mex.endStep();
    }
    int changed = mr.writeback(kLevel);
    TASSERT(changed > 0);

    // The fold point dropped the domain; refetch and verify it matches the
    // materialized view bit-exactly.
    GridLevelDomain *d2 = mr.gridDomain(kLevel);
    slot = mr.findSlot(kLevel);
    TASSERT(slot && slot->mesh);
    bool same = true;
    for (int v = 0; v < d2->vertCount(); v++) {
      same =
          same && std::memcmp(&d2->pos()[v], &slot->mesh->v.co[v], sizeof(float3)) == 0;
    }
    TASSERT(same);

    ex.attach(d2);
    ex.beginStep();
    int m3 = ex.applyDab(SculptBrushes::DRAW, float3(-0.3f, 0, 0.5f), float3(0, 0, 1));
    ex.endStep();
    TASSERT(m3 > 0);
  }

  /* Domain generation: a drop + rebuild must be observable even when the
   * allocator hands back the same block (the pointer-ABA bug: a stale
   * session kept its freed tree). GridStroke_sync must report the rebind. */
  {
    Brush brush;
    setupBrush(brush, 0.25f, 0.5f);
    restoreStore(mr, s0);
    mr.gridDomain(kLevel);
    const uint64_t g0 = mr.domainGeneration();

    GridStrokeSession *s = GridStroke_new(&mr, kLevel, &brush);
    TASSERT(s != nullptr);
    TASSERT(GridStroke_sync(s) == 1); /* fresh bind reads as current */

    // A slot edit + writeback drops the domain (the fold every mesh-path
    // stroke takes); the generation must move on drop AND on rebuild.
    MultiresSlot *slot = mr.setActiveLevel(kLevel);
    slot->mesh->v.co[0][2] += 0.25f;
    TASSERT(mr.writeback(kLevel) > 0);
    TASSERT(!mr.hasGridDomain(kLevel));
    const uint64_t g1 = mr.domainGeneration();
    TASSERT(g1 > g0);
    TASSERT(GridStroke_sync(s) == 2); /* rebuild detected regardless of address */
    TASSERT(mr.domainGeneration() > g1);
    TASSERT(GridStroke_sync(s) == 1); /* and the rebind is now current */
    GridStroke_free(s);
  }

  /* Writeback authority: a grids fold with NO host mirror leaves the slot
   * stale; a later writeback (reached implicitly by level switches, saves,
   * the undo heal) must not diff that pre-stroke slot back over the grids
   * stroke — the pressure-test's silent-data-loss scenario. */
  {
    Brush brush;
    setupBrush(brush, 0.25f, 0.5f);
    restoreStore(mr, s0);
    MultiresSlot *slot = mr.setActiveLevel(kLevel);
    TASSERT(slot && slot->mesh);
    GridLevelDomain *d = mr.gridDomain(kLevel);
    GridStrokeLog log;
    GridBrushExecutor ex(d, &brush, &log);

    ex.beginStep();
    int moved = ex.applyDab(SculptBrushes::DRAW, float3(0, 0, 0.5f), float3(0, 0, 1));
    ex.endStep(); /* fold ran; no mirror -> slot is pre-stroke */
    TASSERT(moved > 0);
    TASSERT(mr.slotStale(kLevel));

    const std::string post = storeBlob(mr.store);
    TASSERT(mr.writeback(kLevel) == 0);   /* refused the stale diff */
    TASSERT(storeBlob(mr.store) == post); /* grids stroke survives */
    TASSERT(!mr.slotStale(kLevel));       /* and the slot was healed */
    bool healed = true;
    for (int v = 0; v < d->vertCount(); v++) {
      healed =
          healed && std::memcmp(&d->pos()[v], &slot->mesh->v.co[v], sizeof(float3)) == 0;
    }
    TASSERT(healed);

    /* A real mesh-path edit on the healed slot still folds normally. */
    slot->mesh->v.co[0][2] += 0.125f;
    TASSERT(mr.writeback(kLevel) > 0);
  }
}
