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

/** The materialized-mesh path: stroke `tool` over the active-level slot and
 * fold it into the store. Positions of the level mesh land in `posOut`. */
static void meshStroke(Multires &mr,
                       Brush &brush,
                       SculptBrushes tool,
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
                        GridStrokeLog *log = nullptr)
{
  GridLevelDomain *d = mr.gridDomain(kLevel);
  GridBrushExecutor ex(d, &brush, log);
  ex.beginStep();
  for (int i = 0; i < int(dabs.origins.size()); i++) {
    if (tool == SculptBrushes::GRAB || tool == SculptBrushes::KELVINLET) {
      ex.setGrabAccumAdd(false);
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
  };
  for (const ToolCase &tc : cases) {
    Brush brush;
    setupBrush(brush, 0.25f, 0.5f);
    if (tc.tool == SculptBrushes::CLAY) {
      brush.planeSide = 1.0f;
    }

    restoreStore(mr, s0);
    Vector<float3> posA;
    meshStroke(mr, brush, tc.tool, dabs, posA);
    std::string blobA = storeBlob(mr.store);

    restoreStore(mr, s0);
    Vector<float3> posB;
    gridsStroke(mr, brush, tc.tool, dabs, posB);

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

  fprintf(stderr, "grid stroke gates passed\n");
  /* Skip test_end(): attr name strings stay live in the alloc tracker
   * (mirrors the other subdiv tests). */
  return retval;
}
