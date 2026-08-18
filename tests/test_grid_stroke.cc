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
 *     the domain refetched across the fold point. */
#include "test_util.h"

#include "brush/brush.h"
#include "brush/brush_executor.h"
#include "brush/gpu_marshal.h"
#include "brush/grid_executor.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "spatial/c-api/external_draw.h"
#include "spatial/spatial.h"
#include "subdiv/grid_domain.h"
#include "subdiv/grid_draw_source.h"
#include "subdiv/grid_stroke_log.h"
#include "subdiv/grid_tree.h"
#include "subdiv/grids.h"
#include "subdiv/multires.h"
#include "subdiv/subdiv.h"

#include "litestl/math/mix.h"
#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

test_init;

/* Grids stroke session c-api (brush/c-api/grid_stroke_c_api.cc). */
struct GridStrokeSession;
extern "C" {
void sc_external_draw_register_grids(unsigned int object_key, void *multires, int level);
void sc_external_draw_unregister(unsigned int object_key);
void sc_external_draw_update(unsigned int object_key);
const ScExternalDrawProvider *sc_external_draw_provider(void);
GridStrokeSession *GridStroke_new(sculptcore::subdiv::Multires *mr,
                                  int level,
                                  sculptcore::brush::Brush *b);
void GridStroke_free(GridStrokeSession *s);
int GridStroke_supported(int tool);
int GridStroke_begin(GridStrokeSession *s);
int GridStroke_dab(GridStrokeSession *s, int tool, float ox, float oy, float oz,
                   float nx, float ny, float nz, int grabAdd);
int GridStroke_dabProgram(GridStrokeSession *s, sculptcore::brush::BrushProgram *prog,
                          float ox, float oy, float oz, float nx, float ny, float nz);
int GridStroke_dabBatchProgram(GridStrokeSession *s,
                               sculptcore::brush::BrushProgram *prog, int n,
                               const float *dabs, float strength, int invert,
                               float pressure, int usePressure, const float *signs,
                               int mirrorCount);
int MeshStroke_dabBatchProgram(sculptcore::brush::CommandExecutor *exec,
                               sculptcore::spatial::SpatialTree *tree,
                               sculptcore::mesh::Mesh *m,
                               sculptcore::brush::Brush *b,
                               sculptcore::brush::BrushProgram *prog, int n,
                               const float *dabs, float strength, int invert,
                               float pressure, int usePressure, float filterMul,
                               const float *signs, int mirrorCount);
void GridStroke_end(GridStrokeSession *s);
int GridStroke_sync(GridStrokeSession *s);
int GridStroke_undo(GridStrokeSession *s);
int GridStroke_redo(GridStrokeSession *s);
double GridStroke_undoBytes(GridStrokeSession *s);
int GridTree_castRay(sculptcore::subdiv::Multires *mr, int level, float ox, float oy,
                     float oz, float dx, float dy, float dz, float *out10,
                     int *nearestVert);
}

// Local assert that flips retval (the shared test_assert macro has a known
// retval=0-on-failure bug — see tests/test_meshlog_topo.cc:14-21).
#define TASSERT(expr)                                                                    \
  do {                                                                                   \
    if (!(expr)) {                                                                       \
      retval = 1;                                                                        \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                  \
      fflush(stderr);                                                                    \
    }                                                                                    \
  } while (0)

using namespace sculptcore;
using namespace sculptcore::brush;
using namespace sculptcore::mesh;
using namespace litestl;
using namespace litestl::math;
using litestl::util::Vector;
using subdiv::GridLevelDomain;
using subdiv::GridStrokeLog;
using subdiv::Multires;
using subdiv::MultiresSlot;

static constexpr int kLevel = 3;

/* The grids capability table, transcribed independently from the kernels' own
 * annotations — NOT read back from supportsBrush, which is what it grades.
 *
 * GridBrushExecutor::supportsBrush keeps no tool list: a kernel is declined
 * only for a capability the domain lacks — a `face` stage (no face iterator on
 * a grid leaf) or an attr layer this domain cannot bind. `why` records which
 * one, so a mismatch below names the missing capability instead of just a bool.
 *
 * Graded twice, with grid attribute channels off and on: `off` is the pre-P2
 * roster and must never move, `on` is what the session channels widen it to.
 * As later phases land, entries flip to supported and their `why` goes away;
 * nothing here should ever flip the other direction. */
struct RosterGolden {
  SculptBrushes tool;
  bool supported;
  bool supportedWithAttrs;
  const char *why;
};

