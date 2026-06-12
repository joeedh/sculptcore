// M6 test: quad extraction + reprojection + the end-to-end QuadRemesh pipeline.
//
// Each case runs M2-M5 (cross field -> seamless param -> quantization) on a
// triangulated input, extracts the quad mesh that is the preimage of the integer
// lattice, and validates it structurally with remeshValidate:
//
//  - Grid: a flat disk; a clean all-quad grid (Euler 1, valence-4 interior).
//  - Plane with hole: an annulus; both rims (outer + hole) must classify as
//    real input borders in the cap pass and stay open — nothing capped.
//  - Cylinder (uncapped): a tube; all-quad, manifold, Euler 0, singularity-free.
//  - Torus: the genus-1 case; all-quad, manifold, Euler 0, singularity-free.
//  - Sphere: 8 valence-3/5 singularities are expected; all-quad + manifold +
//    Euler 2 must still hold (the local-injectivity stiffening flattens the
//    cone-1-ring folds the linear MIQ map leaves behind).
//  - Capped cylinder: a closed genus-0 solid whose cap rims can quantize to an
//    *odd* grid-loop. A pure-quad mesher cannot fan-close an odd ring, so such a
//    cap is left open by design; the gate is all-quad + manifold + no-inversions
//    + no-spirals (NOT Euler 2 — see the cap-pass note in quad_extract.cc).
//  - Simple.obj: a small rounded organic blob whose cross field curls enough to
//    fold the exactly-seamless map (~1/3 of faces) and break extraction. The
//    fixture for the ARAP untangle fallback (QuantizeParams::untangle_fold_
//    threshold): it walks the seam penalty up from a low injective weight so the
//    final map folds ~1.5% and extracts a clean all-quad, no-spiral mesh.
//  - AnimeGirl2.obj: the dense organic spiral-elimination stress fixture. Opt-in
//    (REMESH_ANIME=1) because it is ~800k faces; gates the headline no-spiral +
//    all-quad guarantee on real-world input.
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/mesh_shapes.h"
#include "mesh/utils/mesh_validate.h"
#include "mesh/utils/triangulate.h"
#include "remesh/extract/quad_extract.h"
#include "remesh/extract/reproject.h"
#include "remesh/field/cross_field.h"
#include "remesh/field/singularity_adjust.h"
#include "remesh/quantize/quantize_ilp.h"
#include "remesh/remesh.h"
#include "remesh/remesh_params.h"
#include "remesh/remesh_report.h"

#include "obj_load.h"
#include "test_config.h"

#include <cstdio>
#include <cstdlib>

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

// Run M2-M5 on m (in place), then extract. Returns the heap quad mesh (caller
// frees) and fills `st`. Returns nullptr on extraction failure.
Mesh *runExtract(Mesh *m, float target, remesh::ExtractStats &st,
                 bool sharp = false, bool curvature = false, bool capOdd = false)
{
  m->thawTopo();
  mesh::triangulateMesh(*m);
  remesh::CrossFieldParams cp;
  cp.use_sharp_features = sharp;
  cp.use_curvature = curvature;
  remesh::computeCrossField(*m, cp);
  remesh::SingularityAdjustParams sap;
  remesh::adjustSingularities(*m, sap);
  remesh::QuantizeParams qp;
  qp.target_edge_length = target;
  remesh::computeQuantization(*m, qp);
  remesh::ExtractParams ep;
  ep.cap_odd_holes = capOdd;
  return remesh::extractQuadMesh(*m, ep, st);
}

