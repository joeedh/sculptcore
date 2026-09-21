/* The grids draw source (extdraw provider v3): partition coverage, the
 * born-dirty contract and restricted dirty marking. */

#include "test_grid_stroke_common.h"

void gridStrokeDrawSource(GridStrokeFixture &fx)
{
  Multires &mr = fx.mr;
  const std::string &s0 = fx.s0;

  /* Grids draw source (extdraw provider v3): partition coverage in both
   * fill modes (indexed default / SC_GRIDS_INDEXED=0 soup), the
   * born-dirty/consume-on-read contract through the c-api, and restricted
   * dirty marking after a stroke and an undo. */
  {
    Brush brush;
    setupBrush(brush, 0.25f, 0.5f);
    restoreStore(mr, s0);
    GridLevelDomain *d = mr.gridDomain(kLevel);

    const unsigned key = 4242;
    sc_external_draw_register_grids(key, &mr, kLevel);
    TASSERT(mr.drawSource() != nullptr);

    const ScExternalDrawProvider *prov = sc_external_draw_provider();
    ScExternalDrawNode *nodes = nullptr;
    int count = prov->nodes_get(prov->user_data, key, nullptr, &nodes);
    TASSERT(count > 0);

    /* Coverage: every cell drawn exactly once — corner count and position
     * sum match an independent walk of the level's grid lattices. */
    subdiv::SubdivLevel &lvl = mr.refiner.levels[kLevel - 1];
    const int S = lvl.gridSide, w = S + 1;
    const Vector<float3> &pos = d->pos();
    double expectSum = 0.0;
    long expectCorners = 0;
    for (int g = 0; g < mr.store.gridCount(); g++) {
      const int *gv = &lvl.gridVerts[g * w * w];
      for (int cv = 0; cv < S; cv++) {
        for (int cu = 0; cu < S; cu++) {
          const int c4[4] = {gv[cv * w + cu],
                             gv[cv * w + cu + 1],
                             gv[(cv + 1) * w + cu + 1],
                             gv[(cv + 1) * w + cu]};
          /* 6 corners per cell: a,b,c + a,c,d. */
          const int corner[6] = {c4[0], c4[1], c4[2], c4[0], c4[2], c4[3]};
          for (int k = 0; k < 6; k++) {
            const float3 &p = pos[corner[k]];
            expectSum += double(p[0]) + double(p[1]) + double(p[2]);
            expectCorners++;
          }
        }
      }
    }
    double gotSum = 0.0;
    long gotCorners = 0;
    bool bornDirty = true, idsOk = true;
    for (int i = 0; i < count; i++) {
      const ScExternalDrawNode &n = nodes[i];
      bornDirty = bornDirty && (n.update_flags & SC_EXTERNAL_DRAW_UPDATE_TOPOLOGY) &&
                  (n.update_flags & SC_EXTERNAL_DRAW_UPDATE_DATA);
      idsOk = idsOk && n.node_id >= SC_EXTERNAL_DRAW_CUSTOM_ID_BASE;
      TASSERT(n.verts_num > 0);
      /* mask@2 is gated on the store channel existing (a maskless session
       * must not advertise a stream, or the host overlays the whole mesh). */
      TASSERT(n.attrs != nullptr);
      TASSERT((n.attrs[2] != nullptr) == d->maskChannelExists());
      if (n.indices != nullptr) {
        /* Indexed (the default): shared lattice verts, triangles via the
         * static index stream. The corner walk through it must cover every
         * cell exactly once, same as the soup it replaced. */
        TASSERT(n.indices_num > 0 && n.indices_num % 3 == 0);
        for (int k = 0; k < n.indices_num; k++) {
          TASSERT(n.indices[k] < uint32_t(n.verts_num));
          const float *p = n.positions[n.indices[k]];
          gotSum += double(p[0]) + double(p[1]) + double(p[2]);
          gotCorners++;
        }
      } else {
        /* Soup (SC_GRIDS_INDEXED=0 on the whole test run). */
        TASSERT(n.indices_num == 0);
        TASSERT(n.verts_num % 3 == 0);
        for (int v = 0; v < n.verts_num; v++) {
          gotSum += double(n.positions[v][0]) + double(n.positions[v][1]) +
                    double(n.positions[v][2]);
          gotCorners++;
        }
      }
    }
    TASSERT(bornDirty);
    TASSERT(idsOk);
    TASSERT(gotCorners == expectCorners); /* == 6 x total cells */
    TASSERT(std::abs(gotSum - expectSum) < 1e-6 * std::abs(expectSum) + 1e-9);

    /* Kill-switch: SC_GRIDS_INDEXED=0 (read at construction) restores the
     * de-indexed soup — no index stream, verts a multiple of 3, and the
     * identical corner sum. */
    {
      const char *prevEnv = getenv("SC_GRIDS_INDEXED");
      const std::string prev = prevEnv ? prevEnv : "";
#ifdef _WIN32
      _putenv_s("SC_GRIDS_INDEXED", "0");
#else
      setenv("SC_GRIDS_INDEXED", "0", 1);
#endif
      subdiv::GridDrawSource soup(&mr, kLevel);
#ifdef _WIN32
      _putenv_s("SC_GRIDS_INDEXED", prev.c_str());
#else
      prevEnv ? setenv("SC_GRIDS_INDEXED", prev.c_str(), 1)
              : unsetenv("SC_GRIDS_INDEXED");
#endif
      double soupSum = 0.0;
      long soupCorners = 0;
      for (int i = 0; i < soup.nodeCount(); i++) {
        subdiv::GridDrawSource::Node &n = soup.node(i);
        TASSERT(n.indices.size() == 0);
        TASSERT(n.verts > 0 && n.verts % 3 == 0);
        for (int v = 0; v < n.verts; v++) {
          soupSum += double(n.pos[v][0]) + double(n.pos[v][1]) + double(n.pos[v][2]);
          soupCorners++;
        }
      }
      TASSERT(soupCorners == expectCorners);
      TASSERT(std::abs(soupSum - expectSum) < 1e-6 * std::abs(expectSum) + 1e-9);
    }

    /* Consume-on-read: a second sync with no edits reports NONE. */
    count = prov->nodes_get(prov->user_data, key, nullptr, &nodes);
    bool allNone = true;
    for (int i = 0; i < count; i++) {
      allNone = allNone && nodes[i].update_flags == SC_EXTERNAL_DRAW_UPDATE_NONE;
    }
    TASSERT(allNone);

    /* A stroke marks a strict subset; the update refills only that subset. */
    GridStrokeSession *s = GridStroke_new(&mr, kLevel, &brush);
    TASSERT(GridStroke_begin(s) == 1);
    TASSERT(GridStroke_dab(s, int(SculptBrushes::DRAW), 0, 0, 0.5f, 0, 0, 1, 0) > 0);
    GridStroke_end(s);
    sc_external_draw_update(key);
    count = prov->nodes_get(prov->user_data, key, nullptr, &nodes);
    int dirtyCount = 0;
    for (int i = 0; i < count; i++) {
      dirtyCount += (nodes[i].update_flags & SC_EXTERNAL_DRAW_UPDATE_DATA) ? 1 : 0;
    }
    fprintf(stderr, "draw source: %d nodes, %d dirty after dab\n", count, dirtyCount);
    TASSERT(dirtyCount > 0); /* fixture fits one node; subset gated below */

    /* Undo marks again (via the log's applySwap feed). */
    TASSERT(GridStroke_undo(s) == 1);
    sc_external_draw_update(key);
    count = prov->nodes_get(prov->user_data, key, nullptr, &nodes);
    dirtyCount = 0;
    for (int i = 0; i < count; i++) {
      dirtyCount += (nodes[i].update_flags & SC_EXTERNAL_DRAW_UPDATE_DATA) ? 1 : 0;
    }
    TASSERT(dirtyCount > 0);

    /* A mask stroke creates the store channel and flips the mask@2 gate on. */
    TASSERT(!d->maskChannelExists());
    TASSERT(GridStroke_begin(s) == 1);
    TASSERT(GridStroke_dab(s, int(SculptBrushes::MASK), 0, 0, 0.5f, 0, 0, 1, 0) > 0);
    GridStroke_end(s);
    sc_external_draw_update(key);
    count = prov->nodes_get(prov->user_data, key, nullptr, &nodes);
    TASSERT(d->maskChannelExists());
    bool maskAdvertised = count > 0;
    for (int i = 0; i < count; i++) {
      maskAdvertised = maskAdvertised && nodes[i].attrs[2] != nullptr;
    }
    TASSERT(maskAdvertised);

    GridStroke_free(s);
    sc_external_draw_unregister(key);
    TASSERT(mr.drawSource() == nullptr);

    /* Small-target partition: many nodes, and a dab dirties a strict
     * subset (the whole point of the sub-grid granularity). */
    {
      subdiv::GridDrawSource src(&mr, kLevel, /*nodeTriTarget=*/64);
      TASSERT(src.nodeCount() > 4);
      mr.setDrawSource(&src);
      GridStrokeSession *s2 = GridStroke_new(&mr, kLevel, &brush);
      src.update();
      for (int i = 0; i < src.nodeCount(); i++) {
        src.node(i).update = subdiv::GridDrawSource::Update_None;
      }
      TASSERT(GridStroke_begin(s2) == 1);
      TASSERT(GridStroke_dab(s2, int(SculptBrushes::DRAW), 0, 0, 0.5f, 0, 0, 1, 0) > 0);
      GridStroke_end(s2);
      src.update();
      int sub = 0;
      for (int i = 0; i < src.nodeCount(); i++) {
        sub += (src.node(i).update & subdiv::GridDrawSource::Update_Data) ? 1 : 0;
      }
      fprintf(
          stderr, "draw source small-target: %d nodes, %d dirty\n", src.nodeCount(), sub);
      TASSERT(sub > 0 && sub < src.nodeCount());
      GridStroke_free(s2);
      mr.setDrawSource(nullptr);
    }
  }
}
