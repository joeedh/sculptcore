#include "remesh/remesh.h"
#include "remesh/remesh_params.h"
#include "remesh/remesh_report.h"
#include "remesh/triage.h"

#include "remesh/extract/quad_extract.h"
#include "remesh/extract/reproject.h"
#include "remesh/field/cross_field.h"
#include "remesh/field/singularity_adjust.h"
#include "remesh/quantize/quantize_ilp.h"

#include "dyntopo/dyntopo.h"
#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "mesh/utils/mesh_validate.h"
#include "mesh/utils/triangulate.h"

#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <chrono>

namespace sculptcore::remesh {

using namespace litestl;
using mesh::Mesh;

namespace {

/* Deep-copy @p src's positions + face topology into a fresh triangle mesh, and
 * (Tier 1b) carry the input constraint layers across by vertex map. The pipeline
 * mutates its working mesh (thaw, TEMP field/param attrs, triangulation,
 * recomputed normals), but QuadRemesh's contract is that the caller's input is
 * left intact for undo — so the stages run on this copy, never on `input`.
 *
 * Copied constraint layers (INPUT layers only, never the computed
 * .remesh.v.pole_index): .remesh.v.density (float) + .remesh.v.pole_pinned (bool)
 * by vmap, and .remesh.f.stroke_dir (float3) onto the 1:1 work faces, preserved
 * through the fan via triangulateFaceFanCb. Absent layers add nothing — a
 * geometry-only input takes the plain triangulateMesh path (byte-identical to
 * pre-Tier-1). TODO(M6h): barycentrically transfer the input's per-vertex attrs
 * onto the OUTPUT at extraction's source face/bary (a separate cross-mesh pass). */
Mesh *buildTriCopy(Mesh &src)
{
  src.thawTopo();

  Mesh *work = alloc::New<Mesh>("Mesh QuadRemesh work");

  // Tier 1b: input constraint layers (copy input layers only).
  bool have_density =
      src.v.attrs.has(mesh::AttrType::FLOAT, util::string(".remesh.v.density"));
  bool have_pinned =
      src.v.attrs.has(mesh::AttrType::BOOL, util::string(".remesh.v.pole_pinned"));
  bool have_stroke =
      src.f.attrs.has(mesh::AttrType::FLOAT3, util::string(".remesh.f.stroke_dir"));

  mesh::BuiltinAttr<float, ".remesh.v.density"> srcDensity, dstDensity;
  mesh::BuiltinAttr<bool, ".remesh.v.pole_pinned"> srcPinned, dstPinned;
  mesh::BuiltinAttr<math::float3, ".remesh.f.stroke_dir"> srcStroke, dstStroke;
  if (have_density) {
    srcDensity.ensure(src.v.attrs);
    dstDensity.ensure(work->v.attrs);
  }
  if (have_pinned) {
    srcPinned.ensure(src.v.attrs);
    dstPinned.ensure(work->v.attrs);
  }
  if (have_stroke) {
    srcStroke.ensure(src.f.attrs);
    dstStroke.ensure(work->f.attrs);
  }

  util::Vector<int> vmap;
  vmap.resize(int(src.v.capacity()));
  for (int v : src.v) {
    int nv = work->make_vertex(src.v.co[v]);
    vmap[v] = nv;
    if (have_density)
      dstDensity[nv] = srcDensity[v];
    if (have_pinned)
      dstPinned.set(nv, srcPinned[v]);
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
      int nf = work->make_face(vs);
      if (have_stroke)
        dstStroke[nf] = srcStroke[f];
    }
  }