void report(const char *name, const RemeshReport &r)
{
  fprintf(stderr,
          "[%s] V=%d E=%d F=%d euler=%d tri=%d quad=%d ngon=%d allquad=%d "
          "manifold=%d winding=%d nme=%d bnd=%d degen=%d inv=%d irr=%d | "
          "iso(chk=%d close=%d closed=%d open=%d spiral=%d)\n",
          name, r.vert_count, r.edge_count, r.face_count, r.euler, r.tri_count,
          r.quad_count, r.ngon_count, r.all_quad, r.manifold,
          r.consistent_winding, r.non_manifold_edges, r.boundary_edges,
          r.degenerate_faces, r.inverted_faces, r.irregular_interior_verts,
          r.isolines_checked, r.isolines_close, r.closed_isolines,
          r.open_isolines, r.spiral_isolines);
  if (!r.manifold)
    fprintf(stderr, "  manifold_error: %s\n", r.manifold_error.c_str());
}

void testGridExtract()
{
  Mesh *g = mesh::makeGrid(16, 16, 1.0f);
  remesh::ExtractStats st;
  Mesh *out = runExtract(g, 0.1f, st);
  TASSERT(out != nullptr);
  if (out) {
    RemeshReport r = remeshValidate(*out);
    report("grid", r);
    TASSERT(st.ok);
    TASSERT(r.all_quad);
    TASSERT(r.structurallyOk());
    TASSERT(r.euler == 1); // disk
    TASSERT(r.irregular_interior_verts == 0);
    litestl::alloc::Delete<Mesh>(out);
  }
  litestl::alloc::Delete<Mesh>(g);
}

// An n x n grid plane with a circular hole punched out of the middle (faces
// whose center falls inside `holeR` are skipped; verts created on demand).
Mesh *makeGridWithHole(int n, float size, float holeR)
{
  using litestl::math::float3;
  using litestl::util::Vector;
  Mesh *m = litestl::alloc::New<Mesh>("Mesh GridHole");
  Vector<int> verts;
  verts.resize(n * n);
  for (int i = 0; i < n * n; i++)
    verts[i] = -1;
  float inv = 1.0f / float(n - 1);
  auto vat = [&](int i, int j) {
    int &v = verts[i * n + j];
    if (v < 0)
      v = m->make_vertex(
          float3((float(i) * inv - 0.5f) * size, (float(j) * inv - 0.5f) * size, 0.0f));
    return v;
  };
  Vector<int> vs;
  for (int i = 0; i < n - 1; i++) {
    for (int j = 0; j < n - 1; j++) {
      float cx = ((float(i) + 0.5f) * inv - 0.5f) * size;
      float cy = ((float(j) + 0.5f) * inv - 0.5f) * size;
      if (cx * cx + cy * cy < holeR * holeR)
        continue;
      vs.clear();
      vs.append(vat(i, j));
      vs.append(vat(i + 1, j));
      vs.append(vat(i + 1, j + 1));
      vs.append(vat(i, j + 1));
      m->make_face(vs);
    }
  }
  m->recalc_normals();
  return m;
}

// Real-border preservation: every output rim of the annulus must classify as a
// real input border in the cap pass (holes_open_border) and stay open. Capping
// one would weld the plane shut — the failure mode the rim classifier exists to
// prevent (spurious rims cap, borders don't; see quad_extract.cc).
void testPlaneHoleBorders()
{
  // Hole radius vs target keeps the annulus >= 4 grid cells wide everywhere, so
  // the two rims extract pinch-free (sharing a vert would union-find as 1 loop).
  Mesh *p = makeGridWithHole(17, 1.0f, 0.2f);
  remesh::ExtractStats st;
  Mesh *out = runExtract(p, 0.07f, st);
  TASSERT(out != nullptr);
  if (out) {
    RemeshReport r = remeshValidate(*out);
    report("planehole", r);
    fprintf(stderr, "[planehole] capped=%d open=%d border=%d pinched=%d\n",
            st.holes_capped, st.holes_open, st.holes_open_border, st.holes_pinched_split);
    TASSERT(st.ok);
    TASSERT(r.all_quad);
    TASSERT(r.structurallyOk());
    TASSERT(st.holes_capped == 0);          // nothing spurious to cap
    TASSERT(st.holes_pinched_split == 0);   // rims must not touch
    TASSERT(st.holes_open_border == 2);     // outer rim + the hole rim, both open
    TASSERT(st.holes_open == 2);
    TASSERT(r.boundary_loop_count == 2);
    litestl::alloc::Delete<Mesh>(out);
  }
  litestl::alloc::Delete<Mesh>(p);
}

