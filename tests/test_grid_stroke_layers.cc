/* Sculpt layers on grids: the edit-target channel, LAYERDRAW's conditional
 * roster entry and its dab-for-dab parity with the mesh path. */

#include "test_grid_stroke_common.h"

void gridStrokeLayers(GridStrokeFixture &fx)
{
  Multires &mr = fx.mr;
  const std::string &s0 = fx.s0;
  const DabBattery &dabs = fx.dabs;

  /* Layer interaction: an edit-target stroke lands in the layer's channel,
   * channel 0 stays byte-identical. */
  {
    Brush brush;
    setupBrush(brush, 0.25f, 0.5f);
    restoreStore(mr, s0);
    int li = mr.layerAdd();
    TASSERT(li >= 0);
    TASSERT(mr.setEditTarget(li) == li);
    int lch = mr.writebackChannel();
    TASSERT(lch > 0);

    GridLevelDomain *d = mr.gridDomain(kLevel);
    int S = subdiv::GridsStore::sideForLevel(kLevel), w = S + 1;
    Vector<float> ch0Pre;
    for (int g = 0; g < mr.store.gridCount(); g++) {
      for (int v = 0; v < w; v++) {
        for (int u = 0; u < w; u++) {
          const float *x = mr.store.elem(kLevel, 0, g, u, v);
          ch0Pre.append(x[0]);
          ch0Pre.append(x[1]);
          ch0Pre.append(x[2]);
        }
      }
    }

    GridStrokeLog log;
    GridBrushExecutor ex(d, &brush, &log);
    ex.beginStep();
    ex.applyDab(SculptBrushes::DRAW, float3(0, 0, 0.5f), float3(0, 0, 1));
    ex.endStep();

    float layerMax = 0.0f;
    int at = 0;
    bool ch0Same = true;
    for (int g = 0; g < mr.store.gridCount(); g++) {
      for (int v = 0; v < w; v++) {
        for (int u = 0; u < w; u++) {
          const float *x = mr.store.elem(kLevel, 0, g, u, v);
          ch0Same = ch0Same && x[0] == ch0Pre[at] && x[1] == ch0Pre[at + 1] &&
                    x[2] == ch0Pre[at + 2];
          at += 3;
          const float *l = mr.store.elem(kLevel, lch, g, u, v);
          for (int k = 0; k < 3; k++) {
            layerMax = std::fabs(l[k]) > layerMax ? std::fabs(l[k]) : layerMax;
          }
        }
      }
    }
    fprintf(stderr,
            "layer stroke: channel %d max |d| %.6f, ch0 same %d\n",
            lch,
            layerMax,
            int(ch0Same));
    TASSERT(layerMax > 1e-4f);
    TASSERT(ch0Same);
    mr.setEditTarget(-1);
  }

  /* LAYERDRAW on grids (LD1): declined without an edit target; under one, the
   * dab moves co by w*delta with the residual attributed to the target layer's
   * channel (channel 0 byte-identical), undo restores co + channel atomically,
   * and a post-stroke layerSetWeight re-composites. */
  {
    Brush brush;
    setupBrush(brush, 0.5f, 0.5f);
    restoreStore(mr, s0);

    TASSERT(mr.editTarget() < 0);
    TASSERT(!GridBrushExecutor::supportsBrush(SculptBrushes::LAYERDRAW, &mr.gridAttrs()));

    int li = mr.layerAdd();
    TASSERT(li >= 0);
    TASSERT(mr.setEditTarget(li) == li);
    int lch = mr.writebackChannel();
    TASSERT(lch > 0);
    TASSERT(GridBrushExecutor::supportsBrush(SculptBrushes::LAYERDRAW, &mr.gridAttrs()));

    // setEditTarget rebuilt the level (refreshAfterLayerChange): fetch after.
    GridLevelDomain *d = mr.gridDomain(kLevel);
    int S = subdiv::GridsStore::sideForLevel(kLevel), w = S + 1;
    Vector<float3> prePos;
    prePos.resize(d->vertCount());
    for (int v = 0; v < d->vertCount(); v++) {
      prePos[v] = d->pos()[v];
    }
    Vector<float> ch0Pre;
    for (int g = 0; g < mr.store.gridCount(); g++) {
      for (int v = 0; v < w; v++) {
        for (int u = 0; u < w; u++) {
          const float *x = mr.store.elem(kLevel, 0, g, u, v);
          ch0Pre.append(x[0]);
          ch0Pre.append(x[1]);
          ch0Pre.append(x[2]);
        }
      }
    }
    const std::string blobPre = storeBlob(mr.store);

    GridStrokeLog log;
    GridBrushExecutor ex(d, &brush, &log);
    ex.beginStep();
    int moved =
        ex.applyDab(SculptBrushes::LAYERDRAW, float3(0, 0, 0.5f), float3(0, 0, 1));
    ex.endStep();
    TASSERT(moved > 0);

    Vector<float3> postPos;
    postPos.resize(d->vertCount());
    for (int v = 0; v < d->vertCount(); v++) {
      postPos[v] = d->pos()[v];
    }
    TASSERT(maxPosDiff(postPos, prePos) > 1e-4f);

    float layerMax = 0.0f;
    int at = 0;
    bool ch0Same = true;
    for (int g = 0; g < mr.store.gridCount(); g++) {
      for (int v = 0; v < w; v++) {
        for (int u = 0; u < w; u++) {
          const float *x = mr.store.elem(kLevel, 0, g, u, v);
          ch0Same = ch0Same && x[0] == ch0Pre[at] && x[1] == ch0Pre[at + 1] &&
                    x[2] == ch0Pre[at + 2];
          at += 3;
          const float *l = mr.store.elem(kLevel, lch, g, u, v);
          for (int k = 0; k < 3; k++) {
            layerMax = std::fabs(l[k]) > layerMax ? std::fabs(l[k]) : layerMax;
          }
        }
      }
    }
    fprintf(stderr,
            "layerdraw stroke: channel %d max |d| %.6f, ch0 same %d\n",
            lch,
            layerMax,
            int(ch0Same));
    TASSERT(layerMax > 1e-4f);
    TASSERT(ch0Same);
    const std::string blobPost = storeBlob(mr.store);

    // Undo/redo before any weight mutation: refreshAfterLayerChange rebuilds
    // the domain, which orphans the log's leaf ids.
    Vector<float3> cur;
    cur.resize(d->vertCount());
    TASSERT(log.undo());
    for (int v = 0; v < d->vertCount(); v++) {
      cur[v] = d->pos()[v];
    }
    TASSERT(samePosBits(cur, prePos));
    TASSERT(storeBlob(mr.store) == blobPre);
    TASSERT(log.redo());
    for (int v = 0; v < d->vertCount(); v++) {
      cur[v] = d->pos()[v];
    }
    TASSERT(samePosBits(cur, postPos));
    TASSERT(storeBlob(mr.store) == blobPost);

    // Re-composite: weight 0 removes the layer's contribution (which also
    // clears the edit target), weight 1 restores it. Refetch the domain after
    // each mutation -- refreshAfterLayerChange drops it.
    mr.layerSetWeight(li, 0.0f);
    TASSERT(mr.editTarget() < 0);
    d = mr.gridDomain(kLevel);
    for (int v = 0; v < d->vertCount(); v++) {
      cur[v] = d->pos()[v];
    }
    float offDiff = maxPosDiff(cur, prePos);
    mr.layerSetWeight(li, 1.0f);
    d = mr.gridDomain(kLevel);
    for (int v = 0; v < d->vertCount(); v++) {
      cur[v] = d->pos()[v];
    }
    float onDiff = maxPosDiff(cur, postPos);
    fprintf(stderr, "layerdraw recomposite: off %.2e on %.2e\n", offDiff, onDiff);
    TASSERT(offDiff < 1e-5f);
    TASSERT(onDiff < 1e-5f);
  }

  /* LAYERDRAW A/B (LD2): dab-for-dab parity, mesh path vs grids path, both
   * sides configured -- the mr edit target for stroke-end attribution, and a
   * "slayer" settings row + column on the slot mesh so the mesh executor's
   * LayerEditScope bracket folds (it is inert without one). */
  {
    Brush brush;
    setupBrush(brush, 0.25f, 0.5f);
    restoreStore(mr, s0);
    int li = mr.layerAdd();
    TASSERT(li >= 0);
    TASSERT(mr.setEditTarget(li) == li);
    int lch = mr.writebackChannel();
    TASSERT(lch > 0);
    const std::string s1 = storeBlob(mr.store);

    // Fresh slot: "slayer" is unique there, so the settings row and the
    // kernel's manifest binding resolve to the same attr.
    MultiresSlot *slot = mr.setActiveLevel(kLevel);
    TASSERT(slot && slot->mesh);
    TASSERT(slot->mesh->findSculptLayer(string("slayer")) < 0);
    TASSERT(slot->mesh->addSculptLayerNamed("slayer") >= 0);
    Vector<float3> posA;
    meshStroke(mr, brush, SculptBrushes::LAYERDRAW, dabs, posA);
    std::string blobA = storeBlob(mr.store);

    restoreStore(mr, s1);
    Vector<float3> posB;
    gridsStroke(mr, brush, SculptBrushes::LAYERDRAW, dabs, posB);

    TASSERT(posA.size() == posB.size());
    float diff = maxPosDiff(posA, posB);
    fprintf(stderr, "A/B layerdraw: max pos diff %.8f\n", diff);
    TASSERT(diff <= 1e-6f);

    // Both channels: ch0 must agree (neither path may leak the delta into
    // base displacement) and the layer channel holds the same residual.
    {
      subdiv::GridsStore tmp;
      std::stringstream ss(blobA, std::ios::in | std::ios::out | std::ios::binary);
      TASSERT(tmp.read(ss));
      int S = subdiv::GridsStore::sideForLevel(kLevel), w = S + 1;
      float maxd = 0.0f;
      for (int ch : {0, lch}) {
        for (int g = 0; g < mr.store.gridCount(); g++) {
          for (int v = 0; v < w; v++) {
            for (int u = 0; u < w; u++) {
              const float *da = tmp.elem(kLevel, ch, g, u, v);
              const float *db = mr.store.elem(kLevel, ch, g, u, v);
              for (int k = 0; k < 3; k++) {
                float dd = std::fabs(da[k] - db[k]);
                maxd = dd > maxd ? dd : maxd;
              }
            }
          }
        }
      }
      fprintf(stderr, "A/B layerdraw store: max disp diff %.8f\n", maxd);
      TASSERT(maxd <= 1e-6f);
    }
    mr.setEditTarget(-1);
  }
}
