/* Shared by the test_grid_stroke_*.cc gates: the c-api surface under test, the
 * displaced-cube fixture and the stroke helpers. One executable, one main
 * (test_grid_stroke.cc) — the gates run in a fixed order over one fixture. */
#pragma once

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


/* Grids stroke session c-api (brush/c-api/grid_stroke_c_api.cc). */
struct GridStrokeSession;
extern "C" {
void sc_external_draw_register_grids(unsigned int object_key, void *multires, int level);
void sc_external_draw_unregister(unsigned int object_key);
void sc_external_draw_update(unsigned int object_key);
const ScExternalDrawProvider *sc_external_draw_provider(void);
GridStrokeSession *
GridStroke_new(sculptcore::subdiv::Multires *mr, int level, sculptcore::brush::Brush *b);
void GridStroke_free(GridStrokeSession *s);
int GridStroke_supported(sculptcore::subdiv::Multires *mr, int tool);
int GridStroke_begin(GridStrokeSession *s);
int GridStroke_dab(GridStrokeSession *s,
                   int tool,
                   float ox,
                   float oy,
                   float oz,
                   float nx,
                   float ny,
                   float nz,
                   int grabAdd);
int GridStroke_dabProgram(GridStrokeSession *s,
                          sculptcore::brush::BrushProgram *prog,
                          float ox,
                          float oy,
                          float oz,
                          float nx,
                          float ny,
                          float nz);
int GridStroke_dabBatchProgram(GridStrokeSession *s,
                               sculptcore::brush::BrushProgram *prog,
                               int n,
                               const float *dabs,
                               float strength,
                               int invert,
                               float pressure,
                               int usePressure,
                               const float *signs,
                               int mirrorCount);
int MeshStroke_dabBatchProgram(sculptcore::brush::CommandExecutor *exec,
                               sculptcore::spatial::SpatialTree *tree,
                               sculptcore::mesh::Mesh *m,
                               sculptcore::brush::Brush *b,
                               sculptcore::brush::BrushProgram *prog,
                               int n,
                               const float *dabs,
                               float strength,
                               int invert,
                               float pressure,
                               int usePressure,
                               float filterMul,
                               const float *signs,
                               int mirrorCount);
void GridStroke_end(GridStrokeSession *s);
int GridStroke_sync(GridStrokeSession *s);
int GridStroke_undo(GridStrokeSession *s);
int GridStroke_redo(GridStrokeSession *s);
double GridStroke_undoBytes(GridStrokeSession *s);
int GridTree_castRay(sculptcore::subdiv::Multires *mr,
                     int level,
                     float ox,
                     float oy,
                     float oz,
                     float dx,
                     float dy,
                     float dz,
                     float *out10,
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

constexpr int kLevel = 3;

/* Smooth per-vert displacement field (mirrors test_grid_domain.cc). */
inline void injectDisp(Multires &mr)
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

inline std::string storeBlob(subdiv::GridsStore &store)
{
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  store.write(ss);
  return ss.str();
}

inline void restoreStore(Multires &mr, const std::string &blob)
{
  std::stringstream ss(blob, std::ios::in | std::ios::out | std::ios::binary);
  bool ok = mr.store.read(ss);
  TASSERT(ok);
  mr.invalidateAll();
}

inline void setupBrush(Brush &b, float radius, float strength)
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

inline DabBattery topFaceBattery()
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
inline void meshStroke(Multires &mr,
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
inline void gridsStroke(Multires &mr,
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
inline void meshProgramStroke(Multires &mr,
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
inline void gridsProgramStroke(Multires &mr,
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

inline float maxPosDiff(const Vector<float3> &a, const Vector<float3> &b)
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
inline bool sameColor(const float4 &a, const float4 &b)
{
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

// Non-const refs: litestl Vector::data() has no const overload.
inline bool samePosBits(Vector<float3> &a, Vector<float3> &b)
{
  if (a.size() != b.size()) {
    return false;
  }
  return std::memcmp(a.data(), b.data(), a.size() * sizeof(float3)) == 0;
}

/* The displaced cube every gate strokes: `s0` is its pristine store blob,
 * restored between gates that need it. */
struct GridStrokeFixture {
  Mesh *cage = nullptr;
  Multires mr;
  std::string s0;
  DabBattery dabs;
};

void gridStrokeAB(GridStrokeFixture &fx);
void gridStrokeUndo(GridStrokeFixture &fx);
void gridStrokeLayers(GridStrokeFixture &fx);
void gridStrokeSession(GridStrokeFixture &fx);
void gridStrokeDrawSource(GridStrokeFixture &fx);
void gridStrokeBatch(GridStrokeFixture &fx);
void gridStrokeLevels(GridStrokeFixture &fx);
void gridStrokeAttrs(GridStrokeFixture &fx);