static void gateGridsRoster()
{
  const RosterGolden golden[] = {
      {SculptBrushes::DRAW, true, true, nullptr},
      {SculptBrushes::INFLATE, true, true, nullptr},
      {SculptBrushes::CLAY, true, true, nullptr},
      {SculptBrushes::PINCH, true, true, nullptr},
      {SculptBrushes::SHARP, true, true, nullptr},
      {SculptBrushes::MASK, true, true, nullptr},
      {SculptBrushes::SMOOTH, true, true, nullptr},
      {SculptBrushes::KELVINLET, true, true, nullptr},
      {SculptBrushes::POSE, true, true, nullptr},
      {SculptBrushes::TEXDRAW, true, true, nullptr},
      {SculptBrushes::SCRAPE, true, true, nullptr},
      {SculptBrushes::FILL, true, true, nullptr},
      {SculptBrushes::WINGSCRAPE, true, true, nullptr},
      {SculptBrushes::COLOR, false, true, "color attr layer"},
      {SculptBrushes::POLYGROUP, false, false, "face stage"},
      {SculptBrushes::BSMOOTH, true, true, nullptr},
      {SculptBrushes::GRAB, true, true, nullptr},
      {SculptBrushes::SNAKEHOOK, true, true, nullptr},
      {SculptBrushes::COLORSMOOTH, false, true, "color attr layer"},
      {SculptBrushes::FEATURE_ALIGN, false, false, "crossfield attr layer"},
      {SculptBrushes::LAYERDRAW, false, false, "sculpt-layer attr layer"},
      {SculptBrushes::ENHANCE, false, false, "per-vert displacement attr layer"},
      {SculptBrushes::TEXGRAD, true, true, nullptr},
  };
  // Every built-in id is covered — a new tool must state its answer here.
  TASSERT(int(sizeof(golden) / sizeof(golden[0])) == SculptBrushesBuiltinCount);

  for (int pass = 0; pass < 2; pass++) {
    brush::setGridAttrsEnabled(pass == 1);
    for (const RosterGolden &g : golden) {
      const int id = int(g.tool);
      const bool want = pass ? g.supportedWithAttrs : g.supported;
      const bool got = GridBrushExecutor::supportsBrush(g.tool);
      TASSERT(id >= 0 && id < SculptBrushesBuiltinCount);
      if (got != want) {
        fprintf(stderr,
                "grids roster %s (id %d, attrs %s): supportsBrush=%d, golden %d%s%s\n",
                kBuiltinBrushNames[id], id, pass ? "on" : "off", int(got), int(want),
                g.why ? " — declined for: " : "", g.why ? g.why : "");
      }
      TASSERT(got == want);
      // A face-stage kernel is declined by the generated dispatch itself, so it
      // must not even build a def here (the attr loop never sees it).
      if (builtinBrushFaceMode(id)) {
        Brush scratch;
        GridBrushExecutor::brush_command def;
        const bool handled =
            GridBrushExecutor::createCommandSwitch<AccumLive>(g.tool, &scratch, def);
        TASSERT(!handled);
      }
    }
  }
  brush::setGridAttrsEnabled(false);
}

/* Smooth per-vert displacement field (mirrors test_grid_domain.cc). */
static void injectDisp(Multires &mr)
{
  for (int level = 1; level <= mr.maxLevel(); level++) {
    Vector<float3> base = mr.levelPositions(level);
    subdiv::SubdivLevel &lvl = mr.refiner.levels[level - 1];
    int S = lvl.gridSide, w = S + 1;
    for (int g = 0; g < mr.store.gridCount(); g++) {
      const int *gv = &lvl.gridVerts[g * w * w];
      for (int v = 0; v < w; v++) {
        for (int u = 0; u < w; u++) {
          const float3 &p = base[gv[v * w + u]];
          float *d = mr.store.elem(level, 0, g, u, v);
          d[0] = 0.02f * std::sin(3.0f * p[0]) * std::cos(2.0f * p[1]);
          d[1] = 0.02f * std::sin(2.0f * p[1] + 1.0f) * std::cos(2.5f * p[2]);
          d[2] = 0.025f * std::sin(2.5f * p[2] + 0.5f) * std::cos(3.0f * p[0]);
        }
      }
    }
  }
  mr.invalidateAll();
}

static std::string storeBlob(subdiv::GridsStore &store)
{
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  store.write(ss);
  return ss.str();
}

static void restoreStore(Multires &mr, const std::string &blob)
{
  std::stringstream ss(blob, std::ios::in | std::ios::out | std::ios::binary);
  bool ok = mr.store.read(ss);
  TASSERT(ok);
  mr.invalidateAll();
}

