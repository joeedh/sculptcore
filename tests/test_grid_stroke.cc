/* Grids-native brush path, G2 gate (grid_executor.h / grid_stroke_log.h).
 * On a displaced cube cage at level 3:
 *   - per-kernel A/B vs the materialized path: the same dab battery through
 *     CommandExecutor (mesh slot) and GridBrushExecutor (domain) — positions
 *     near-bit-exact for position-only kernels (draw, plane, pinch, sharp),
 *     tolerance for normal-consuming (inflate) and neighbor-order-sensitive
 *     (smooth) kernels; the draw store writeback compares element-wise tight;
 *   - undo fidelity: store blob + positions restore BIT-exact through
 *     GridStrokeLog undo, and redo restores the post state likewise;
 *   - mask stroke round-trip through the store channel (undo/redo included);
 *   - layer interaction: an edit-target stroke lands in the layer's channel,
 *     channel 0 byte-identical;
 *   - grab functional: an anchored grab stroke moves verts and undoes clean;
 *   - interleaving: grids stroke -> mesh-path stroke -> grids stroke, with
 *     the domain refetched across the fold point.
 * The gates live in test_grid_stroke_*.cc, one function each over the shared
 * fixture (test_grid_stroke_common.h); main() runs them in a fixed order, so
 * a gate may rely on the store state the previous one left behind. */
#include "test_grid_stroke_common.h"

test_init;

/* The grids capability table, transcribed independently from the kernels' own
 * annotations — NOT read back from supportsBrush, which is what it grades.
 *
 * GridBrushExecutor::supportsBrush keeps no tool list: a kernel is declined
 * only for a capability the domain lacks, which is now exactly one thing — an
 * attr layer this domain cannot bind. `why` records it, so a mismatch below
 * names the missing capability instead of just a bool.
 *
 * One column since P5 deleted the kill switch: the roster is what the kernel
 * metadata says, full stop. Three built-ins still decline, each for a named
 * capability this domain lacks rather than for being itself -- so the count
 * below is the ledger P5 leaves behind: 20 of 23 run grids-native. Entries may
 * flip to supported as those capabilities land; nothing here should ever flip
 * the other direction. The table is the session-free answer: LAYERDRAW's
 * decline is conditional, and flips under a live sculpt-layer edit target
 * (asserted at the end of gateGridsRoster). */
struct RosterGolden {
  SculptBrushes tool;
  bool supported;
  const char *why;
};

/* C2: the storage class decides the route -- since P5, it is the only thing
 * that does. With a live
 * stack to ask, an attr-writing kernel binds grids-native only where the host
 * declared it can store that attribute per grid element; otherwise the cage is
 * the author and the mesh path is the route home (grid_attr_bind.h). */
static void gateAttrPlanStorage()
{
  // Its own stack: the gate declares host attrs, which must not leak into the
  // A/B batteries below.
  Mesh *cage = createCube(2, 1.0f);
  Multires mr;
  mr.init(*cage, 1);

  struct Case {
    SculptBrushes tool;
    const char *layer;
    AttrType type;
  };
  const Case cases[] = {
      {SculptBrushes::COLOR, "color", AttrType::FLOAT4},
      {SculptBrushes::COLORSMOOTH, "color", AttrType::FLOAT4},
      {SculptBrushes::POLYGROUP, "group", AttrType::INT},
  };
  subdiv::MultiresAttrs &attrs = mr.gridAttrs();

  for (const Case &c : cases) {
    // Derived (nothing declared): refused, and since P5 there is no switch
    // that could say otherwise.
    TASSERT(attrs.storageFor(c.layer, c.type, mesh::AttrFlag::NONE) ==
            subdiv::GridAttrStorage::Derived);
    TASSERT(!GridBrushExecutor::supportsBrush(c.tool, &attrs));
    // Temp: engine scratch the host never stores, so per-element writes are
    // always allowed — and that answer must not depend on the declaration.
    TASSERT(attrs.storageFor(c.layer, c.type, mesh::AttrFlag::TEMP) ==
            subdiv::GridAttrStorage::Temp);
    // No stack to ask: the pre-session probe keeps the type/domain answer.
    TASSERT(GridBrushExecutor::supportsBrush(c.tool, nullptr));
  }

  // Host: a host that does carry multires attributes gets the grids route.
  attrs.declareHostAttr("color", AttrType::FLOAT4);
  attrs.declareHostAttr("group", AttrType::INT);
  for (const Case &c : cases) {
    TASSERT(attrs.storageFor(c.layer, c.type, mesh::AttrFlag::NONE) ==
            subdiv::GridAttrStorage::Host);
    TASSERT(GridBrushExecutor::supportsBrush(c.tool, &attrs));
  }
  // A position kernel is untouched by any of this.
  TASSERT(GridBrushExecutor::supportsBrush(SculptBrushes::DRAW, &attrs));

  // Undeclare: the batteries below share the process and must not inherit a
  // host capability this one invented.
  attrs.clearHostAttrs();
  for (const Case &c : cases) {
    TASSERT(!GridBrushExecutor::supportsBrush(c.tool, &attrs));
  }
  alloc::Delete(cage);
}

