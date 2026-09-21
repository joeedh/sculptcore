/* Per-kernel A/B against the materialized path, the mirror-stamp seam, the
 * BSMOOTH and program composites and the program undo footprint. */

#include "test_grid_stroke_common.h"

void gridStrokeAB(GridStrokeFixture &fx)
{
  Multires &mr = fx.mr;
  const std::string &s0 = fx.s0;
  const DabBattery &dabs = fx.dabs;

  for (bool reverse : {false, true}) {
    Brush brush;
    setupBrush(brush, 0.25f, 0.5f);
    brush.props.struct_def->Bool("projection", "Conflict", -1);
    BrushProgram program;
    program.addCommand(int(reverse ? SculptBrushes::BSMOOTH : SculptBrushes::KELVINLET));
    program.addCommand(int(reverse ? SculptBrushes::KELVINLET : SculptBrushes::BSMOOTH));
    auto *domain = mr.gridDomain(kLevel);
    GridStrokeLog log;
    GridBrushExecutor executor(domain, &brush, &log);
    Vector<float3> before;
    for (int i = 0; i < domain->vertCount(); i++) {
      before.append(domain->pos()[i]);
    }
    int samples = brush.strokePathCount;
    bool hadUndo = log.canUndo();
    TASSERT(executor.applyProgram(&program, float3(99), float3(0, 0, 1)) == -1);
    TASSERT(executor.applyProgram(&program, dabs.origins[0], dabs.normals[0]) == -1);
    TASSERT(!brush.props.struct_def->has("mu") && !brush.props.struct_def->has("nu"));
    TASSERT(samples == brush.strokePathCount && log.canUndo() == hadUndo);
    for (int i = 0; i < domain->vertCount(); i++) {
      TASSERT(std::memcmp(&domain->pos()[i][0], &before[i][0], 3 * sizeof(float)) == 0);
    }
    TASSERT(storeBlob(mr.store) == s0);
    GridStrokeSession *session = GridStroke_new(&mr, kLevel, &brush);
    TASSERT(session != nullptr);
    const float batchDab[] = {0, 0, 0.25f, 0, 0, 1, 0.25f};
    float strength = brush.strength;
    TASSERT(GridStroke_dabBatchProgram(
                session, &program, 1, batchDab, 0.123f, 1, 0.25f, 1, nullptr, 0) == -1);
    TASSERT(brush.strength == strength && brush.strokePathCount == samples);
    GridStroke_free(session);
  }

  /* Per-kernel A/B: mesh path vs grids path on identical store state. */
  struct ToolCase {
    SculptBrushes tool;
    const char *name;
    float posEps;
  };
  const ToolCase cases[] = {
      {SculptBrushes::DRAW, "draw", 1e-6f},
      {SculptBrushes::CLAY, "clay", 1e-6f},
      {SculptBrushes::PINCH, "pinch", 1e-6f},
      {SculptBrushes::SHARP, "sharp", 2e-3f},
      // Inflate displaces along v.no, and the two paths derive vertex normals
      // differently (recalc_normals' per-edge-radial fan pick vs the domain's
      // Newell cell fans) — the divergence is normal-source, not kernel.
      {SculptBrushes::INFLATE, "inflate", 5e-2f},
      {SculptBrushes::SMOOTH, "smooth", 2e-3f},
      // Newly grids-native once the roster became metadata-derived: snake hook
      // needs the per-dab grab step (the hook below) and wing scrape needs the
      // `host` stage, which this path used to skip entirely.
      {SculptBrushes::SNAKEHOOK, "snakehook", 1e-6f},
      {SculptBrushes::WINGSCRAPE, "wingscrape", 1e-6f},
  };
  for (const ToolCase &tc : cases) {
    Brush brush;
    setupBrush(brush, 0.25f, 0.5f);
    if (tc.tool == SculptBrushes::CLAY) {
      brush.planeSide = 1.0f;
    }
    DabHook hook = nullptr;
    if (tc.tool == SculptBrushes::SNAKEHOOK) {
      brush.pinch = 0.5f;
      // grabTo is the step since the last dab, so it stays constant; grabFrom
      // is this dab's center. Both are raw fields — the single-brush path does
      // not run loadUniformProps, so no writeProps is needed.
      hook = [&dabs](Brush &b, int i) {
        b.grabFrom = dabs.origins[i];
        b.grabTo = float3(0.0f, 0.0f, 0.03f);
      };
    }
    if (tc.tool == SculptBrushes::WINGSCRAPE) {
      // The wings meet below the surface or nothing on a flat face is above
      // one (see wingscrape.sbrush); wingAngle keeps its authored default.
      brush.planeoff = -0.25f;
      brush.writeProps();
    }

    restoreStore(mr, s0);
    Vector<float3> posA;
    meshStroke(mr, brush, tc.tool, dabs, posA, nullptr, hook);
    std::string blobA = storeBlob(mr.store);

    restoreStore(mr, s0);
    Vector<float3> posB;
    gridsStroke(mr, brush, tc.tool, dabs, posB, nullptr, hook);

    TASSERT(posA.size() == posB.size());
    float diff = maxPosDiff(posA, posB);
    fprintf(stderr, "A/B %s: max pos diff %.8f (eps %.8f)\n", tc.name, diff, tc.posEps);
    TASSERT(diff <= tc.posEps);

    if (tc.tool == SculptBrushes::DRAW) {
      // Tight store-writeback compare for the bit-stable kernel.
      subdiv::GridsStore tmp;
      std::stringstream ss(blobA, std::ios::in | std::ios::out | std::ios::binary);
      TASSERT(tmp.read(ss));
      int S = subdiv::GridsStore::sideForLevel(kLevel), w = S + 1;
      float maxd = 0.0f;
      for (int g = 0; g < mr.store.gridCount(); g++) {
        for (int v = 0; v < w; v++) {
          for (int u = 0; u < w; u++) {
            const float *da = tmp.elem(kLevel, 0, g, u, v);
            const float *db = mr.store.elem(kLevel, 0, g, u, v);
            for (int k = 0; k < 3; k++) {
              float dd = std::fabs(da[k] - db[k]);
              maxd = dd > maxd ? dd : maxd;
            }
          }
        }
      }
      fprintf(stderr, "A/B draw store: max disp diff %.8f\n", maxd);
      TASSERT(maxd <= 1e-6f);
    }
  }

  /* E3 mirror-stamp case: interleaved primary/mirror dab pairs whose query
   * regions share the x=0 seam. The mesh path snapshots co_prev fully per
   * call, so parity proves the region-restricted refresh re-copies a shared
   * leaf for the mirror image after the primary image's writes. */
  {
    Brush brush;
    setupBrush(brush, 0.35f, 0.5f);
    DabBattery mirrored;
    for (int i = 0; i < 4; i++) {
      float y = -0.15f + 0.1f * float(i);
      mirrored.origins.append(float3(0.12f, y, 0.5f));
      mirrored.normals.append(float3(0.0f, 0.0f, 1.0f));
      mirrored.origins.append(float3(-0.12f, y, 0.5f));
      mirrored.normals.append(float3(0.0f, 0.0f, 1.0f));
    }
    restoreStore(mr, s0);
    Vector<float3> posA;
    meshStroke(mr, brush, SculptBrushes::SMOOTH, mirrored, posA);
    restoreStore(mr, s0);
    Vector<float3> posB;
    gridsStroke(mr, brush, SculptBrushes::SMOOTH, mirrored, posB);
    TASSERT(posA.size() == posB.size());
    float diff = maxPosDiff(posA, posB);
    fprintf(stderr, "E3 mirror-stamp smooth: max pos diff %.8f\n", diff);
    TASSERT(diff <= 2e-3f);
  }

  // E1 BSMOOTH A/B: both arms run the plain-Laplacian branch (grids vclass
  // shim is all-zero; the mesh classifier derives zero on this closed,
  // unmarked mesh). Split eps: the normal damping's v.no source differs.
  {
    Brush brush;
    setupBrush(brush, 0.35f, 0.5f);
    restoreStore(mr, s0);
    Vector<float3> posA, norA;
    meshStroke(mr, brush, SculptBrushes::BSMOOTH, dabs, posA, &norA);
    restoreStore(mr, s0);
    Vector<float3> posB;
    gridsStroke(mr, brush, SculptBrushes::BSMOOTH, dabs, posB);
    TASSERT(posA.size() == posB.size());
    float maxTan = 0.0f, maxNor = 0.0f;
    for (int i = 0; i < int(posA.size()); i++) {
      float3 n = norA[i];
      n.normalize();
      float3 dv = posA[i] - posB[i];
      float dn = dv.dot(n);
      float3 dt = dv - n * dn;
      maxNor = std::fmax(maxNor, std::fabs(dn));
      maxTan = std::fmax(maxTan, dt.length());
    }
    fprintf(stderr, "E1 bsmooth A/B: tangent %.8f normal %.8f\n", maxTan, maxNor);
    TASSERT(maxTan <= 2e-3f);
    TASSERT(maxNor <= 5e-2f);
  }

  /* E2 program A/B: a [DRAW, BSMOOTH strength-override] composite (autosmooth's
   * shape) through execProgram vs applyProgram, then again with a strength
   * pressure dynamic configured. Split eps as E1 (BSMOOTH's normal damping
   * reads v.no). The base props must survive both arms — applyProgram's
   * per-entry override rollback. */
  {
    Brush brush;
    setupBrush(brush, 0.35f, 0.5f);
    BrushProgram prog;
    prog.addCommand(int(SculptBrushes::DRAW));
    int smoothIdx = prog.addCommand(int(SculptBrushes::BSMOOTH));
    prog.setCommandFloatByName(smoothIdx, util::string("strength"), 0.25f);

    auto splitCompare = [&](const char *label,
                            Vector<float3> &posA,
                            Vector<float3> &norA,
                            Vector<float3> &posB) {
      TASSERT(posA.size() == posB.size());
      float maxTan = 0.0f, maxNor = 0.0f;
      for (int i = 0; i < int(posA.size()); i++) {
        float3 n = norA[i];
        n.normalize();
        float3 dv = posA[i] - posB[i];
        float dn = dv.dot(n);
        float3 dt = dv - n * dn;
        maxNor = std::fmax(maxNor, std::fabs(dn));
        maxTan = std::fmax(maxTan, dt.length());
      }
      fprintf(stderr, "%s: tangent %.8f normal %.8f\n", label, maxTan, maxNor);
      TASSERT(maxTan <= 2e-3f);
      TASSERT(maxNor <= 5e-2f);
    };

    restoreStore(mr, s0);
    Vector<float3> posA, norA;
    meshProgramStroke(mr, brush, prog, dabs, posA, &norA);
    restoreStore(mr, s0);
    Vector<float3> posB;
    gridsProgramStroke(mr, brush, prog, dabs, posB);
    splitCompare("E2 program A/B", posA, norA, posB);
    TASSERT(std::fabs(brush.props.lookupFloat("strength", 0.0f) - 0.5f) <= 1e-6f);

    // Pressure case: strength MULTIPLY dynamic at pressure 0.6 must resolve
    // identically through both arms (applyProgram runs the same
    // loadCommonProps/loadUniformProps per entry) and actually weaken the dab.
    brush.addPropDynamicByName(util::string("strength"),
                               int(props::DeviceType::PRESSURE),
                               int(BasicMix::MULTIPLY),
                               1.0f);
    brush.clearDeviceInputs();
    brush.pushDeviceInput(int(props::DeviceType::PRESSURE), 0.6f);

    restoreStore(mr, s0);
    Vector<float3> posC, norC;
    meshProgramStroke(mr, brush, prog, dabs, posC, &norC);
    restoreStore(mr, s0);
    Vector<float3> posD;
    gridsProgramStroke(mr, brush, prog, dabs, posD);
    splitCompare("E2 pressure A/B", posC, norC, posD);
    TASSERT(maxPosDiff(posA, posC) > 1e-4f);
    TASSERT(std::fabs(brush.props.lookupFloat("strength", 0.0f) - 0.5f) <= 1e-6f);
  }

  /* E2 program undo: a two-stage program's captured bytes must not scale with
   * the stage count (first-touch leaf capture is shared across entries), and
   * undo/redo restore bit-exact. */
  {
    Brush brush;
    setupBrush(brush, 0.35f, 0.5f);

    restoreStore(mr, s0);
    GridStrokeLog logRef;
    Vector<float3> posRef;
    gridsStroke(mr, brush, SculptBrushes::BSMOOTH, dabs, posRef, &logRef);

    BrushProgram prog;
    prog.addCommand(int(SculptBrushes::DRAW));
    int smoothIdx = prog.addCommand(int(SculptBrushes::BSMOOTH));
    prog.setCommandFloatByName(smoothIdx, util::string("strength"), 0.25f);

    restoreStore(mr, s0);
    const std::string preBlob = storeBlob(mr.store);
    GridLevelDomain *d = mr.gridDomain(kLevel);
    Vector<float3> pre;
    pre.resize(d->vertCount());
    for (int v = 0; v < d->vertCount(); v++) {
      pre[v] = d->pos()[v];
    }

    GridStrokeLog log;
    Vector<float3> post;
    gridsProgramStroke(mr, brush, prog, dabs, post, &log);
    const std::string postBlob = storeBlob(mr.store);

    fprintf(stderr,
            "E2 program undo: %zu bytes (single-stage ref %zu)\n",
            log.bytes(),
            logRef.bytes());
    TASSERT(log.stepCount() == 1);
    TASSERT(log.bytes() <= logRef.bytes() * 3 / 2);

    TASSERT(log.undo());
    Vector<float3> cur;
    cur.resize(d->vertCount());
    for (int v = 0; v < d->vertCount(); v++) {
      cur[v] = d->pos()[v];
    }
    TASSERT(samePosBits(cur, pre));
    TASSERT(storeBlob(mr.store) == preBlob);

    TASSERT(log.redo());
    for (int v = 0; v < d->vertCount(); v++) {
      cur[v] = d->pos()[v];
    }
    TASSERT(samePosBits(cur, post));
    TASSERT(storeBlob(mr.store) == postBlob);
  }
}
