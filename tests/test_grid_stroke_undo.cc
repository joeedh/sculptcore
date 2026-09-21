/* Undo fidelity through GridStrokeLog and the mask stroke round-trip. */

#include "test_grid_stroke_common.h"

void gridStrokeUndo(GridStrokeFixture &fx)
{
  Multires &mr = fx.mr;
  const std::string &s0 = fx.s0;
  const DabBattery &dabs = fx.dabs;

  /* Undo fidelity: blob + positions bit-exact through undo, post state
   * bit-exact through redo — two strokes deep. */
  {
    Brush brush;
    setupBrush(brush, 0.25f, 0.5f);
    restoreStore(mr, s0);
    GridLevelDomain *d = mr.gridDomain(kLevel);
    GridStrokeLog log;
    GridBrushExecutor ex(d, &brush, &log);

    Vector<float3> pos0;
    pos0.resize(d->vertCount());
    for (int v = 0; v < d->vertCount(); v++) {
      pos0[v] = d->pos()[v];
    }
    const std::string blob0 = storeBlob(mr.store);

    auto stroke = [&](float xoff) {
      ex.beginStep();
      for (int i = 0; i < int(dabs.origins.size()); i++) {
        ex.applyDab(
            SculptBrushes::DRAW, dabs.origins[i] + float3(xoff, 0, 0), dabs.normals[i]);
      }
      ex.endStep();
    };
    auto snapshotPos = [&](Vector<float3> &out) {
      out.resize(d->vertCount());
      for (int v = 0; v < d->vertCount(); v++) {
        out[v] = d->pos()[v];
      }
    };

    stroke(0.0f);
    Vector<float3> pos1;
    snapshotPos(pos1);
    const std::string blob1 = storeBlob(mr.store);

    stroke(0.15f);
    Vector<float3> pos2;
    snapshotPos(pos2);
    const std::string blob2 = storeBlob(mr.store);

    fprintf(stderr, "undo log: %d steps, %zu bytes\n", log.stepCount(), log.bytes());
    TASSERT(log.stepCount() == 2);

    TASSERT(log.undo());
    Vector<float3> cur;
    snapshotPos(cur);
    TASSERT(samePosBits(cur, pos1));
    TASSERT(storeBlob(mr.store) == blob1);

    TASSERT(log.undo());
    snapshotPos(cur);
    TASSERT(samePosBits(cur, pos0));
    TASSERT(storeBlob(mr.store) == blob0);
    TASSERT(!log.undo());

    TASSERT(log.redo());
    snapshotPos(cur);
    TASSERT(samePosBits(cur, pos1));
    TASSERT(storeBlob(mr.store) == blob1);

    TASSERT(log.redo());
    snapshotPos(cur);
    TASSERT(samePosBits(cur, pos2));
    TASSERT(storeBlob(mr.store) == blob2);
    TASSERT(!log.redo());

    /* dropOldest: evicting the front step shortens reachable history without
     * touching the live surface; refused when everything is undone. */
    const size_t bytesBefore = log.bytes();
    TASSERT(log.dropOldest());
    TASSERT(log.stepCount() == 1);
    TASSERT(log.bytes() < bytesBefore);
    snapshotPos(cur);
    TASSERT(samePosBits(cur, pos2)); /* surface untouched */
    TASSERT(log.undo());
    snapshotPos(cur);
    TASSERT(samePosBits(cur, pos1));
    TASSERT(storeBlob(mr.store) == blob1);
    TASSERT(!log.undo());       /* pos0 evicted with the front step */
    TASSERT(!log.dropOldest()); /* cursor 0: front step is redo history */
    TASSERT(log.redo());
    snapshotPos(cur);
    TASSERT(samePosBits(cur, pos2));
    TASSERT(storeBlob(mr.store) == blob2);
  }

  /* Mask stroke: mirror + store channel round-trip with undo/redo. */
  {
    Brush brush;
    setupBrush(brush, 0.3f, 0.8f);
    restoreStore(mr, s0);
    GridLevelDomain *d = mr.gridDomain(kLevel);
    GridStrokeLog log;
    GridBrushExecutor ex(d, &brush, &log);

    ex.beginStep();
    ex.applyDab(SculptBrushes::MASK, float3(0, 0, 0.5f), float3(0, 0, 1));
    ex.endStep();

    int mch = mr.store.findChannel(util::string("mask"));
    TASSERT(mch > 0);
    float maxMask = 0.0f;
    for (int v = 0; v < d->vertCount(); v++) {
      maxMask = d->mask[v] > maxMask ? d->mask[v] : maxMask;
    }
    fprintf(stderr, "mask stroke: max mask %.4f\n", maxMask);
    TASSERT(maxMask > 0.01f);
    Vector<float> maskPost;
    maskPost.resize(d->vertCount());
    for (int v = 0; v < d->vertCount(); v++) {
      maskPost[v] = d->mask[v];
    }
    const std::string blobPost = storeBlob(mr.store);

    const auto beforeUndoMaskGeneration = mr.maskGeneration();
    TASSERT(log.undo());
    TASSERT(mr.maskGeneration() > beforeUndoMaskGeneration);
    for (int v = 0; v < d->vertCount(); v++) {
      TASSERT(d->mask[v] == 0.0f);
    }
    const auto beforeRedoMaskGeneration = mr.maskGeneration();
    TASSERT(log.redo());
    TASSERT(mr.maskGeneration() > beforeRedoMaskGeneration);
    bool same = true;
    for (int v = 0; v < d->vertCount(); v++) {
      same = same && d->mask[v] == maskPost[v];
    }
    TASSERT(same);
    TASSERT(storeBlob(mr.store) == blobPost);
  }
}