static void setupBrush(Brush &b, float radius, float strength)
{
  b.radius = radius;
  b.strength = strength;
  b.planeoff = 0.0f;
  b.writeProps(); // loadCommonProps re-reads these from props each dab
}

struct DabBattery {
  Vector<float3> origins;
  Vector<float3> normals;
};

static DabBattery topFaceBattery()
{
  DabBattery b;
  for (int i = 0; i < 5; i++) {
    float x = -0.2f + 0.1f * float(i);
    b.origins.append(float3(x, 0.05f, 0.5f));
    b.normals.append(float3(0.0f, 0.0f, 1.0f));
  }
  return b;
}

/** Per-dab host state both paths must set identically (snake hook's grab
 * step); called with the dab index just before the dab executes. */
using DabHook = std::function<void(Brush &, int)>;

/** The materialized-mesh path: stroke `tool` over the active-level slot and
 * fold it into the store. Positions of the level mesh land in `posOut`. */
static void meshStroke(Multires &mr,
                       Brush &brush,
                       SculptBrushes tool,
                       const DabBattery &dabs,
                       Vector<float3> &posOut,
                       Vector<float3> *norOut = nullptr,
                       const DabHook &hook = nullptr)
{
  MultiresSlot *slot = mr.setActiveLevel(kLevel);
  TASSERT(slot && slot->mesh && slot->tree);
  CommandExecutor ex(slot->tree, &brush);
  ex.setStrokeGen(1);
  ex.beginStep(false);
  for (int i = 0; i < int(dabs.origins.size()); i++) {
    if (hook) {
      hook(brush, i);
    }
    Vector<spatial::SpatialNode *> nodes;
    slot->tree->filterNodes(dabs.origins[i], brush.radius, nodes);
    ex.execBrush(slot->mesh, tool, &nodes, dabs.origins[i], dabs.normals[i]);
    // Keep query bounds + normals current, as the native per-frame loop does.
    slot->tree->updateQueries();
    slot->tree->updateNormals();
  }
  ex.endStep();
  posOut.resize(slot->mesh->v.count);
  for (int v = 0; v < slot->mesh->v.count; v++) {
    posOut[v] = slot->mesh->v.co[v];
  }
  if (norOut) {
    norOut->resize(slot->mesh->v.count);
    for (int v = 0; v < slot->mesh->v.count; v++) {
      (*norOut)[v] = slot->mesh->v.no[v];
    }
  }
  mr.writeback(kLevel);
}

/** The grids-native path over a fresh domain + executor. */
static void gridsStroke(Multires &mr,
                        Brush &brush,
                        SculptBrushes tool,
                        const DabBattery &dabs,
                        Vector<float3> &posOut,
                        GridStrokeLog *log = nullptr,
                        const DabHook &hook = nullptr)
{
  GridLevelDomain *d = mr.gridDomain(kLevel);
  GridBrushExecutor ex(d, &brush, log);
  ex.beginStep();
  for (int i = 0; i < int(dabs.origins.size()); i++) {
    if (tool == SculptBrushes::GRAB || tool == SculptBrushes::KELVINLET) {
      ex.setGrabAccumAdd(false);
    }
    if (hook) {
      hook(brush, i);
    }
    ex.applyDab(tool, dabs.origins[i], dabs.normals[i]);
  }
  ex.endStep();
  posOut.resize(d->vertCount());
  for (int v = 0; v < d->vertCount(); v++) {
    posOut[v] = d->pos()[v];
  }
}

/** The materialized-mesh composite path (CommandExecutor::execProgram). */
static void meshProgramStroke(Multires &mr,
                              Brush &brush,
                              BrushProgram &prog,
                              const DabBattery &dabs,
                              Vector<float3> &posOut,
                              Vector<float3> *norOut = nullptr)
{
  MultiresSlot *slot = mr.setActiveLevel(kLevel);
  TASSERT(slot && slot->mesh && slot->tree);
  CommandExecutor ex(slot->tree, &brush);
  ex.setStrokeGen(1);
  ex.beginStep(false);
  for (int i = 0; i < int(dabs.origins.size()); i++) {
    Vector<spatial::SpatialNode *> nodes;
    slot->tree->filterNodes(dabs.origins[i], brush.radius, nodes);
    ex.execProgram(&prog, &nodes, dabs.origins[i], dabs.normals[i]);
    // execProgram leaves isFirstOfStep to its caller (applyDab's job normally).
    ex.clearIsFirstOfStep();
    slot->tree->updateQueries();
    slot->tree->updateNormals();
  }
  ex.endStep();
  posOut.resize(slot->mesh->v.count);
  for (int v = 0; v < slot->mesh->v.count; v++) {
    posOut[v] = slot->mesh->v.co[v];
  }
  if (norOut) {
    norOut->resize(slot->mesh->v.count);
    for (int v = 0; v < slot->mesh->v.count; v++) {
      (*norOut)[v] = slot->mesh->v.no[v];
    }
  }
  mr.writeback(kLevel);
}

