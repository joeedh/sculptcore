/* Regression for two related Anchored/Drag Dot NW.js crashes:
 *
 * 1. MeshLog::beginPreviewDab() must not assume setActiveMesh() was already
 *    called on this mesh. The JS driver calls beginPreviewDab() as the
 *    literal first MeshLog interaction of an Anchored/Drag Dot stroke,
 *    strictly before the applyDab() that normally triggers setActiveMesh()
 *    (the only place that binds the vertGate_/faceGate_ .strokeid columns).
 *    Every other meshlog test goes through setActiveMesh (or a debug Scene
 *    that calls it for them) before touching the log, so none of them
 *    reproduce this ordering.
 *
 * 2. capturePreviewRegion() sweeps every non-NOCOPY vertex attribute, unlike
 *    a normal brush's AttrSaver capture (which only ever touches co/no).
 *    A custom scalar attribute added via AttrGroup::ensure() defaults to
 *    unmaterialized (materialize=false) until something actually writes a
 *    touched vertex's page, so ChunkElemData::cpyFrom() reading it through
 *    the untyped AttrDataBase::getElemData() got a null source pointer and
 *    crashed in memcpy. */
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"
#include "meshlog/meshlog_base.h"
#include "spatial/spatial.h"

#include <cstdio>

test_init;

/* Local assert that actually flips retval to nonzero on failure
 * (the shared test_assert macro has retval=0 on the failure branch). */
#define TASSERT(expr)                                                                   \
  do {                                                                                  \
    if (!(expr)) {                                                                      \
      retval = 1;                                                                       \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                 \
      fflush(stderr);                                                                   \
    }                                                                                    \
  } while (0)

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace sculptcore::meshlog;
using litestl::math::float3;

namespace {

Mesh *makeTriGrid(int n)
{
  Mesh *m = litestl::alloc::New<Mesh>("test_meshlog_preview_unbound grid");
  litestl::util::Vector<int> grid;
  grid.resize(n * n);
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      float fx = float(x) / float(n - 1) - 0.5f;
      float fy = float(y) / float(n - 1) - 0.5f;
      grid[y * n + x] = m->make_vertex(float3(fx, fy, 0.0f));
    }
  }
  for (int y = 0; y < n - 1; y++) {
    for (int x = 0; x < n - 1; x++) {
      int a = grid[y * n + x], b = grid[y * n + x + 1];
      int c = grid[(y + 1) * n + x + 1], d = grid[(y + 1) * n + x];
      int t0[3] = {a, b, c};
      int t1[3] = {a, c, d};
      m->make_face(std::span<int>(t0, 3));
      m->make_face(std::span<int>(t1, 3));
    }
  }
  return m;
}

} // namespace

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  Mesh *m = makeTriGrid(9);
  spatial::SpatialTree *tree = litestl::alloc::New<spatial::SpatialTree>("test tree", m);
  tree->leaf_limit = 64;
  tree->buildAll();

  MeshLog log;
  log.beginStep(true);

  /* No setActiveMesh() call before this — mirrors Anchored/Drag Dot's real
   * first-dab-of-a-stroke ordering. Used to crash inside capturePreviewRegion
   * (vertGate_.needsData() indexing an unbound .strokeid.vertex column). */
  log.beginPreviewDab(m, tree, float3(0.0f, 0.0f, 0.0f), 0.3f);
  TASSERT(log.previewActive());

  /* A second region (mirrors a symmetry tick) exercises the same unbound-gate
   * path through extendPreviewDab(). */
  log.extendPreviewDab(m, tree, float3(0.2f, 0.2f, 0.0f), 0.3f);
  TASSERT(log.previewActive());

  log.commitPreviewDab();
  TASSERT(!log.previewActive());

  log.endStep();

  /* Second scenario: a sparse, never-materialized custom vertex attribute
   * must not crash capturePreviewRegion's broad attribute sweep. */
  m->v.attrs.ensure(AttrType::FLOAT, "test.sparse_scalar");

  MeshLog log2;
  log2.beginStep(true);
  log2.beginPreviewDab(m, tree, float3(0.0f, 0.0f, 0.0f), 0.3f);
  TASSERT(log2.previewActive());
  log2.extendPreviewDab(m, tree, float3(0.2f, 0.2f, 0.0f), 0.3f);
  TASSERT(log2.previewActive());
  log2.commitPreviewDab();
  TASSERT(!log2.previewActive());
  log2.endStep();

  printf("meshlog_preview_unbound: ok\n");

  /* Don't use test_end() here: linking against spatial pulls in static-init
   * allocations that aren't tagged PermanentGuard and would be reported as
   * leaks. Functional correctness is checked via TASSERT; return retval
   * directly. */
  return retval;
}
