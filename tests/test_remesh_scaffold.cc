// Scaffolding smoke test for the quad-remesh module. Verifies the shared test
// machinery built up front (procedural shape generators, the reusable
// remeshValidate report, the OBJ loader) and that the Mesh_quadRemesh entry
// point is wired end to end (runs, returns a mesh, leaves the input intact).
// Per-shape structural correctness of the output is gated in test_remesh_extract;
// this file asserts wiring + fixtures, not output quality.
#include "test_util.h"

#include "litestl/util/alloc.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "mesh/utils/mesh_validate.h"
#include "remesh/remesh.h"
#include "remesh/remesh_params.h"

#include "obj_load.h"
#include "test_config.h"

#include <cstdio>
#include <cstring>

using namespace sculptcore;
using sculptcore::mesh::Mesh;
using sculptcore::mesh::RemeshReport;

test_init;

// A closed genus-0/1 fixture: cycle-integrity, expected Euler, consistent
// winding, no degenerate/inverted faces.
static void checkClosed(const char *name, Mesh *m, int expectedEuler)
{
  RemeshReport rep = mesh::remeshValidate(*m);
  if (!rep.manifold) {
    fprintf(stderr, "%s: not manifold: %s\n", name, rep.manifold_error.c_str());
  }
  test_assert(rep.manifold);
  test_assert(rep.consistent_winding);
  test_assert(rep.non_manifold_edges == 0);
  test_assert(rep.boundary_edges == 0);
  test_assert(rep.degenerate_faces == 0);
  test_assert(rep.inverted_faces == 0);
  if (rep.euler != expectedEuler) {
    fprintf(stderr, "%s: euler %d != expected %d\n", name, rep.euler, expectedEuler);
  }
  test_assert(rep.euler == expectedEuler);
}

int main()
{
  // --- procedural shape fixtures ---
  {
    Mesh *grid = mesh::makeGrid(16, 16, 1.0f);
    RemeshReport rep = mesh::remeshValidate(*grid);
    test_assert(rep.manifold);
    test_assert(rep.consistent_winding);
    test_assert(rep.all_quad);                       // grid is pure quads
    test_assert(rep.euler == 1);                     // topological disk
    test_assert(rep.boundary_edges == (16 - 1) * 4); // open border
    test_assert(rep.degenerate_faces == 0);
    alloc::Delete<Mesh>(grid);
  }
  {
    Mesh *cyl = mesh::makeCylinder(24, 8, 0.5f, 2.0f, /*capped=*/true);
    checkClosed("cylinder", cyl, 2);
    alloc::Delete<Mesh>(cyl);
  }
  {
    Mesh *torus = mesh::makeTorus(32, 16, 1.0f, 0.3f);
    RemeshReport rep = mesh::remeshValidate(*torus);
    test_assert(rep.manifold);
    test_assert(rep.consistent_winding);
    test_assert(rep.all_quad);   // torus is pure quads
    test_assert(rep.euler == 0); // genus 1
    test_assert(rep.degenerate_faces == 0);
    test_assert(rep.inverted_faces == 0);
    alloc::Delete<Mesh>(torus);
  }
  {
    Mesh *sphere = mesh::makeUVSphere(16, 24, 1.0f);
    checkClosed("uvsphere", sphere, 2);
    alloc::Delete<Mesh>(sphere);
  }

  // --- Mesh_quadRemesh end-to-end wiring ---
  {
    Mesh *cube = mesh::createCube(8, 0.5f, 0.0f);
    RemeshReport inRep = mesh::remeshValidate(*cube);
    test_assert(inRep.manifold);
    test_assert(inRep.euler == 2);

    remesh::RemeshParams params;
    params.target_edge_length = 0.1f;
    Mesh *out = remesh::QuadRemesh(*cube, params);
    test_assert(out != nullptr);

    // Input must survive untouched (the host snapshots it for undo).
    RemeshReport inAfter = mesh::remeshValidate(*cube);
    test_assert(inAfter.manifold);
    test_assert(inAfter.face_count == inRep.face_count);

    // Diagnostic only. The faceted cube's valence-3 sharp corners (three 90deg
    // creases meeting at a point) are a hard case the pipeline does not yet
    // remesh cleanly; structural gates live in test_remesh_extract (sphere /
    // cylinder / torus), and sharp-corner robustness is tracked for M6g.
    RemeshReport outRep = mesh::remeshValidate(*out);
    fprintf(stderr,
            "[scaffold:cube] V=%d E=%d F=%d euler=%d allquad=%d manifold=%d "
            "bnd=%d inv=%d\n",
            outRep.vert_count,
            outRep.edge_count,
            outRep.face_count,
            outRep.euler,
            outRep.all_quad,
            outRep.manifold,
            outRep.boundary_edges,
            outRep.inverted_faces);

    alloc::Delete<Mesh>(out);
    alloc::Delete<Mesh>(cube);
  }

  // --- OBJ loader fixture ---
  {
    char path[2048];
    std::snprintf(path, sizeof(path), "%s/AnimeGirl2.obj", SCULPTCORE_ASSETS_DIR);
    Mesh *obj = mesh::loadObj(path);
    if (!obj) {
      fprintf(stderr, "could not open %s\n", path);
    }
    test_assert(obj != nullptr);
    if (obj) {
      test_assert(obj->v.count > 0);
      test_assert(obj->f.count > 0);
      // Imported as a triangle soup.
      std::string err;
      test_assert(mesh::checkTopology(*obj, err, /*requireTriangles=*/true));
      alloc::Delete<Mesh>(obj);
    }
  }

  return test_end();
}