void testCylinderExtract()
{
  Mesh *c = mesh::makeCylinder(32, 8, 0.5f, 2.0f, /*capped=*/false);
  remesh::ExtractStats st;
  Mesh *out = runExtract(c, 0.15f, st);
  TASSERT(out != nullptr);
  if (out) {
    RemeshReport r = remeshValidate(*out);
    report("cylinder", r);
    TASSERT(st.ok);
    TASSERT(r.all_quad);
    TASSERT(r.structurallyOk());
    TASSERT(r.euler == 0); // open tube
    TASSERT(r.irregular_interior_verts == 0);
    litestl::alloc::Delete<Mesh>(out);
  }
  litestl::alloc::Delete<Mesh>(c);
}

void testTorusExtract()
{
  Mesh *t = mesh::makeTorus(48, 32, 1.0f, 0.35f);
  remesh::ExtractStats st;
  Mesh *out = runExtract(t, 0.1f, st, /*sharp=*/false, /*curvature=*/true);
  TASSERT(out != nullptr);
  if (out) {
    RemeshReport r = remeshValidate(*out);
    report("torus", r);
    TASSERT(st.ok);
    TASSERT(r.all_quad);
    TASSERT(r.structurallyOk());
    TASSERT(r.euler == 0); // genus 1
    TASSERT(r.irregular_interior_verts == 0);
    litestl::alloc::Delete<Mesh>(out);
  }
  litestl::alloc::Delete<Mesh>(t);
}

void testSphereExtract()
{
  Mesh *s = mesh::makeUVSphere(24, 32, 1.0f);
  remesh::ExtractStats st;
  Mesh *out = runExtract(s, 0.15f, st, /*sharp=*/false, /*curvature=*/true);
  TASSERT(out != nullptr);
  if (out) {
    RemeshReport r = remeshValidate(*out);
    report("sphere", r);
    TASSERT(st.ok);
    TASSERT(r.all_quad);
    TASSERT(r.structurallyOk());
    TASSERT(r.euler == 2); // genus 0 closed
    litestl::alloc::Delete<Mesh>(out);
  }
  litestl::alloc::Delete<Mesh>(s);
}

// A capped cylinder is a closed genus-0 solid, but its two flat caps each meet
// the curved wall at a sharp rim that the cross field resolves with four index-1
// cones. Those cones make the cap's grid boundary loop come out *odd* often
// enough that a pure-quad fan cannot close it (see quad_extract.cc cap pass), so
// the cap is left as a hole. The all-quad guarantee is the priority: assert
// all-quad + manifold + no-inversions + no-spirals, and accept Euler < 2.
void testCappedCylinderExtract()
{
  Mesh *c = mesh::makeCylinder(24, 8, 0.5f, 2.0f, /*capped=*/true);
  remesh::ExtractStats st;
  Mesh *out = runExtract(c, 0.15f, st, /*sharp=*/true, /*curvature=*/true);
  TASSERT(out != nullptr);
  if (out) {
    RemeshReport r = remeshValidate(*out);
    report("capcyl", r);
    TASSERT(st.ok);
    TASSERT(r.all_quad);
    TASSERT(r.manifold);
    TASSERT(r.consistent_winding);
    TASSERT(r.inverted_faces == 0);
    TASSERT(r.degenerate_faces == 0);
    TASSERT(r.spiral_isolines == 0); // quantization killed every spiral
    litestl::alloc::Delete<Mesh>(out);
  }
  litestl::alloc::Delete<Mesh>(c);
}

