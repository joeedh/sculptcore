// Tier 3 test: automatic curvature-density + gradation limiting (density.cc).
//
//  - Auto-density: a flat grid has ~zero curvature so its density clamps to the
//    floor; a small (high-curvature) sphere clamps to the ceiling. This brackets
//    the s = k·L → density law independent of the estimator's absolute
//    magnitude calibration (only the direction + clamps are asserted).
//  - Gradation: a sharp density step planted across a flat grid; the Alauzet
//    limiter must drop the worst adjacent-vertex density ratio (the size field
//    smears over several rings) while preserving the fine region's peak density.
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "remesh/field/density.h"

#include <cmath>
#include <cstdio>

test_init;

// The shared test_assert macro has a known retval=0-on-failure bug; use a local
// one that flips retval (mirrors test_remesh_curvature.cc).
#define TASSERT(expr)                                                                    \
  do {                                                                                   \
    if (!(expr)) {                                                                       \
      retval = 1;                                                                        \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                  \
      fflush(stderr);                                                                    \
    }                                                                                    \
  } while (0)

using namespace sculptcore;
using namespace sculptcore::mesh;
using litestl::math::float3;

namespace {

// Mean .remesh.v.density over all verts (the generated field is uniform per
// constant-curvature fixture, so the mean is a faithful summary).
double meanDensity(Mesh &m)
{
  BuiltinAttr<float, ".remesh.v.density"> density;
  density.ensure(m.v.attrs);
  double s = 0.0;
  int n = 0;
  for (int v : m.v) {
    s += density[v];
    n++;
  }
  return n > 0 ? s / n : 0.0;
}

// Worst max/min density ratio across any edge (the per-edge size discontinuity
// the gradation limiter bounds).
double worstAdjacentRatio(Mesh &m)
{
  BuiltinAttr<float, ".remesh.v.density"> density;
  density.ensure(m.v.attrs);
  double worst = 1.0;
  for (int e : m.e) {
    float a = density[m.e.vs[e][0]], b = density[m.e.vs[e][1]];
    float lo = a < b ? a : b, hi = a < b ? b : a;
    if (lo > 1e-12f) {
      double r = double(hi) / double(lo);
      if (r > worst) {
        worst = r;
      }
    }
  }
  return worst;
}

void testAutoDensity()
{
  const float L = 0.1f;
  remesh::DensityParams dp;
  dp.target_edge_length = L;
  dp.density_min = 0.25f;
  dp.density_max = 4.0f;

  // Flat grid: k ~ 0 -> s ~ 0 -> density clamps to the floor.
  Mesh *grid = mesh::makeGrid(24, 24, 1.0f);
  remesh::generateAutoDensity(*grid, dp);
  double gridMean = meanDensity(*grid);
  fprintf(stderr, "[auto-density] grid mean=%.4f (min=%.2f)\n", gridMean, dp.density_min);
  TASSERT(gridMean < dp.density_min + 0.05); // ~ at the floor

  // Small (high-curvature) sphere: k ~ 1/R large -> density clamps to the ceiling.
  Mesh *sph = mesh::makeUVSphere(24, 32, 0.03f);
  remesh::generateAutoDensity(*sph, dp);
  double sphMean = meanDensity(*sph);
  fprintf(stderr,
          "[auto-density] small-sphere mean=%.4f (max=%.2f)\n",
          sphMean,
          dp.density_max);
  TASSERT(sphMean > dp.density_max - 0.4); // ~ at the ceiling
  TASSERT(sphMean > gridMean + 0.1);       // curvature raises density

  litestl::alloc::Delete<Mesh>(grid);
  litestl::alloc::Delete<Mesh>(sph);
}

void testGradationStep()
{
  const float L = 0.1f;
  Mesh *grid = mesh::makeGrid(24, 24, 1.0f);

  // Plant a sharp density step: fine (8) on the x<0 half, coarse (1) on x>=0.
  BuiltinAttr<float, ".remesh.v.density"> density;
  density.ensure(grid->v.attrs);
  for (int v : grid->v) {
    density[v] = grid->v.co[v][0] < 0.0f ? 8.0f : 1.0f;
  }

  double before = worstAdjacentRatio(*grid);
  double peakBefore = 8.0;
  fprintf(stderr, "[gradation] worst-ratio before=%.4f\n", before);
  TASSERT(before > 7.0); // the planted step is a hard 8:1 jump

  remesh::limitDensityGradation(*grid,
                                L,
                                /*gradation=*/0.5f,
                                /*iters=*/30,
                                /*density_min=*/0.25f,
                                /*density_max=*/8.0f);

  double after = worstAdjacentRatio(*grid);
  // The fine region's peak density must survive (limiter only refines, never
  // coarsens) — check the x<0 interior still carries high density somewhere.
  double peakAfter = 0.0;
  for (int v : grid->v) {
    if (grid->v.co[v][0] < -0.3f && density[v] > peakAfter) {
      peakAfter = density[v];
    }
  }
  fprintf(
      stderr, "[gradation] worst-ratio after=%.4f peak after=%.4f\n", after, peakAfter);

  TASSERT(after < before);               // the step smeared
  TASSERT(after < 0.6 * before);         // by a meaningful margin
  TASSERT(peakAfter > 0.9 * peakBefore); // fine region preserved

  litestl::alloc::Delete<Mesh>(grid);
}

} // namespace

int main()
{
  testAutoDensity();
  testGradationStep();
  return retval;
}
