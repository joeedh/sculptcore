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
#include "brush/grid_executor.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "spatial/spatial.h"
#include "subdiv/grid_domain.h"
#include "subdiv/grid_stroke_log.h"
#include "subdiv/grid_tree.h"
#include "subdiv/grids.h"
#include "subdiv/multires.h"
#include "subdiv/subdiv.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

test_init;

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
                       Vector<float3> &posOut)
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

  fprintf(stderr, "grid stroke gates passed\n");
  /* Skip test_end(): attr name strings stay live in the alloc tracker
   * (mirrors the other subdiv tests). */
  return retval;
}