  if (have_stroke) {
    // Fan each face with the attr-preserving path so stroke_dir survives onto
    // every fan triangle. Same fan topology as triangulateMesh ⇒ geometry is
    // unchanged; only the carried face attr differs.
    util::Vector<int> faces;
    for (int f : work->f) {
      faces.append(f);
    }
    for (int f : faces) {
      mesh::triangulateFaceFanCb(*work, f);
    }
  } else {
    mesh::triangulateMesh(*work);
  }
  work->recalc_normals();
  return work;
}

/* Global uniform-remesh pre-pass: coarsen `m` toward edge length `L` so the
 * heavy global solve runs on a tractable triangle count. Reuses dyntopo's
 * Botsch-Kobbelt operators (collapse short / split long / flip / tangential
 * smooth) over a whole-mesh sphere. Geometry only — the quads are reprojected
 * onto the full-res original afterward, so mild tangential drift here is fine. */
void decimateForSolve(Mesh &m, float L, uint32_t seed)
{
  m.thawTopo();

  bool have = false;
  math::float3 bmin{}, bmax{};
  for (int v : m.v) {
    math::float3 co = m.v.co[v];
    if (!have) {
      bmin = bmax = co;
      have = true;
      continue;
    }
    for (int i = 0; i < 3; i++) {
      if (co[i] < bmin[i]) bmin[i] = co[i];
      if (co[i] > bmax[i]) bmax[i] = co[i];
    }
  }
  if (!have) {
    return;
  }
  math::float3 center = (bmin + bmax) * 0.5f;
  float radius = (bmax - bmin).length(); // > half-diagonal: covers the whole mesh

  dyntopo::DynTopoParams dp;
  dp.l_max = L * (4.0f / 3.0f);
  dp.l_min = L * (4.0f / 5.0f);
  dp.mode = dyntopo::DynTopoMode::Both;
  dp.do_flips = true;
  dp.do_smooth = true;
  dp.preserve_features = false; // the tri copy carries no boundary overlays
  dp.max_rounds = 100;
  dyntopo::applyBrushDab(m, center, radius, dp, seed);

  // Extra tangential relaxation of the decimated copy. The global solve folds
  // where curvature concentrates; a few smoothing sweeps even out the
  // triangulation (smaller curvature gradients -> fewer parametrization folds).
  // Each vertex moves toward its one-ring centroid, projected onto the ring's
  // own best-fit plane (Newell normal) so only the tangential component applies
  // -- no volume shrinkage, and no dependence on stored vertex normals. The
  // displacement is clamped to the local edge scale, so irregular dyntopo
  // connectivity can never blow positions up.
  const int smooth_iters = 5;
  const float smooth_lambda = 0.5f;
  m.thawTopo();
  util::Vector<math::float3> nco;
  nco.resize(int(m.v.capacity()));
  util::Vector<math::float3> ring;
  for (int it = 0; it < smooth_iters; it++) {
    for (int v : m.v) {
      math::float3 vco = m.v.co[v];
      int e0 = m.v.e[v];
      if (e0 == ELEM_NONE) {
        nco[v] = vco;
        continue;
      }
      ring.clear();
      int ec = e0, guard = 0;
      do {
        int ov = m.e.vs[ec][0] == v ? m.e.vs[ec][1] : m.e.vs[ec][0];
        ring.append(m.v.co[ov]);
        int side = m.e.vs[ec][0] == v ? 0 : 1;
        ec = m.e.disk[ec][side * 2 + 1];
      } while (ec != e0 && ++guard < 256);
      int k = int(ring.size());
      if (k < 3) {
        nco[v] = vco;
        continue;
      }
      math::float3 cen{};
      for (int i = 0; i < k; i++) cen += ring[i];
      cen = cen * (1.0f / float(k));
      math::float3 nrm{}; // Newell normal of the ordered ring polygon about cen
      float minlen = 1e30f;
      for (int i = 0; i < k; i++) {
        math::float3 a = ring[i] - cen, b = ring[(i + 1) % k] - cen;
        nrm += a.cross(b);
        float el = (ring[i] - vco).length();
        if (el < minlen) minlen = el;
      }
      math::float3 d = cen - vco;
      float nl = nrm.length();
      if (nl > 1e-20f) {
        math::float3 un = nrm * (1.0f / nl);
        d = d - un * d.dot(un); // tangent-plane component only
      }
      float dl = d.length();
      if (dl > minlen && dl > 1e-20f) d = d * (minlen / dl); // clamp to edge scale
      nco[v] = vco + d * smooth_lambda;
    }
    for (int v : m.v) {
      m.v.co[v] = nco[v];
    }
  }
  m.recalc_normals();
}

} // namespace

