// M4 test: seamless parametrization.
//
//  - Grid: a flat grid carries a uniform cross field with no singularities, so
//    the seamless (u,v) is a clean linear ramp — its gradient must align with the
//    field (tiny angle error), every non-cut transition must be a pure rotation
//    (zero translation), and the map must be locally injective (positive
//    Jacobian everywhere, since there are no singularities).
//  - Cylinder (uncapped tube): a non-simply-connected disk forces a homology cut,
//    yet the field is still singularity-free — non-cut transitions stay pure
//    rotations and the Jacobian stays positive.
//  - Torus: genus-1, two homology cuts; only the frame-aligned gradient and the
//    pure-rotation non-cut transitions are asserted (curvature distorts area).
//  - Determinism: two identical grids parametrize to identical diagnostics.
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "mesh/utils/triangulate.h"
#include "remesh/field/cross_field.h"
#include "remesh/param/cut_graph.h"
#include "remesh/param/seamless_param.h"

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

void testGridParam()
{
  Mesh *g = mesh::makeGrid(16, 16, 1.0f);
  g->thawTopo();
  mesh::triangulateMesh(*g);

  remesh::CrossFieldParams p;
  p.use_sharp_features = false;
  p.use_curvature = false;
  remesh::computeCrossField(*g, p);

  remesh::SeamlessParamParams spp;
  spp.target_edge_length = 0.1f;
  remesh::SeamlessParamStats st = remesh::computeSeamlessParam(*g, spp);

  fprintf(stderr,
          "[grid] faces=%d corners=%d classes=%d cut=%d grad_err=%.6f "
          "seam_t=%.3e min_jac=%.6f solved=%d\n",
          st.num_faces, st.num_corners, st.num_classes, st.num_cut_edges,
          st.grad_angle_err, st.max_seam_translation, st.min_jacobian, st.solved);
  TASSERT(st.solved);
  TASSERT(st.num_classes > 0);
  TASSERT(st.grad_angle_err < 0.05);       // gradient ∥ cross field
  TASSERT(st.max_seam_translation < 1e-3); // non-cut transitions are pure rotation
  TASSERT(st.min_jacobian > 0.0);          // locally injective (no singularities)
  litestl::alloc::Delete<Mesh>(g);
}

void testCylinderParam()
{
  Mesh *c = mesh::makeCylinder(32, 8, 0.5f, 2.0f, /*capped=*/false);
  c->thawTopo();
  mesh::triangulateMesh(*c);

  remesh::CrossFieldParams p;
  remesh::computeCrossField(*c, p);

  remesh::SeamlessParamParams spp;
  spp.target_edge_length = 0.15f;
  remesh::SeamlessParamStats st = remesh::computeSeamlessParam(*c, spp);

  fprintf(stderr,
          "[cylinder] faces=%d classes=%d cut=%d grad_err=%.6f seam_t=%.3e "
          "min_jac=%.6f solved=%d\n",
          st.num_faces, st.num_classes, st.num_cut_edges, st.grad_angle_err,
          st.max_seam_translation, st.min_jacobian, st.solved);
  TASSERT(st.solved);
  TASSERT(st.grad_angle_err < 0.1);
  TASSERT(st.max_seam_translation < 1e-2);
  TASSERT(st.min_jacobian > 0.0); // tube is singularity-free
  litestl::alloc::Delete<Mesh>(c);
}

void testTorusParam()
{
  Mesh *t = mesh::makeTorus(48, 32, 1.0f, 0.35f);
  t->thawTopo();
  mesh::triangulateMesh(*t);

  remesh::CrossFieldParams p;
  remesh::computeCrossField(*t, p);

  remesh::SeamlessParamParams spp;
  spp.target_edge_length = 0.1f;
  remesh::SeamlessParamStats st = remesh::computeSeamlessParam(*t, spp);

  fprintf(stderr,
          "[torus] faces=%d classes=%d cut=%d grad_err=%.6f seam_t=%.3e "
          "min_jac=%.6f solved=%d\n",
          st.num_faces, st.num_classes, st.num_cut_edges, st.grad_angle_err,
          st.max_seam_translation, st.min_jacobian, st.solved);
  TASSERT(st.solved);
  TASSERT(st.grad_angle_err < 0.15);
  TASSERT(st.max_seam_translation < 1e-2);
  litestl::alloc::Delete<Mesh>(t);
}