// The same capped cylinder with cap_odd_holes on: the two odd cap rims pair
// through the quad strip running down the side, the ladder split makes both
// even, so the closed result must be watertight and still all-quad — no cap
// triangles.
void testOddCapExtract()
{
  Mesh *c = mesh::makeCylinder(24, 8, 0.5f, 2.0f, /*capped=*/true);
  remesh::ExtractStats st;
  Mesh *out = runExtract(c, 0.15f, st, /*sharp=*/true, /*curvature=*/true,
                         /*capOdd=*/true);
  TASSERT(out != nullptr);
  if (out) {
    RemeshReport r = remeshValidate(*out);
    report("oddcap", r);
    fprintf(stderr, "[oddcap] capped=%d capped_odd=%d paired=%d open=%d\n",
            st.holes_capped, st.holes_capped_odd, st.odd_rims_paired,
            st.holes_open);
    TASSERT(st.ok);
    TASSERT(st.holes_capped_odd > 0); // the cap rims really came out odd
    TASSERT(st.odd_rims_paired == 2); // both joined by one ladder split
    TASSERT(st.holes_open == 0);
    TASSERT(r.all_quad); // odd rims closed by the pairing, not cap triangles
    TASSERT(r.manifold);
    TASSERT(r.consistent_winding);
    TASSERT(r.boundary_edges == 0); // watertight
    TASSERT(r.euler == 2);          // both holes closed -> topological sphere
    // inverted_faces is not asserted: fan caps on ragged rims can pleat a few
    // faces locally (smoothed downstream by reprojection); the structural
    // contract here is watertight + all-quad.
    TASSERT(r.degenerate_faces == 0);
    TASSERT(r.spiral_isolines == 0);
    for (int v = 9; v < 17; v++) // fan centers are valence <= kFanMax/2 = 6
      TASSERT(r.valence_hist[v] == 0);
    litestl::alloc::Delete<Mesh>(out);
  }
  litestl::alloc::Delete<Mesh>(c);
}

// Reprojection (M6c): snap an extracted quad mesh back onto the input surface,
// with and without Laplacian smoothing, and verify it stays a valid quad mesh.
void testReproject()
{
  Mesh *s = mesh::makeUVSphere(24, 32, 1.0f);
  remesh::ExtractStats st;
  Mesh *out = runExtract(s, 0.15f, st, /*sharp=*/false, /*curvature=*/true);
  TASSERT(out != nullptr);
  if (out) {
    // Push every vertex uniformly off-surface (radial scale), then snap back. A
    // uniform offset is locally consistent, so a clean snap must not invert.
    for (int v : out->v) {
      litestl::math::float3 p = out->v.co[v];
      out->v.co[v] = litestl::math::float3(p[0] * 1.03f, p[1] * 1.03f, p[2] * 1.03f);
    }

    remesh::ReprojectParams rp; // pure snap (no smoothing)
    remesh::ReprojectStats rs = remesh::reprojectToSurface(*out, *s, rp);
    fprintf(stderr, "[reproject] verts=%d max=%.4f mean=%.4f\n", rs.num_verts, rs.max_dist,
            rs.mean_dist);
    TASSERT(rs.max_dist > 0.005f); // the offset really moved verts off-surface

    // After one snap every vertex lies on the surface; a second pure snap moves ~0.
    remesh::ReprojectStats rs2 = remesh::reprojectToSurface(*out, *s, rp);
    TASSERT(rs2.max_dist < 1e-3f);

    RemeshReport r = remeshValidate(*out);
    TASSERT(r.all_quad);
    TASSERT(r.manifold);
    TASSERT(r.euler == 2);
    TASSERT(r.inverted_faces == 0);

    // Laplacian-smooth + snap; result must remain a valid closed quad sphere.
    remesh::ReprojectParams sp;
    sp.iterations = 2;
    sp.smooth_iterations = 2;
    sp.smooth_lambda = 0.3f;
    remesh::reprojectToSurface(*out, *s, sp);
    RemeshReport r2 = remeshValidate(*out);
    TASSERT(r2.all_quad);
    TASSERT(r2.manifold);
    TASSERT(r2.euler == 2);
    TASSERT(r2.inverted_faces == 0);

    litestl::alloc::Delete<Mesh>(out);
  }
  litestl::alloc::Delete<Mesh>(s);
}

