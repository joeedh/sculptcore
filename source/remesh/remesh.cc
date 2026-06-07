#include "remesh/remesh.h"
#include "remesh/remesh_params.h"

#include "remesh/extract/quad_extract.h"
#include "remesh/extract/reproject.h"
#include "remesh/field/cross_field.h"
#include "remesh/field/singularity_adjust.h"
#include "remesh/quantize/quantize_ilp.h"

#include "mesh/mesh.h"
#include "mesh/utils/triangulate.h"

#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

namespace sculptcore::remesh {

using namespace litestl;
using mesh::Mesh;

namespace {

/* Deep-copy @p src's positions + face topology into a fresh triangle mesh. The
 * pipeline mutates its working mesh (thaw, TEMP field/param attrs, triangulation,
 * recomputed normals), but QuadRemesh's contract is that the caller's input is
 * left intact for undo — so the stages run on this copy, never on `input`.
 *
 * Only geometry is copied: the auto-only v1 reads no user-painted constraint
 * layers. TODO(M6h): when the host writes .remesh.v.density/.pole_index /
 * .remesh.f.stroke_dir, copy those layers here too (and barycentrically transfer
 * the input's per-vertex attrs onto the output at extraction's source face/bary,
 * float/vec lerp + int/bool copy a la attr_interp.h — interpAttrs itself is
 * within-mesh, so the cross-mesh transfer needs its own pass). */
Mesh *buildTriCopy(Mesh &src)
{
  src.thawTopo();

  Mesh *work = alloc::New<Mesh>("Mesh QuadRemesh work");

  util::Vector<int> vmap;
  vmap.resize(int(src.v.capacity()));
  for (int v : src.v) {
    vmap[v] = work->make_vertex(src.v.co[v]);
  }

  util::Vector<int> vs;
  for (int f : src.f) {
    // Outer boundary only; our inputs carry no holes (list_count > 1).
    int li = src.f.l[f];
    int c0 = src.l.c[li];
    vs.clear();
    int cc = c0;
    do {
      vs.append(vmap[src.c.v[cc]]);
      cc = src.c.next[cc];
    } while (cc != c0);
    if (vs.size() >= 3) {
      work->make_face(vs);
    }
  }

  mesh::triangulateMesh(*work);
  work->recalc_normals();
  return work;
}

} // namespace

mesh::Mesh *QuadRemesh(mesh::Mesh &input, const RemeshParams &params)
{
  int _i = 0;
  printf("copy mesh\n");
  Mesh *work = buildTriCopy(input);
  fflush(stdout);
  // M2 cross field -> M3 singularity adjust -> M5 quantization (M5 rebuilds the
  // cut graph / seamless map internally). Mirrors test_remesh_extract's proven
  // sequence.
  CrossFieldParams cp;
  cp.use_curvature = params.use_curvature;
  cp.use_sharp_features = params.use_sharp_features;
  cp.sharp_angle = params.sharp_angle;
  cp.seed = params.seed;
  printf("compute cross field\n");
  computeCrossField(*work, cp);

  SingularityAdjustParams sap;
  sap.seed = params.seed;
  printf("adjust singularities\n");
  adjustSingularities(*work, sap);

  QuantizeParams qp;
  qp.target_edge_length = params.target_edge_length;
  qp.use_density = params.use_density;
  printf("compute quantization\n");
  computeQuantization(*work, qp);

  // M6: extract the integer-lattice preimage, then snap onto the input surface.
  ExtractParams ep;
  ExtractStats st;
  printf("extract quad mesh\n");
  Mesh *out = extractQuadMesh(*work, ep, st);
  if (!out) {
    alloc::Delete<Mesh>(work);
    return nullptr; // clean failure: no integer-grid map / no lattice points
  }

  if (params.reproject) {
    ReprojectParams rp;
    rp.smooth_iterations = params.smooth_iterations;
    rp.smooth_lambda = params.smooth_strength;
    // One extra [smooth -> snap] pass when smoothing, else a single pure snap.
    rp.iterations = params.smooth_iterations > 0 ? 2 : 1;
    printf("reproject to surface\n");
    reprojectToSurface(*out, *work, rp);
  }

  printf("delete temp mesh\n");
  alloc::Delete<Mesh>(work);
  printf("done\n");
  return out;
}

} // namespace sculptcore::remesh
