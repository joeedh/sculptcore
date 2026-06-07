// M5 test: integer quantization (spiral elimination).
//
//  - Grid: a flat singularity-free disk — every cut translation is already ~0,
//    so quantization is trivially integral and every one-ring loop closes.
//  - Cylinder (uncapped tube): one homology loop carries a genuine non-integer
//    translation that quantization must snap; integrality + loop closure prove
//    the integer-grid map is valid.
//  - Torus: two homology loops, the hardest singularity-free case; both must
//    snap to integers and every one-ring must close (no spirals).
//  - Determinism: two identical tori quantize to identical diagnostics.
//  - Over-constrained fallback: a starved penalty ramp cannot reach integrality;
//    the pass must still return a valid (non-integer) result, not crash.
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "mesh/utils/triangulate.h"
#include "remesh/field/cross_field.h"
#include "remesh/quantize/quantize_ilp.h"

#include <cmath>
#include <cstdio>

test_init;

#define TASSERT(expr)                                                                     \
  do {                                                                                    \
    if (!(expr)) {                                                                        \
      retval = 1;                                                                         \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                   \
      fflush(stderr);                                                                     \
    }                                                                                     \
  } while (0)

using namespace sculptcore;
using namespace sculptcore::mesh;

namespace {

remesh::QuantizeStats runQuantize(Mesh *m, float target, double tol = 1e-3)
{
  m->thawTopo();
  mesh::triangulateMesh(*m);
  remesh::CrossFieldParams p;
  remesh::computeCrossField(*m, p);
  remesh::QuantizeParams qp;
  qp.target_edge_length = target;
  qp.integer_tol = tol;
  return remesh::computeQuantization(*m, qp);
}

void testGridQuantize()
{
  Mesh *g = mesh::makeGrid(16, 16, 1.0f);
  g->thawTopo();
  mesh::triangulateMesh(*g);
  remesh::CrossFieldParams p;
  p.use_sharp_features = false;
  p.use_curvature = false;
  remesh::computeCrossField(*g, p);
  remesh::QuantizeParams qp;
  qp.target_edge_length = 0.1f;
  remesh::QuantizeStats st = remesh::computeQuantization(*g, qp);

  fprintf(stderr,
          "[grid] classes=%d cut=%d int_res=%.3e loop=%.3e min_jac=%.4f iters=%d "
          "solved=%d feasible=%d\n",
          st.num_classes, st.num_cut_edges, st.max_integer_residual,
          st.max_loop_closure, st.min_jacobian, st.iters, st.solved, st.feasible);
  TASSERT(st.solved);
  TASSERT(st.num_classes > 0);
  TASSERT(st.feasible);
  TASSERT(st.max_integer_residual < 1e-3);
  TASSERT(st.max_loop_closure < 1e-6); // integer loops close exactly
  litestl::alloc::Delete<Mesh>(g);
}

void testCylinderQuantize()
{
  Mesh *c = mesh::makeCylinder(32, 8, 0.5f, 2.0f, /*capped=*/false);
  remesh::QuantizeStats st = runQuantize(c, 0.15f);
  fprintf(stderr,
          "[cylinder] classes=%d cut=%d int_res=%.3e loop=%.3e min_jac=%.4f iters=%d "
          "solved=%d feasible=%d\n",
          st.num_classes, st.num_cut_edges, st.max_integer_residual,
          st.max_loop_closure, st.min_jacobian, st.iters, st.solved, st.feasible);
  TASSERT(st.solved);
  TASSERT(st.feasible);
  TASSERT(st.max_integer_residual < 1e-3);
  TASSERT(st.max_loop_closure < 1e-6);
  litestl::alloc::Delete<Mesh>(c);
}

void testTorusQuantize()
{
  Mesh *t = mesh::makeTorus(48, 32, 1.0f, 0.35f);
  remesh::QuantizeStats st = runQuantize(t, 0.1f);
  fprintf(stderr,
          "[torus] classes=%d cut=%d int_res=%.3e loop=%.3e min_jac=%.4f iters=%d "
          "solved=%d feasible=%d\n",
          st.num_classes, st.num_cut_edges, st.max_integer_residual,
          st.max_loop_closure, st.min_jacobian, st.iters, st.solved, st.feasible);
  TASSERT(st.solved);
  TASSERT(st.feasible);
  TASSERT(st.max_integer_residual < 1e-3);
  TASSERT(st.max_loop_closure < 1e-6);
  litestl::alloc::Delete<Mesh>(t);
}

void testDeterminism()
{
  Mesh *a = mesh::makeTorus(48, 32, 1.0f, 0.35f);
  Mesh *b = mesh::makeTorus(48, 32, 1.0f, 0.35f);
  remesh::QuantizeStats sa = runQuantize(a, 0.1f);
  remesh::QuantizeStats sb = runQuantize(b, 0.1f);
  fprintf(stderr, "[determinism] classes(a,b)=(%d,%d) int_res(a,b)=(%.3e,%.3e)\n",
          sa.num_classes, sb.num_classes, sa.max_integer_residual,
          sb.max_integer_residual);
  TASSERT(sa.num_classes == sb.num_classes);
  TASSERT(sa.num_cut_edges == sb.num_cut_edges);
  TASSERT(sa.feasible == sb.feasible);
  TASSERT(std::fabs(sa.max_integer_residual - sb.max_integer_residual) < 1e-12);
  TASSERT(std::fabs(sa.max_loop_closure - sb.max_loop_closure) < 1e-12);
  litestl::alloc::Delete<Mesh>(a);
  litestl::alloc::Delete<Mesh>(b);
}

void testOverConstrainedFallback()
{
  // Starve the penalty ramp (tiny max_lambda, few rounds): the solver cannot pull
  // the torus's genuine homology translations onto integers, so it must report
  // feasible=false yet still return a usable, finite, non-crashing result.
  Mesh *t = mesh::makeTorus(48, 32, 1.0f, 0.35f);
  t->thawTopo();
  mesh::triangulateMesh(*t);
  remesh::CrossFieldParams p;
  remesh::computeCrossField(*t, p);
  remesh::QuantizeParams qp;
  qp.target_edge_length = 0.1f;
  qp.max_lambda = 1e-3;
  qp.max_iters = 3;
  remesh::QuantizeStats st = remesh::computeQuantization(*t, qp);
  fprintf(stderr,
          "[overconstrained] cut=%d int_res=%.3e loop=%.3e min_jac=%.4f solved=%d "
          "feasible=%d\n",
          st.num_cut_edges, st.max_integer_residual, st.max_loop_closure,
          st.min_jacobian, st.solved, st.feasible);
  TASSERT(st.solved);            // linear solves still succeed
  TASSERT(!st.feasible);         // integrality unreachable -> fallback path
  TASSERT(st.num_cut_edges > 0); // there were sides to quantize
  TASSERT(std::isfinite(st.max_integer_residual)); // valid, non-crashing result
  litestl::alloc::Delete<Mesh>(t);
}

} // namespace

int main()
{
  testGridQuantize();
  testCylinderQuantize();
  testTorusQuantize();
  testDeterminism();
  testOverConstrainedFallback();
  return retval;
}