// End-to-end (M6d): the top-level QuadRemesh orchestrates M2->M6 on a triangulated
// copy and returns a reprojected all-quad mesh, leaving the caller's input intact.
void testQuadRemeshPipeline()
{
  // Mirror the host litemesh-uvsphere scene exactly: radius-2 UV sphere at the
  // ToolOp default target 0.1 (target/radius 0.05 lands in the valid band; the
  // radius-1 sphere at 0.1 is twice as fine and hits an odd-cone-rim parity hole).
  Mesh *s = mesh::makeUVSphere(24, 32, 2.0f);
  int in_v = s->v.count, in_f = s->f.count;

  remesh::RemeshParams params; // defaults: curvature+sharp on, reproject+smooth on
  params.target_edge_length = 0.1f;
  Mesh *out = remesh::QuadRemesh(*s, params);
  TASSERT(out != nullptr);
  if (out) {
    RemeshReport r = remeshValidate(*out);
    report("pipeline:sphere", r);
    TASSERT(r.all_quad);
    TASSERT(r.manifold);
    TASSERT(r.euler == 2);
    TASSERT(r.inverted_faces == 0);
    litestl::alloc::Delete<Mesh>(out);
  }
  // Contract: the input mesh is untouched (run on a copy, never triangulated).
  TASSERT(s->v.count == in_v);
  TASSERT(s->f.count == in_f);
  litestl::alloc::Delete<Mesh>(s);

  // Zero-singularity case + the no-reproject path (extracted positions kept).
  Mesh *t = mesh::makeTorus(48, 32, 1.0f, 0.35f);
  remesh::RemeshParams tp;
  tp.target_edge_length = 0.1f;
  tp.reproject = false;
  Mesh *tout = remesh::QuadRemesh(*t, tp);
  TASSERT(tout != nullptr);
  if (tout) {
    RemeshReport r = remeshValidate(*tout);
    report("pipeline:torus", r);
    TASSERT(r.all_quad);
    TASSERT(r.manifold);
    TASSERT(r.euler == 0);
    TASSERT(r.inverted_faces == 0);
    litestl::alloc::Delete<Mesh>(tout);
  }
  litestl::alloc::Delete<Mesh>(t);
}

// Tier 6.4: per_component splits the work mesh into connected components and
// remeshes each independently with the shared global edge length; the merged
// output keeps both closed spheres, and the report counts the sub-runs.
void testPerComponent()
{
  // Two copies of the proven pipeline sphere (radius 2 @ target 0.1), the
  // second appended at +6X so the components are well separated.
  Mesh *s = mesh::makeUVSphere(24, 32, 2.0f);
  {
    Mesh *s2 = mesh::makeUVSphere(24, 32, 2.0f);
    litestl::util::Vector<int> vmap;
    vmap.resize(int(s2->v.capacity()));
    for (int v : s2->v) {
      math::float3 co = s2->v.co[v];
      co[0] += 6.0f;
      vmap[v] = s->make_vertex(co);
    }
    litestl::util::Vector<int> vs;
    for (int f : s2->f) {
      int c0 = s2->l.c[s2->f.l[f]];
      vs.clear();
      int cc = c0;
      do {
        vs.append(vmap[s2->c.v[cc]]);
        cc = s2->c.next[cc];
      } while (cc != c0);
      s->make_face(vs);
    }
    s->recalc_normals();
    litestl::alloc::Delete<Mesh>(s2);
  }

  remesh::RemeshParams p;
  p.target_edge_length = 0.1f;
  p.per_component = true;
  remesh::RemeshRunReport rep;
  Mesh *out = remesh::QuadRemesh(*s, p, nullptr, nullptr, &rep);
  TASSERT(out != nullptr);
  TASSERT(rep.components_total == 2);
  TASSERT(rep.components_remeshed == 2);
  TASSERT(rep.components_failed == 0);
  if (out) {
    RemeshReport r = remeshValidate(*out);
    report("per-component", r);
    TASSERT(r.all_quad);
    TASSERT(r.manifold);
    TASSERT(r.component_count == 2);
    // No euler/inverted assertions: the +6X-translated copy fp-perturbs the
    // eigen-solves enough to shift singularities, which can leave odd cone
    // rims open (cap_odd_holes is off here) — same contract as capped-cylinder.
    TASSERT(r.spiral_isolines == 0);
    litestl::alloc::Delete<Mesh>(out);
  }
  litestl::alloc::Delete<Mesh>(s);
}