// Regression: the cut graph must be a dual spanning *forest* — a single-rooted
// DFS left every other component fully cut, and a pole in a small accessory
// component used to leave the main body fully cut (3x the quantize sides).
void testCutGraphSpansComponents()
{
  using litestl::math::float3;

  Mesh *m = mesh::makeGrid(8, 8, 1.0f);
  m->thawTopo();

  // Far-away second component: a 4x4-quad patch.
  const int N = 5;
  int vid[N][N];
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < N; j++) {
      vid[i][j] = m->make_vertex(float3(100.0f + float(i), float(j), 0.0f));
    }
  }
  for (int i = 0; i + 1 < N; i++) {
    for (int j = 0; j + 1 < N; j++) {
      litestl::util::Vector<int> vs;
      vs.append(vid[i][j]);
      vs.append(vid[i + 1][j]);
      vs.append(vid[i + 1][j + 1]);
      vs.append(vid[i][j + 1]);
      m->make_face(vs);
    }
  }
  mesh::triangulateMesh(*m);

  int interior = 0;
  for (int e : m->e) {
    int c1 = m->e.c[e];
    if (c1 == ELEM_NONE) {
      continue;
    }
    int c2 = m->c.radial_next[c1];
    if (c2 == c1 || m->c.radial_next[c2] != c1) {
      continue;
    }
    if (m->l.f[m->c.l[c1]] != m->l.f[m->c.l[c2]]) {
      interior++;
    }
  }
  const int want_tree = m->f.count - 2; // one tree per component

  // No pole field: pass-2 rooting must still span both components.
  remesh::CutGraphStats st = remesh::buildCutGraph(*m);
  fprintf(stderr, "[cutgraph] interior=%d tree=%d cut=%d (no poles)\n", interior,
          st.num_tree_edges, st.num_cut_edges);
  TASSERT(st.num_tree_edges == want_tree);
  TASSERT(st.num_tree_edges + st.num_cut_edges == interior);

  // The regression case: the only singular vertex sits in the small far
  // component; rooting there must not leave the main grid fully cut.
  BuiltinAttr<short, ".remesh.v.pole_index"> pole;
  pole.ensure(m->v.attrs);
  for (int v : m->v) {
    pole[v] = 0;
  }
  pole[vid[2][2]] = 1;
  st = remesh::buildCutGraph(*m);
  fprintf(stderr,
          "[cutgraph] interior=%d tree=%d cut=%d sing=%d (pole in far component)\n",
          interior, st.num_tree_edges, st.num_cut_edges, st.num_singularities);
  TASSERT(st.num_singularities == 1);
  TASSERT(st.num_tree_edges == want_tree);
  TASSERT(st.num_tree_edges + st.num_cut_edges == interior);

  litestl::alloc::Delete<Mesh>(m);
}

void testDeterminism()
{
  Mesh *a = mesh::makeGrid(16, 16, 1.0f);
  Mesh *b = mesh::makeGrid(16, 16, 1.0f);
  a->thawTopo();
  b->thawTopo();
  mesh::triangulateMesh(*a);
  mesh::triangulateMesh(*b);

  remesh::CrossFieldParams p;
  p.use_sharp_features = false;
  p.use_curvature = false;
  remesh::computeCrossField(*a, p);
  remesh::computeCrossField(*b, p);

  remesh::SeamlessParamParams spp;
  spp.target_edge_length = 0.1f;
  remesh::SeamlessParamStats sa = remesh::computeSeamlessParam(*a, spp);
  remesh::SeamlessParamStats sb = remesh::computeSeamlessParam(*b, spp);

  fprintf(stderr, "[determinism] classes(a,b)=(%d,%d) grad(a,b)=(%.6f,%.6f)\n",
          sa.num_classes, sb.num_classes, sa.grad_angle_err, sb.grad_angle_err);
  TASSERT(sa.num_classes == sb.num_classes);
  TASSERT(std::fabs(sa.grad_angle_err - sb.grad_angle_err) < 1e-9);
  TASSERT(std::fabs(sa.max_seam_translation - sb.max_seam_translation) < 1e-9);
  litestl::alloc::Delete<Mesh>(a);
  litestl::alloc::Delete<Mesh>(b);
}

} // namespace

int main()
{
  testGridParam();
  testCylinderParam();
  testTorusParam();
  testCutGraphSpansComponents();
  testDeterminism();
  return retval;
}