static void gateGridsRoster()
{
  const RosterGolden golden[] = {
      {SculptBrushes::DRAW, true, nullptr},
      {SculptBrushes::INFLATE, true, nullptr},
      {SculptBrushes::CLAY, true, nullptr},
      {SculptBrushes::PINCH, true, nullptr},
      {SculptBrushes::SHARP, true, nullptr},
      {SculptBrushes::MASK, true, nullptr},
      {SculptBrushes::SMOOTH, true, nullptr},
      {SculptBrushes::KELVINLET, true, nullptr},
      {SculptBrushes::POSE, true, nullptr},
      {SculptBrushes::TEXDRAW, true, nullptr},
      {SculptBrushes::SCRAPE, true, nullptr},
      {SculptBrushes::FILL, true, nullptr},
      {SculptBrushes::WINGSCRAPE, true, nullptr},
      {SculptBrushes::COLOR, true, nullptr},
      {SculptBrushes::POLYGROUP, true, nullptr},
      {SculptBrushes::BSMOOTH, true, nullptr},
      {SculptBrushes::GRAB, true, nullptr},
      {SculptBrushes::SNAKEHOOK, true, nullptr},
      {SculptBrushes::COLORSMOOTH, true, nullptr},
      {SculptBrushes::FEATURE_ALIGN, false, "crossfield attr layer"},
      {SculptBrushes::LAYERDRAW, false, "sculpt-layer attr layer"},
      {SculptBrushes::ENHANCE, false, "per-vert displacement attr layer"},
      {SculptBrushes::TEXGRAD, true, nullptr},
      {SculptBrushes::CREASE, true, nullptr},
      {SculptBrushes::BLOB, true, nullptr},
      {SculptBrushes::PLANE, true, nullptr},
      {SculptBrushes::ROTATE, true, nullptr},
  };
  // Every built-in id is covered — a new tool must state its answer here.
  TASSERT(int(sizeof(golden) / sizeof(golden[0])) == SculptBrushesBuiltinCount);

  int native = 0;
  for (const RosterGolden &g : golden) {
    const int id = int(g.tool);
    const bool got = GridBrushExecutor::supportsBrush(g.tool);
    TASSERT(id >= 0 && id < SculptBrushesBuiltinCount);
    if (got != g.supported) {
      fprintf(stderr,
              "grids roster %s (id %d): supportsBrush=%d, golden %d%s%s\n",
              kBuiltinBrushNames[id],
              id,
              int(got),
              int(g.supported),
              g.why ? " declined for: " : "",
              g.why ? g.why : "");
    }
    TASSERT(got == g.supported);
    native += got ? 1 : 0;
    // A face-stage kernel instantiates here like any other: the grids domain
    // has a face iterator (GridFaceIter over each leaf's grids), so the
    // generated dispatch builds its def and only the attr bind can decline it.
    if (builtinBrushFaceMode(id)) {
      Brush scratch;
      GridBrushExecutor::brush_command def;
      const bool handled =
          GridBrushExecutor::createCommandSwitch<AccumLive>(g.tool, &scratch, def);
      TASSERT(handled);
    }
  }
  /* The ledger P5 leaves behind: everything but the three named decliners. */
  TASSERT(native == SculptBrushesBuiltinCount - 3);

  /* Under a live sculpt-layer edit target the LAYERDRAW decline flips -- the
   * write has a channel to land in (LD1) -- while FEATURE_ALIGN and ENHANCE
   * still decline: two decliners, the `- 2` ledger. */
  {
    Mesh *cage = createCube(2, 1.0f);
    Multires mr;
    mr.init(*cage, 1);
    mr.setActiveLevel(1);
    int li = mr.layerAdd();
    TASSERT(li >= 0);
    TASSERT(mr.setEditTarget(li) == li);
    subdiv::MultiresAttrs &attrs = mr.gridAttrs();
    TASSERT(GridBrushExecutor::supportsBrush(SculptBrushes::LAYERDRAW, &attrs));
    int decliners = 0;
    for (const RosterGolden &g : golden) {
      if (!g.supported && !GridBrushExecutor::supportsBrush(g.tool, &attrs)) {
        decliners++;
      }
    }
    TASSERT(decliners == 2);
    alloc::Delete(cage);
  }
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  gateGridsRoster();
  gateAttrPlanStorage();

  GridStrokeFixture fx;
  fx.cage = createCube(2, 1.0f);
  fx.mr.init(*fx.cage, 3);
  injectDisp(fx.mr);
  fx.s0 = storeBlob(fx.mr.store);
  fx.dabs = topFaceBattery();

  gridStrokeAB(fx);
  gridStrokeUndo(fx);
  gridStrokeLayers(fx);
  gridStrokeSession(fx);
  gridStrokeDrawSource(fx);
  gridStrokeBatch(fx);
  gridStrokeLevels(fx);
  gridStrokeAttrs(fx);

  fprintf(stderr, "grid stroke gates passed\n");
  /* Skip test_end(): attr name strings stay live in the alloc tracker
   * (mirrors the other subdiv tests). */
  return retval;
}