/** The grids-native composite path (GridBrushExecutor::applyProgram). */
static void gridsProgramStroke(Multires &mr,
                               Brush &brush,
                               BrushProgram &prog,
                               const DabBattery &dabs,
                               Vector<float3> &posOut,
                               GridStrokeLog *log = nullptr)
{
  GridLevelDomain *d = mr.gridDomain(kLevel);
  GridBrushExecutor ex(d, &brush, log);
  ex.beginStep();
  for (int i = 0; i < int(dabs.origins.size()); i++) {
    ex.applyProgram(&prog, dabs.origins[i], dabs.normals[i]);
  }
  ex.endStep();
  posOut.resize(d->vertCount());
  for (int v = 0; v < d->vertCount(); v++) {
    posOut[v] = d->pos()[v];
  }
}

static float maxPosDiff(const Vector<float3> &a, const Vector<float3> &b)
{
  float maxd = 0.0f;
  for (int i = 0; i < int(a.size()); i++) {
    for (int k = 0; k < 3; k++) {
      float dd = std::fabs(a[i][k] - b[i][k]);
      maxd = dd > maxd ? dd : maxd;
    }
  }
  return maxd;
}

/* litestl's math vectors have no operator==; these comparisons are bit-exact
 * on purpose (both sides come from the same mirror column). */