mesh::Mesh *QuadRemesh(mesh::Mesh &input, const RemeshParams &params,
                       RemeshProgressFn progress, void *user,
                       RemeshRunReport *report)
{
#define PROG(pct, stage)                                                        \
  do {                                                                          \
    if (progress)                                                               \
      progress(user, (pct), (stage));                                           \
  } while (0)

  auto t_start = std::chrono::steady_clock::now();
  auto elapsedMs = [&]() -> long long {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t_start)
        .count();
  };

  PROG(0, "copy");
  Mesh *work = buildTriCopy(input);
  if (report)
    report->copy = StageStatus::Ok;

  // Tier 1: optional input triage (weld near-coincident verts, drop degenerate
  // faces / tiny components, detect non-manifold). Defaults off; on clean input
  // it is a no-op (byte-identical), so the merge stays behavior-preserving.
  if (params.triage) {
    PROG(6, "triage");
    TriageParams tp;
    tp.weld_rel = params.triage_weld_rel;
    tp.min_component_frac = params.triage_min_component_frac;
    TriageReport tr;
    triageMesh(*work, tp, tr);
    if (report) {
      report->triage = StageStatus::Ok;
      report->triage_report = tr;
    }
  }

  // Optional decimation pre-pass: coarsen the SOLVE mesh so dense inputs stay
  // tractable. The reprojection below still snaps onto the full-res original.
  bool decimated = false;
  if (params.solve_edge_length > 0.0f) {
    PROG(10, "decimate");
    decimateForSolve(*work, params.solve_edge_length, params.seed);
    decimated = true;
    if (report)
      report->decimate = StageStatus::Ok;
  }

  // M2 cross field -> M3 singularity adjust -> M5 quantization (M5 rebuilds the
  // cut graph / seamless map internally). Mirrors test_remesh_extract's proven
  // sequence.
  PROG(25, "cross_field");
  CrossFieldParams cp;
  cp.use_curvature = params.use_curvature;
  cp.use_sharp_features = params.use_sharp_features;
  cp.sharp_angle = params.sharp_angle;
  cp.seed = params.seed;
  CrossFieldStats cfs = computeCrossField(*work, cp);
  if (report) {
    report->cross_field = StageStatus::Ok;
    report->num_singularities = cfs.num_singularities;
    report->index_sum = cfs.index_sum;
    report->field_solved_eigen = cfs.solved_eigen;
  }

  PROG(45, "singularity");
  SingularityAdjustParams sap;
  sap.seed = params.seed;
  adjustSingularities(*work, sap);
  if (report)
    report->singularity = StageStatus::Ok;

  PROG(65, "quantize");
  QuantizeParams qp;
  qp.target_edge_length = params.target_edge_length;
  qp.use_density = params.use_density;
  QuantizeStats qs = computeQuantization(*work, qp);
  if (report) {
    report->quantize = StageStatus::Ok;
    report->parametrization_folds = qs.parametrization_folds;
    report->min_jacobian = qs.min_jacobian;
    report->quantize_feasible = qs.feasible;
  }

  // M6: extract the integer-lattice preimage, then snap onto the input surface.
  PROG(80, "extract");
  ExtractParams ep;
  ep.cap_odd_holes = params.cap_odd_holes;
  ExtractStats st;
  Mesh *out = extractQuadMesh(*work, ep, st);
  if (!out) {
    PROG(100, "failed");
    if (report) {
      report->extract = StageStatus::Failed;
      report->failure_reason = "extract_no_lattice";
      report->duration_ms = elapsedMs();
    }
    alloc::Delete<Mesh>(work);
    return nullptr; // clean failure: no integer-grid map / no lattice points
  }
  if (report)
    report->extract = StageStatus::Ok;

  if (params.reproject) {
    PROG(92, "reproject");
    ReprojectParams rp;
    rp.smooth_iterations = params.smooth_iterations;
    rp.smooth_lambda = params.smooth_strength;
    // One extra [smooth -> snap] pass when smoothing, else a single pure snap.
    rp.iterations = params.smooth_iterations > 0 ? 2 : 1;
    if (decimated) {
      // Snap onto the ORIGINAL full-res surface, not the coarsened solve mesh,
      // so the output recovers detail the decimation dropped.
      Mesh *full = buildTriCopy(input);
      reprojectToSurface(*out, *full, rp);
      alloc::Delete<Mesh>(full);
    } else {
      reprojectToSurface(*out, *work, rp);
    }
    if (report)
      report->reproject = StageStatus::Ok;
  }

  PROG(100, "done");
  if (report) {
    report->success = true;
    report->duration_ms = elapsedMs();
    // Self-contained validation for Tier-8's in-process retry (skipped on the
    // no-report path). The pre-extraction fold count can't be derived from the
    // output, so copy it in from the quantize stats.
    report->validation = mesh::remeshValidate(*out);
    report->validation.parametrization_folds = qs.parametrization_folds;
    report->validation_filled = true;
  }
  alloc::Delete<Mesh>(work);
  return out;
#undef PROG
}

} // namespace sculptcore::remesh