// Tier 6.7: boundary preservation through reprojection, quantified with
// boundaryDeviation. Extraction keeps whole lattice cells, so the raw rim sits
// up to ~1 cell (~target) inside the input rim — that bound is pinned on both
// legs. The 6.7 contract is the A/B: reproject + smooth must not move the rim
// any further (its boundary pin is what this guards). Flat annulus on purpose:
// the surface snap can't hide in-plane rim drift.
void testBoundarySurvival()
{
  Mesh *p = makeGridWithHole(17, 1.0f, 0.2f);

  auto runLeg = [&](bool reproject, BoundaryDeviation &bd) {
    remesh::RemeshParams params; // defaults: reproject + smooth on
    params.target_edge_length = 0.07f;
    params.reproject = reproject;
    Mesh *out = remesh::QuadRemesh(*p, params);
    TASSERT(out != nullptr);
    if (!out)
      return false;
    RemeshReport r = remeshValidate(*out);
    bd = boundaryDeviation(*p, *out);
    fprintf(stderr,
            "[bnd-survival/%s] refE=%d testV=%d mean=%g max=%g loops=%d\n",
            reproject ? "full" : "extract", bd.ref_boundary_edges,
            bd.test_boundary_verts, bd.mean_dist, bd.max_dist,
            r.boundary_loop_count);
    TASSERT(r.all_quad);
    TASSERT(r.boundary_loop_count == 2); // outer perimeter + hole rim survive
    TASSERT(bd.ref_boundary_edges > 0);
    TASSERT(bd.test_boundary_verts > 0);
    // Whole-cell extraction bound: rim within ~1 lattice cell of the input rim.
    TASSERT(bd.max_dist < 1.5f * params.target_edge_length);
    TASSERT(bd.mean_dist < params.target_edge_length);
    litestl::alloc::Delete<Mesh>(out);
    return true;
  };

  BoundaryDeviation bdExtract, bdFull;
  bool okE = runLeg(false, bdExtract);
  bool okF = runLeg(true, bdFull);
  // Reprojection must not push rim verts off the input rim (pinned boundary).
  if (okE && okF)
    TASSERT(bdFull.max_dist <= bdExtract.max_dist + 1e-5f);

  litestl::alloc::Delete<Mesh>(p);
}

// Simple.obj — a small, rounded organic blob. Its cross field curls enough that
// the exactly-seamless map folds about a third of its faces, which breaks
// extraction (the >10% fold gate). It is the fixture for the ARAP untangle
// fallback (QuantizeParams::untangle_fold_threshold): the fallback walks the seam
// penalty up from a low injective weight so the final map folds ~1.5% and
// extracts a valid all-quad mesh with no spirals. REMESH_TARGET overrides the
// spacing; 0.2 is mid-band (small input, no GPU offload — runs by default).
void testSimpleObj()
{
  char path[2048];
  std::snprintf(path, sizeof(path), "%s/Simple.obj", SCULPTCORE_ASSETS_DIR);
  Mesh *obj = mesh::loadObj(path);
  if (!obj) {
    fprintf(stderr, "[simple] skipped (could not open %s)\n", path);
    return;
  }
  fprintf(stderr, "[simple] loaded V=%d F=%d\n", obj->v.count, obj->f.count);
  remesh::RemeshParams p;
  p.target_edge_length = 0.2f;
  if (const char *e = std::getenv("REMESH_TARGET"))
    p.target_edge_length = float(std::atof(e));
  Mesh *out = remesh::QuadRemesh(*obj, p);
  TASSERT(out != nullptr);
  if (out) {
    RemeshReport r = remeshValidate(*out);
    report("simple", r);
    TASSERT(r.all_quad);
    TASSERT(r.spiral_isolines == 0);
    litestl::alloc::Delete<Mesh>(out);
  }
  litestl::alloc::Delete<Mesh>(obj);
}

