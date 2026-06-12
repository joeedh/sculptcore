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
//  - Local-GS parity: the Q1 local Gauss-Seidel tier engages, converges, and
//    leaves every lock decision (per-edge integer translation) identical to the
//    direct path.
//  - Direct rounding: the Q4 one-shot strategy (round everything off the
//    seamless solve, zero greedy rounds) stays feasible on the clean fixtures,
//    and greedy never loses to it on fold count.
//  - Over-constrained fallback: a starved penalty ramp cannot reach integrality;
//    the pass must still return a valid (non-integer) result, not crash.
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "mesh/attribute_builtin.h"
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

void testLocalGsParity()
{
  // Q1 (plans/miq.md): the local Gauss-Seidel tier must engage, drain at least
  // once, and leave every lock decision identical to the direct path — the
  // per-edge integer translations are the decisions, so compare them all.
  Mesh *a = mesh::makeTorus(48, 32, 1.0f, 0.35f);
  Mesh *b = mesh::makeTorus(48, 32, 1.0f, 0.35f);
  auto run = [](Mesh *m, bool local_gs) {
    m->thawTopo();
    mesh::triangulateMesh(*m);
    remesh::CrossFieldParams p;
    remesh::computeCrossField(*m, p);
    remesh::QuantizeParams qp;
    qp.target_edge_length = 0.1f;
    qp.use_local_gs = local_gs;
    return remesh::computeQuantization(*m, qp);
  };
  remesh::QuantizeStats sg = run(a, true);
  remesh::QuantizeStats sd = run(b, false);
  fprintf(stderr,
          "[gs-parity] gs: rounds=%d converged=%d visits=%d touched_max=%d "
          "res=%.3e | direct: res=%.3e\n",
          sg.gs_rounds, sg.gs_converged, sg.gs_visits, sg.gs_touched_max,
          sg.max_integer_residual, sd.max_integer_residual);
  TASSERT(sg.solved && sd.solved);
  TASSERT(sg.feasible && sd.feasible);
  TASSERT(sg.gs_rounds > 0);    // the tier engaged
  TASSERT(sg.gs_converged > 0); // and drained within the cap at least once
  TASSERT(sd.gs_rounds == 0);   // the control really ran the direct path
  // Q2: converged GS rounds re-key only the touched sides; the direct path
  // (and every escalated round) re-keys everything.
  TASSERT(sg.resort_incr > 0);
  TASSERT(sd.resort_incr == 0);
  TASSERT(sd.resort_full > 0);
  BuiltinAttr<litestl::math::int2, ".remesh.e.translation_q", AttrFlag::TEMP> ta, tb;
  ta.ensure(a->e.attrs);
  tb.ensure(b->e.attrs);
  int diffs = 0;
  for (int e : a->e) {
    if (ta[e][0] != tb[e][0] || ta[e][1] != tb[e][1]) {
      diffs++;
    }
  }
  TASSERT(diffs == 0); // identical integer-grid decisions
  litestl::alloc::Delete<Mesh>(a);
  litestl::alloc::Delete<Mesh>(b);
}

void testDirectRounding()
{
  // Q4 (plans/miq.md): DIRECT locks every side at once off the initial seamless
  // solve (zero greedy rounds) and must still reach a feasible integer-grid map
  // on the clean fixtures; GREEDY is the quality default and must never lose to
  // the one-shot path on fold count.
  auto run = [](Mesh *m, float target, bool plain_field,
                remesh::RoundingStrategy strat) {
    m->thawTopo();
    mesh::triangulateMesh(*m);
    remesh::CrossFieldParams p;
    if (plain_field) {
      p.use_sharp_features = false;
      p.use_curvature = false;
    }
    remesh::computeCrossField(*m, p);
    remesh::QuantizeParams qp;
    qp.target_edge_length = target;
    qp.rounding = strat;
    return remesh::computeQuantization(*m, qp);
  };
  struct Fixture {
    const char *name;
    float target;
    bool plain_field;
    Mesh *mg, *md;
  };
  Fixture fixtures[] = {
      {"grid", 0.1f, true, mesh::makeGrid(16, 16, 1.0f), mesh::makeGrid(16, 16, 1.0f)},
      {"cylinder", 0.15f, false, mesh::makeCylinder(32, 8, 0.5f, 2.0f, false),
       mesh::makeCylinder(32, 8, 0.5f, 2.0f, false)},
      {"torus", 0.1f, false, mesh::makeTorus(48, 32, 1.0f, 0.35f),
       mesh::makeTorus(48, 32, 1.0f, 0.35f)},
  };
  for (Fixture &f : fixtures) {
    remesh::QuantizeStats sg = run(f.mg, f.target, f.plain_field,
                                   remesh::RoundingStrategy::GREEDY);
    remesh::QuantizeStats sd = run(f.md, f.target, f.plain_field,
                                   remesh::RoundingStrategy::DIRECT);
    fprintf(stderr,
            "[direct:%s] direct: iters=%d loop=%.3e folds=%d feasible=%d | "
            "greedy: iters=%d folds=%d feasible=%d\n",
            f.name, sd.iters, sd.max_loop_closure, sd.parametrization_folds,
            sd.feasible, sg.iters, sg.parametrization_folds, sg.feasible);
    // DIRECT really took the one-shot path and still produced a valid map.
    TASSERT(sd.solved);
    TASSERT(sd.iters == 0);
    TASSERT(sd.feasible);
    TASSERT(sd.max_integer_residual < 1e-3);
    TASSERT(sd.max_loop_closure < 1e-6);
    // The oracle: greedy must never be worse than direct.
    TASSERT(sg.solved && sg.feasible);
    TASSERT(sg.parametrization_folds <= sd.parametrization_folds);
    litestl::alloc::Delete<Mesh>(f.mg);
    litestl::alloc::Delete<Mesh>(f.md);
  }
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
  testLocalGsParity();
  testDirectRounding();
  testOverConstrainedFallback();
  return retval;
}
