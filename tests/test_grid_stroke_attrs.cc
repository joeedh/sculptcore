/* Grid attribute channels: name-keyed undo blocks, attribute debt on the log,
 * colour strokes into session channels, derived samples and face stages. */

#include "test_grid_stroke_common.h"

void gridStrokeAttrs(GridStrokeFixture &fx)
{
  Multires &mr = fx.mr;
  const std::string &s0 = fx.s0;
  Mesh *cage = fx.cage;

  /* P1: undo blocks are keyed by channel NAME, so removeChannel — which shifts
   * every later channel index down — cannot make a captured block restore into
   * the wrong column or off the end of the store. */
  {
    restoreStore(mr, s0);
    GridLevelDomain *d = mr.gridDomain(kLevel);
    const int chA = mr.store.addChannel(util::string("undoA"), 1);
    const int chB = mr.store.addChannel(util::string("undoB"), 1);
    TASSERT(chB == chA + 1);

    Vector<int> grids;
    for (int g = 0; g < mr.store.gridCount(); g++) {
      grids.append(g);
    }
    const int S = mr.store.sideForLevel(kLevel), w = S + 1;
    auto paint = [&](int ch, float bias) {
      for (int g : grids) {
        for (int v = 0; v <= S; v++) {
          for (int u = 0; u <= S; u++) {
            *mr.store.elem(kLevel, ch, g, u, v) = bias + float(g * w * w + v * w + u);
          }
        }
      }
    };
    auto same = [&](int ch, float bias) {
      for (int g : grids) {
        for (int v = 0; v <= S; v++) {
          for (int u = 0; u <= S; u++) {
            const float want = bias + float(g * w * w + v * w + u);
            if (*mr.store.elem(kLevel, ch, g, u, v) != want) {
              return false;
            }
          }
        }
      }
      return true;
    };

    paint(chA, 1000.0f);
    paint(chB, 2000.0f);

    GridStrokeLog log;
    log.attach(d);
    log.beginStep();
    log.captureGrids(std::span<const int>(grids.data(), grids.size()), chB);
    log.endStep(false);
    TASSERT(log.stepCount() == 1);

    paint(chB, 5000.0f);
    /* B slides from chA+1 down to chA; the captured block must follow it. */
    mr.store.removeChannel(chA);
    TASSERT(mr.store.findChannel(util::string("undoB")) == chA);
    TASSERT(log.undo());
    TASSERT(same(chA, 2000.0f));
    TASSERT(log.redo());
    TASSERT(same(chA, 5000.0f));

    /* And a block whose channel is gone entirely is dropped, not applied. */
    mr.store.removeChannel(chA);
    TASSERT(mr.store.findChannel(util::string("undoB")) == -1);
    TASSERT(log.undo());
    fprintf(stderr, "undo blocks survived removeChannel\n");
    restoreStore(mr, s0);
  }

  /* C4: the per-channel attribute debt rides the log the way the position debt
   * does -- undo puts back the state the step opened on, redo the state it
   * closed on. Without it, undoing a paint stroke would leave the level marked
   * as owing the one below, and the next downward level switch would restrict
   * paint the user had just taken back. */
  {
    restoreStore(mr, s0);
    GridLevelDomain *d = mr.gridDomain(kLevel);
    Vector<int> grids;
    for (int g = 0; g < mr.refiner.gridCount(); g++) {
      grids.append(g);
    }
    const int ch = d->ensureMaskChannel();
    GridStrokeLog log;
    log.attach(d);
    log.beginStep();
    log.captureGrids(std::span<const int>(grids.data(), grids.size()), ch);
    Vector<int> touched;
    for (int v = 0; v < d->vertCount(); v++) {
      d->mask[v] = 0.5f;
      touched.append(v);
    }
    /* The touched-verts flush is a stroke folding its dab, so it owes. */
    d->flushMaskToStore(std::span<const int>(touched.data(), touched.size()));
    TASSERT(mr.store.channelLevelDebt(kLevel, ch));
    log.endStep(false);

    TASSERT(log.undo());
    TASSERT(!mr.store.channelLevelDebt(kLevel, ch));
    TASSERT(log.redo());
    TASSERT(mr.store.channelLevelDebt(kLevel, ch));
    mr.store.setChannelLevelDebt(kLevel, ch, false);
    fprintf(stderr, "attr debt round-tripped through undo/redo\n");
    restoreStore(mr, s0);
  }

  /* P2: with grid attribute channels on, a colour stroke paints a session
   * store channel; duplicate boundary samples agree, and undo/redo restores
   * the channel bit-exactly. */
  {
    /* A grids-native host: one that CAN store colour per grid element. That
     * declaration is the whole precondition since P5 deleted the switch. */
    mr.gridAttrs().declareHostAttr("color", mesh::AttrType::FLOAT4);
    restoreStore(mr, s0);
    TASSERT(GridBrushExecutor::supportsBrush(SculptBrushes::COLOR, &mr.gridAttrs()));

    Brush brush;
    setupBrush(brush, 0.25f, 0.5f);
    brush.brushColor = float4(1.0f, 0.5f, 0.25f, 1.0f);
    brush.mixMode = 0;

    GridLevelDomain *d = mr.gridDomain(kLevel);
    GridStrokeLog log;
    GridBrushExecutor ex(d, &brush, &log);
    ex.beginStep();
    ex.applyDab(SculptBrushes::COLOR, float3(0, 0, 0.5f), float3(0, 0, 1));
    ex.endStep();

    const int cch = mr.store.findChannel(util::string("color"));
    TASSERT(cch > 0);
    /* Host class: the host said it can store this per grid element, so the
     * channel is one it saves rather than session scratch. */
    TASSERT(mr.store.channelPersist(cch));
    TASSERT(mr.store.channelElemSize(cch) == 4);
    TASSERT(mr.store.channelDomain(cch) == subdiv::GridElemDomain::Vertex);

    const int S = mr.store.sideForLevel(kLevel);
    int painted = 0;
    float maxR = 0.0f;
    for (int g = 0; g < mr.store.gridCount(); g++) {
      for (int v = 0; v <= S; v++) {
        for (int u = 0; u <= S; u++) {
          const float *c = mr.store.elem(kLevel, cch, g, u, v);
          painted += c[0] != 0.0f;
          maxR = c[0] > maxR ? c[0] : maxR;
        }
      }
    }
    fprintf(stderr, "grid attr colour: %d painted samples, max r %.4f\n", painted, maxR);
    TASSERT(painted > 0);
    TASSERT(maxR > 0.01f);

    /* The scatter walks occurrences, so a vertex shared by several grids
     * reads the same bytes from all of them. */
    for (int v = 0; v < d->vertCount(); v++) {
      auto occs = d->occurrences(v);
      const float *ref = mr.store.elem(kLevel, cch, occs[0], occs[1], occs[2]);
      const float r[4] = {ref[0], ref[1], ref[2], ref[3]};
      for (size_t i = 3; i < occs.size(); i += 3) {
        const float *c = mr.store.elem(kLevel, cch, occs[i], occs[i + 1], occs[i + 2]);
        TASSERT(c[0] == r[0] && c[1] == r[1] && c[2] == r[2] && c[3] == r[3]);
      }
    }

    const std::string blobPost = storeBlob(mr.store);
    TASSERT(log.undo());
    for (int g = 0; g < mr.store.gridCount(); g++) {
      for (int v = 0; v <= S; v++) {
        for (int u = 0; u <= S; u++) {
          const float *c = mr.store.elem(kLevel, cch, g, u, v);
          TASSERT(c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 0.0f);
        }
      }
    }
    TASSERT(log.redo());
    TASSERT(storeBlob(mr.store) == blobPost);
    fprintf(stderr, "grid attr colour undo bit-exact\n");

    mr.gridAttrs().clearHostAttrs();
    restoreStore(mr, s0);
  }

  /* P4: the derived samples the draw path reads are the session channel's
   * mirror. Seeded from the cage so paint starts on the surface's own colour,
   * republished per dab (the store only learns of the stroke at the fold), and
   * re-overlaid after a rebuild or an undo. */
  {
    mr.gridAttrs().declareHostAttr("color", mesh::AttrType::FLOAT4);
    restoreStore(mr, s0);

    /* A distinct colour per cage vertex: a seeded sample can then never be
     * mistaken for the zeros a fresh channel would hold. */
    AttrRef &cref =
        cage->v.attrs.ensure(mesh::AttrType::FLOAT4, "color", /*materialize=*/true);
    mesh::AttrData<float4> *col = cref.get_data<float4>();
    TASSERT(col != nullptr);
    for (int i = 0; i < cage->v.count; i++) {
      (*col)[i] = float4(0.1f + 0.02f * float(i), 0.2f, 0.3f, 1.0f);
    }
    mr.gridAttrs().invalidateAll();

    const int S = mr.store.sideForLevel(kLevel);
    const int w = S + 1;
    const size_t nSamples = size_t(mr.store.gridCount()) * size_t(w) * size_t(w);
    Vector<float4> baseline;
    {
      const float4 *derived = mr.gridAttrs().colorSamples(kLevel);
      TASSERT(derived != nullptr);
      baseline.resize(nSamples);
      for (size_t i = 0; i < nSamples; i++) {
        baseline[i] = derived[i];
      }
    }
    /* The gradient has to actually vary, or the checks below prove nothing. */
    TASSERT(baseline[0][0] != baseline[nSamples - 1][0]);

    Brush brush;
    setupBrush(brush, 0.25f, 0.5f);
    brush.brushColor = float4(1.0f, 0.0f, 0.0f, 1.0f);
    brush.mixMode = 0;

    GridLevelDomain *d = mr.gridDomain(kLevel);
    GridStrokeLog log;
    GridBrushExecutor ex(d, &brush, &log);
    ex.beginStep();
    ex.applyDab(SculptBrushes::COLOR, float3(0, 0, 0.5f), float3(0, 0, 1));

    /* Per-dab publish: the samples moved before endStep folded anything. */
    int movedSamples = 0;
    {
      const float4 *live = mr.gridAttrs().colorSamples(kLevel);
      TASSERT(live != nullptr);
      for (size_t i = 0; i < nSamples; i++) {
        movedSamples += !sameColor(live[i], baseline[i]);
      }
    }
    fprintf(stderr,
            "grid attr colour: %d of %d samples published per dab\n",
            movedSamples,
            int(nSamples));
    TASSERT(movedSamples > 0);
    ex.endStep();

    Vector<float4> post;
    post.resize(nSamples);
    {
      const float4 *afterFold = mr.gridAttrs().colorSamples(kLevel);
      for (size_t i = 0; i < nSamples; i++) {
        post[i] = afterFold[i];
      }
    }

    /* A rebuild re-subdivides the cage and overlays the channel on top, so it
     * reproduces both halves bit-exactly: the paint, and the seed under it.
     * Without the seed the untouched samples would come back zeroed. */
    mr.gridAttrs().invalidateAll();
    {
      const float4 *rebuilt = mr.gridAttrs().colorSamples(kLevel);
      TASSERT(rebuilt != nullptr);
      for (size_t i = 0; i < nSamples; i++) {
        TASSERT(sameColor(rebuilt[i], post[i]));
      }
    }

    /* Undo swaps the store back; the samples have to follow, or the viewport
     * keeps drawing a stroke the store no longer holds. */
    TASSERT(log.undo());
    {
      const float4 *undone = mr.gridAttrs().colorSamples(kLevel);
      for (size_t i = 0; i < nSamples; i++) {
        TASSERT(sameColor(undone[i], baseline[i]));
      }
    }
    TASSERT(log.redo());
    {
      const float4 *redone = mr.gridAttrs().colorSamples(kLevel);
      for (size_t i = 0; i < nSamples; i++) {
        TASSERT(sameColor(redone[i], post[i]));
      }
    }
    fprintf(stderr, "grid attr colour draw samples seeded + bit-exact through undo\n");

    mr.gridAttrs().clearHostAttrs();
    restoreStore(mr, s0);
  }

  /* P4b: a face stage runs grids-native. Polygroup iterates each leaf's quad
   * cells (GridFaceIter), writes a Face-domain session channel, publishes
   * per-sample face-set colours per dab, restores bit-exactly through undo,
   * and pushes down to the cage on demand. */
  {
    mr.gridAttrs().declareHostAttr("group", mesh::AttrType::INT);
    restoreStore(mr, s0);
    TASSERT(GridBrushExecutor::supportsBrush(SculptBrushes::POLYGROUP, &mr.gridAttrs()));

    cage->default_group_id = 1;
    cage->ensureFaceGroups();
    mr.gridAttrs().invalidateAll();
    /* Nothing to sample from until a stroke allocates the channel -- the
     * per-grid cache is still the whole truth. */
    TASSERT(mr.gridAttrs().faceSetSampleColors(kLevel) == nullptr);

    Brush brush;
    setupBrush(brush, 0.25f, 0.5f);
    brush.activeGroup = 7;

    GridLevelDomain *d = mr.gridDomain(kLevel);
    GridStrokeLog log;
    GridBrushExecutor ex(d, &brush, &log);
    ex.beginStep();
    ex.applyDab(SculptBrushes::POLYGROUP, float3(0, 0, 0.5f), float3(0, 0, 1));

    const int gch = mr.store.findChannel(util::string("group"));
    TASSERT(gch > 0);
    /* Host class: the host said it can store this per grid element, so the
     * channel is one it saves rather than session scratch. */
    TASSERT(mr.store.channelPersist(gch));
    TASSERT(mr.store.channelElemSize(gch) == 1);
    TASSERT(mr.store.channelDomain(gch) == subdiv::GridElemDomain::Face);

    const int S = mr.store.sideForLevel(kLevel);
    const int w = S + 1;
    const size_t nSamples = size_t(mr.store.gridCount()) * size_t(w) * size_t(w);

    /* Per-dab publish: the draw path's per-sample colours moved before endStep
     * folded anything, and they are not uniform -- a dab that covered every
     * sample or none would prove nothing about the cell walk. */
    int movedSamples = 0;
    {
      const float3 *live = mr.gridAttrs().faceSetSampleColors(kLevel);
      TASSERT(live != nullptr);
      for (size_t i = 0; i < nSamples; i++) {
        movedSamples += live[i][0] != live[0][0] || live[i][1] != live[0][1] ||
                        live[i][2] != live[0][2];
      }
    }
    fprintf(stderr,
            "grid attr polygroup: %d of %d samples differ per dab\n",
            movedSamples,
            int(nSamples));
    TASSERT(movedSamples > 0);
    ex.endStep();

    /* The fold wrote cells, not verts: the group channel carries the active
     * id where the dab landed. */
    int painted = 0;
    for (int g = 0; g < mr.store.gridCount(); g++) {
      for (int v = 0; v < S; v++) {
        for (int u = 0; u < S; u++) {
          painted +=
              *reinterpret_cast<const int *>(mr.store.elem(kLevel, gch, g, u, v)) == 7;
        }
      }
    }
    fprintf(stderr, "grid attr polygroup: %d cells set to the active group\n", painted);
    TASSERT(painted > 0);

    const std::string blobPost = storeBlob(mr.store);
    TASSERT(log.undo());
    for (int g = 0; g < mr.store.gridCount(); g++) {
      for (int v = 0; v < S; v++) {
        for (int u = 0; u < S; u++) {
          TASSERT(*reinterpret_cast<const int *>(mr.store.elem(kLevel, gch, g, u, v)) ==
                  1);
        }
      }
    }
    TASSERT(log.redo());
    TASSERT(storeBlob(mr.store) == blobPost);
    fprintf(stderr, "grid attr polygroup undo bit-exact\n");

    /* The cage push reads the store (a grids-native stroke materializes no
     * slot mesh), adopts the painted id per base face, and re-stamps every
     * cell of that face so the next push sees no fresh disagreement. */
    Vector<int> touched;
    const int changedFaces = mr.scatterFaceIntToCage(kLevel, "group", touched);
    fprintf(
        stderr, "grid attr polygroup: %d cage faces adopted the group\n", changedFaces);
    TASSERT(changedFaces > 0);
    TASSERT(touched.size() > 0);
    auto *cgroup =
        cage->f.attrs.find_attribute(mesh::AttrType::INT, "group").get_data<int>();
    TASSERT(cgroup != nullptr);
    int cageSet = 0;
    for (int fi : cage->f) {
      cageSet += cgroup->safe_get(fi) == 7;
    }
    TASSERT(cageSet > 0);
    Vector<int> again;
    TASSERT(mr.scatterFaceIntToCage(kLevel, "group", again) == 0);

    mr.gridAttrs().clearHostAttrs();
    restoreStore(mr, s0);
  }
}
