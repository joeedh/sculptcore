#include "mesh/mesh.h"
#include "remesh/remesh.h"
#include "remesh/remesh_params.h"

using namespace sculptcore;
using sculptcore::mesh::Mesh;
using sculptcore::remesh::RemeshParams;

#ifdef WASM
#include <emscripten/bind.h>
#endif

extern "C" {

/* Global quad-remesh op (the Mesh_triangulate-style whole-mesh entry point, NOT
 * a brush). Returns a freshly-allocated all-quad Mesh; @p in is left intact so
 * the host keeps it for undo. Caller frees the result via freeMesh. Returns
 * nullptr on a clean failure or bad args. */
Mesh *Mesh_quadRemesh(Mesh *in, RemeshParams *params)
{
  if (!in || !params) {
    return nullptr;
  }
  return remesh::QuadRemesh(*in, *params);
}
}