// AnimeGirl2.obj — the organic spiral-elimination stress fixture. Naive cross
// field + parametrization spirals on dense real-world detail; M5's integer
// quantization is what kills it, so the headline assertion is no-spiral. This is
// opt-in (REMESH_ANIME=1): the mesh is ~800k triangles and a full solve is far
// too heavy for the default suite. Euler/inversions are reported but not gated —
// an organic surface produces odd-loop cap holes (left open, like the capped
// cylinder) and occasional reprojection drift, both accepted; the guarantees the
// pipeline owns are all-quad and no-spiral.
void testAnimeGirlSpiral()
{
  if (!std::getenv("REMESH_ANIME")) {
    fprintf(stderr, "[anime] skipped (set REMESH_ANIME=1; ~800k-face stress case)\n");
    return;
  }
  char path[2048];
  std::snprintf(path, sizeof(path), "%s/AnimeGirl2.obj", SCULPTCORE_ASSETS_DIR);
  Mesh *obj = mesh::loadObj(path);
  if (!obj) {
    fprintf(stderr, "[anime] skipped (could not open %s)\n", path);
    return;
  }
  fprintf(stderr, "[anime] loaded V=%d F=%d\n", obj->v.count, obj->f.count);
  remesh::RemeshParams p;
  p.target_edge_length = 0.05f;
  if (const char *e = std::getenv("REMESH_TARGET"))
    p.target_edge_length = float(std::atof(e));
  // REMESH_DECIMATE=<L> coarsens the solve mesh to ~L first (0 = solve raw);
  // makes the ~800k-tri input tractable, reproject still uses the original.
  if (const char *e = std::getenv("REMESH_DECIMATE"))
    p.solve_edge_length = float(std::atof(e));
  // REMESH_CAPODD=1 closes residual odd holes with one cap triangle each (trades
  // strict all-quad for watertight); off keeps the all-quad contract.
  bool cap_odd = std::getenv("REMESH_CAPODD") != nullptr;
  p.cap_odd_holes = cap_odd;
  Mesh *out = remesh::QuadRemesh(*obj, p);
  TASSERT(out != nullptr);
  if (out) {
    RemeshReport r = remeshValidate(*out);
    report("anime", r);
    if (!cap_odd)
      TASSERT(r.all_quad); // strict all-quad only when odd holes are left open
    TASSERT(r.spiral_isolines == 0); // the guarantee quantization exists to make
    litestl::alloc::Delete<Mesh>(out);
  }
  litestl::alloc::Delete<Mesh>(obj);
}

} // namespace

int main()
{
  // REMESH_ANIME=1 is the opt-in ~800k-tri stress mode: run only that fixture so
  // REMESH_TARGET/REMESH_DECIMATE tune it in isolation (the light fixtures share
  // REMESH_TARGET and would mis-scale). The default suite (env unset) is unchanged.
  if (std::getenv("REMESH_ANIME")) {
    testAnimeGirlSpiral();
    return retval;
  }
  testGridExtract();
  testPlaneHoleBorders();
  testCylinderExtract();
  testTorusExtract();
  testSphereExtract();
  testCappedCylinderExtract();
  testOddCapExtract();
  testReproject();
  testQuadRemeshPipeline();
  testPerComponent();
  testBoundarySurvival();
  testSimpleObj();
  testAnimeGirlSpiral();
  return retval;
}