static bool sameColor(const float4 &a, const float4 &b)
{
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

// Non-const refs: litestl Vector::data() has no const overload.
static bool samePosBits(Vector<float3> &a, Vector<float3> &b)
{
  if (a.size() != b.size()) {
    return false;
  }
  return std::memcmp(a.data(), b.data(), a.size() * sizeof(float3)) == 0;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  gateGridsRoster();

  Mesh *cage = createCube(2, 1.0f);
  Multires mr;
  mr.init(*cage, 3);
  injectDisp(mr);
  const std::string s0 = storeBlob(mr.store);
  const DabBattery dabs = topFaceBattery();

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

    fprintf(stderr, "E2 program undo: %zu bytes (single-stage ref %zu)\n",
            log.bytes(), logRef.bytes());
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
        ex.applyDab(SculptBrushes::DRAW, dabs.origins[i] + float3(xoff, 0, 0),
                    dabs.normals[i]);
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
    TASSERT(!log.undo());        /* pos0 evicted with the front step */
    TASSERT(!log.dropOldest());  /* cursor 0: front step is redo history */
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

    TASSERT(log.undo());
    for (int v = 0; v < d->vertCount(); v++) {
      TASSERT(d->mask[v] == 0.0f);
    }
    TASSERT(log.redo());
    bool same = true;
    for (int v = 0; v < d->vertCount(); v++) {
      same = same && d->mask[v] == maskPost[v];
    }
    TASSERT(same);
    TASSERT(storeBlob(mr.store) == blobPost);
  }

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
    fprintf(stderr, "layer stroke: channel %d max |d| %.6f, ch0 same %d\n", lch,
            layerMax, int(ch0Same));
    TASSERT(layerMax > 1e-4f);
    TASSERT(ch0Same);
    mr.setEditTarget(-1);
  }

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
      mex.execBrush(slot->mesh, SculptBrushes::DRAW, &nodes, float3(0.3f, 0, 0.5f),
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
      same = same &&
             std::memcmp(&d2->pos()[v], &slot->mesh->v.co[v], sizeof(float3)) == 0;
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
    TASSERT(mr.writeback(kLevel) == 0);        /* refused the stale diff */
    TASSERT(storeBlob(mr.store) == post);      /* grids stroke survives */
    TASSERT(!mr.slotStale(kLevel));            /* and the slot was healed */
    bool healed = true;
    for (int v = 0; v < d->vertCount(); v++) {
      healed = healed &&
               std::memcmp(&d->pos()[v], &slot->mesh->v.co[v], sizeof(float3)) == 0;
    }
    TASSERT(healed);

    /* A real mesh-path edit on the healed slot still folds normally. */
    slot->mesh->v.co[0][2] += 0.125f;
    TASSERT(mr.writeback(kLevel) > 0);
  }

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
          const int c4[4] = {gv[cv * w + cu], gv[cv * w + cu + 1],
                             gv[(cv + 1) * w + cu + 1], gv[(cv + 1) * w + cu]};
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
      }
      else {
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
      prevEnv ? setenv("SC_GRIDS_INDEXED", prev.c_str(), 1) : unsetenv("SC_GRIDS_INDEXED");
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
      fprintf(stderr, "draw source small-target: %d nodes, %d dirty\n",
              src.nodeCount(), sub);
      TASSERT(sub > 0 && sub < src.nodeCount());
      GridStroke_free(s2);
      mr.setDrawSource(nullptr);
    }
  }

  /* c-api session smoke: supported-tool dispatch, a stroke through the
   * session, undo/redo, and the domain raycast. */
  {
    Brush brush;
    setupBrush(brush, 0.25f, 0.5f);
    restoreStore(mr, s0);

    TASSERT(GridStroke_supported(int(SculptBrushes::DRAW)) == 1);
    TASSERT(GridStroke_supported(int(SculptBrushes::BSMOOTH)) == 1);

    float out10[10];
    int nearest = -1;
    int hit = GridTree_castRay(&mr, kLevel, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f, -1.0f,
                               out10, &nearest);
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
    int moved =
        GridStroke_dab(s, int(SculptBrushes::DRAW), 0, 0, 0.5f, 0, 0, 1, 0);
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
                               int(BasicMix::MULTIPLY), 1.0f);

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
    int movedA = GridStroke_dabBatchProgram(sa, &prog, n, flat.data(), kStrength,
                                            0, kPressure, 1, signs, 1);
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
      movedB += GridStroke_dabProgram(sb, &prog, dd[0], dd[1], dd[2],
                                      dd[3], dd[4], dd[5]);
      movedB += GridStroke_dabProgram(sb, &prog, dd[0] * signs[0], dd[1] * signs[1],
                                      dd[2] * signs[2], dd[3] * signs[0],
                                      dd[4] * signs[1], dd[5] * signs[2]);
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
                               int(BasicMix::MULTIPLY), 1.0f);

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
      totalA = MeshStroke_dabBatchProgram(&ex, slot->tree, slot->mesh, &brush,
                                          &prog, n, flat.data(), kStrength, 0,
                                          kPressure, 1, 1.0f, signs, 1);
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
        totalB += oneImage(float3(dd[0], dd[1], dd[2]),
                           float3(dd[3], dd[4], dd[5]), dd[6]);
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
      slotA->mesh->v.co[vid] = float3(samples[size_t(i) * 3], samples[size_t(i) * 3 + 1],
                                      samples[size_t(i) * 3 + 2]);
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
        kLevel, reinterpret_cast<const float(*)[3]>(samples.data()), sampleNum);
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
    fprintf(stderr, "normal topology arrays parity: tris=%d same=%d\n", a.triCount,
            int(same));
    TASSERT(same);
  }

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

  /* P2: with grid attribute channels on, a colour stroke paints a session
   * store channel; duplicate boundary samples agree, and undo/redo restores
   * the channel bit-exactly. */
  {
    setGridAttrsEnabled(true);
    restoreStore(mr, s0);
    TASSERT(GridBrushExecutor::supportsBrush(SculptBrushes::COLOR));

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
    TASSERT(!mr.store.channelPersist(cch));
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

    setGridAttrsEnabled(false);
    restoreStore(mr, s0);
  }

  /* P4: the derived samples the draw path reads are the session channel's
   * mirror. Seeded from the cage so paint starts on the surface's own colour,
   * republished per dab (the store only learns of the stroke at the fold), and
   * re-overlaid after a rebuild or an undo. */
  {
    setGridAttrsEnabled(true);
    restoreStore(mr, s0);

    /* A distinct colour per cage vertex: a seeded sample can then never be
     * mistaken for the zeros a fresh channel would hold. */
    AttrRef &cref = cage->v.attrs.ensure(mesh::AttrType::FLOAT4, "color", /*materialize=*/true);
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
    fprintf(stderr, "grid attr colour: %d of %d samples published per dab\n",
            movedSamples, int(nSamples));
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

    setGridAttrsEnabled(false);
    restoreStore(mr, s0);
  }

  fprintf(stderr, "grid stroke gates passed\n");
  /* Skip test_end(): attr name strings stay live in the alloc tracker
   * (mirrors the other subdiv tests). */
  return retval;
}
