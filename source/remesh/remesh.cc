#include "remesh/remesh.h"
#include "remesh/preremesh.h"
#include "remesh/remesh_params.h"
#include "remesh/remesh_report.h"
#include "remesh/triage.h"

#include "remesh/extract/quad_extract.h"
#include "remesh/extract/reproject.h"
#include "remesh/field/cross_field.h"
#include "remesh/field/density.h"
#include "remesh/field/singularity_adjust.h"
#include "remesh/quantize/quantize_ilp.h"

#include "dyntopo/dyntopo_trace.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "mesh/utils/mesh_validate.h"
#include "mesh/utils/triangulate.h"

#include "litestl/util/alloc.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace sculptcore::remesh {

using namespace litestl;
using mesh::Mesh;

namespace {

/* Tier-5 gate diagnostic: count opposite-index singularity pairs within 2
 * vertex hops (each pair once) — the clutter a pair-cancellation pass could
 * annihilate — plus the poles participating in at least one such pair. */
void countSingularityClutter(Mesh &m, int &pairs, int &clutter_verts)
{
  mesh::BuiltinAttr<short, ".remesh.v.pole_index"> pole;
  pole.ensure(m.v.attrs);

  pairs = 0;
  clutter_verts = 0;

  auto eachNeighbor = [&m](int v, auto &&fn) {
    int e0 = m.v.e[v];
    if (e0 == ELEM_NONE) {
      return;
    }
    int ec = e0;
    do {
      int side = m.e.vs[ec][0] == v ? 0 : 1;
      fn(m.e.vs[ec][side ^ 1]);
      ec = m.e.disk[ec][side * 2 + 1];
    } while (ec != e0);
  };

  util::Set<int> in_pair;
  for (int v : m.v) {
    if (pole[v] == 0) {
      continue;
    }
    util::Set<int> seen;
    util::Vector<int> ring;
    seen.add(v);
    eachNeighbor(v, [&](int vn) {
      if (seen.add(vn)) {
        ring.append(vn);
      }
    });
    int ring1 = int(ring.size());
    for (int i = 0; i < ring1; i++) {
      eachNeighbor(ring[i], [&](int vn) {
        if (seen.add(vn)) {
          ring.append(vn);
        }
      });
    }
    for (int w : ring) {
      if (w > v && int(pole[v]) * int(pole[w]) < 0) {
        pairs++;
        in_pair.add(v);
        in_pair.add(w);
      }
    }
  }
  clutter_verts = int(in_pair.size());
}

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

/* Edge-length statistics feeding the Tier-9 auto heuristics + run report:
 * mean, median, and coefficient of variation (std/mean — irregularity). */
struct EdgeStats {
  float mean = 0.0f;
  float median = 0.0f;
  float cv = 0.0f;
};

EdgeStats measureEdges(Mesh &m)
{
  double sum = 0.0, sum2 = 0.0;
  util::Vector<float> lens;
  lens.ensure_capacity(m.e.count);
  for (int e : m.e) {
    float l = (m.v.co[m.e.vs[e][0]] - m.v.co[m.e.vs[e][1]]).length();
    sum += l;
    sum2 += double(l) * double(l);
    lens.append(l);
  }
  EdgeStats s;
  int n = lens.size();
  if (!n) {
    return s;
  }
  double mean = sum / n;
  s.mean = float(mean);
  if (mean > 1e-20) {
    double var = sum2 / n - mean * mean;
    s.cv = float(std::sqrt(std::fmax(var, 0.0)) / mean);
  }
  std::nth_element(lens.begin(), lens.begin() + n / 2, lens.end());
  s.median = lens[n / 2];
  return s;
}

/* Global uniform-remesh pre-pass: coarsen `m` toward edge length `L` so the
 * heavy global solve runs on a tractable triangle count. BK remesh + a few
 * tangential relaxation sweeps (the global solve folds where curvature
 * concentrates; smoothing evens the triangulation -> fewer parametrization
 * folds). align=0 = classic isotropic relaxation (Tier 9 lifts it to a
 * field-aligned blend). Geometry only — the quads are reprojected onto the
 * full-res original afterward, so mild tangential drift here is fine. */
void decimateForSolve(Mesh &m, float L, uint32_t seed)
{
  // Step toward L in <=2.5x jumps: one giant leap (dense scan -> coarse solve
  // res) collapses through features faster than relaxation can recover,
  // folding the surface the field solve then has to live with.
  float mean = measureEdges(m).mean;
  while (mean > 0.0f && 2.5f * mean < L) {
    bkRemeshToTarget(m, 2.5f * mean, seed);
    float next = measureEdges(m).mean;
    if (next <= mean) {
      break;
    }
    mean = next;
  }
  bkRemeshToTarget(m, L, seed);
  tangentialSmooth(m, 5, 0.5f, 0.0f);
}

/* L = sqrt(integral of density dA / N): the edge length at which an
 * integer-grid lattice over `m` yields ~@p target_quads faces (density scales
 * quad size as 1/sqrt(d), so N = integral(d dA) / L^2). Face fan areas are
 * weighted by the mean corner density when @p use_density and the layer
 * exists; d = 1 otherwise. Returns 0 on degenerate input. */
float countDerivedLength(Mesh &m, int target_quads, bool use_density)
{
  if (target_quads <= 0) {
    return 0.0f;
  }
  bool weight = use_density && m.v.attrs.has(mesh::AttrType::FLOAT,
                                             util::string(".remesh.v.density"));
  mesh::BuiltinAttr<float, ".remesh.v.density"> density;
  if (weight) {
    density.ensure(m.v.attrs);
  }

  double A = 0.0;
  util::Vector<int> vs;
  for (int f : m.f) {
    int li = m.f.l[f];
    int c0 = m.l.c[li];
    vs.clear();
    int cc = c0;
    do {
      vs.append(m.c.v[cc]);
      cc = m.c.next[cc];
    } while (cc != c0);
    if (vs.size() < 3) {
      continue;
    }
    double area2 = 0.0;
    const math::float3 &p0 = m.v.co[vs[0]];
    for (int i = 1; i + 1 < vs.size(); i++) {
      area2 += double((m.v.co[vs[i]] - p0).cross(m.v.co[vs[i + 1]] - p0).length());
    }
    double d = 1.0;
    if (weight) {
      double dsum = 0.0;
      for (int v : vs) {
        dsum += double(density[v]);
      }
      d = dsum / vs.size();
    }
    A += 0.5 * area2 * d;
  }
  if (A <= 0.0) {
    return 0.0f;
  }
  return float(std::sqrt(A / double(target_quads)));
}

} // namespace

float resolveTargetEdgeLength(mesh::Mesh &m, const RemeshParams &params)
{
  if (params.target_edge_length > 0.0f) {
    return params.target_edge_length;
  }
  float L = countDerivedLength(m, params.target_quad_count,
                               params.use_density || params.auto_density);
  return L > 0.0f ? L : 0.1f;
}

float resolvePreRemeshTarget(mesh::Mesh &m, const RemeshParams &params)
{
  if (params.pre_remesh_target > 0.0f) {
    return params.pre_remesh_target;
  }
  if (params.solve_edge_length > 0.0f) {
    return params.solve_edge_length;
  }
  // Count mode: a touch finer than the quad edge (a few solve tris per output
  // quad), floored at half the median input edge so the pre-pass never
  // refines the input more than ~4x — finer quads come from the lattice.
  EdgeStats es = measureEdges(m);
  float L = params.target_edge_length > 0.0f
                ? params.target_edge_length
                : std::fmax(0.7f * resolveTargetEdgeLength(m, params),
                            0.5f * es.median);
  // Edge budget: an auto target may not coarsen away more than 20% of the
  // input edges. E scales ~1/L^2, so E_out/E_in >= 0.8 ==> L <= mean/sqrt(0.8).
  float L_budget = es.mean > 0.0f ? es.mean / std::sqrt(0.8f) : 0.0f;
  return L_budget > 0.0f ? std::fmin(L, L_budget) : L;
}

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

  // Resolve the operative quad edge length: explicit target_edge_length, or
  // derived from target_quad_count (L = sqrt(integral of density dA / N)).
  // Count mode refines L further below as the work mesh / density evolve.
  const bool count_mode = params.target_edge_length <= 0.0f;
  float L_quad = resolveTargetEdgeLength(*work, params);

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

  // Tier 9 (9d): field-aligned input pre-remesh — clean the working
  // triangulation's flow before the field solve. Sentinel knobs (target=0,
  // iters=0, bootstrap=-1) auto-resolve from the measured input; explicit
  // values always win. With --solve the decimate above already coarsened, so
  // the pre-pass field-aligns at that resolution.
  bool pre_remeshed = false;
  if (params.pre_remesh) {
    PROG(14, "pre_remesh");
    auto t_pre = std::chrono::steady_clock::now();

    EdgeStats es = measureEdges(*work);

    // Target: explicit > solve resolution > output resolution (count mode:
    // 0.7x the quad edge, floored at half the median input edge — see
    // resolvePreRemeshTarget; inlined here to reuse L_quad and es).
    float L_pre = params.pre_remesh_target > 0.0f ? params.pre_remesh_target
                  : params.solve_edge_length > 0.0f
                      ? params.solve_edge_length
                      : (count_mode ? std::fmax(0.7f * L_quad, 0.5f * es.median)
                                    : params.target_edge_length);
    // Edge budget (auto targets only — explicit pre_remesh_target/solve win):
    // the pre-pass may not coarsen away more than 20% of the edges it
    // receives. E scales ~1/L^2, so E_out/E_in >= 0.8 ==> L <= mean/sqrt(0.8).
    const bool budgeted = params.pre_remesh_target <= 0.0f &&
                          params.solve_edge_length <= 0.0f && es.mean > 0.0f;
    float L_budget = budgeted ? es.mean / std::sqrt(0.8f) : 0.0f;
    if (budgeted) {
      L_pre = std::fmin(L_pre, L_budget);
    }
    mesh::FoldCounts fin = mesh::countGeometricFolds(*work);
    int verts_in = work->v.count, faces_in = work->f.count;

    // "Noisy" = measurable fold density or degenerate faces: distrust the
    // input's features/field longer (more bootstrap), expect more settle iters.
    bool noisy = fin.fold90 > work->e.count / 100 || fin.degenerate_faces > 0;
    float ratio = es.mean > 1e-20f ? es.mean / L_pre : 1.0f;
    float travel = std::fabs(std::log2(ratio < 1e-6f ? 1e-6f : ratio));

    int iters = params.pre_remesh_iters;
    if (iters <= 0) {
      // More resolution travel (|log2(mean/L)|) and more noise ⇒ more outer
      // iters; converge_eps stops clean inputs earlier regardless.
      iters = 3;
      if (travel > 1.0f)
        iters++;
      if (travel > 2.0f)
        iters++;
      if (noisy || es.cv > 0.6f)
        iters++;
    }
    int bootstrap = params.pre_remesh_bootstrap_iters;
    if (bootstrap < 0) {
      bootstrap = noisy ? 4 : (fin.fold90 == 0 && es.cv < 0.3f ? 1 : 2);
    }

    // Dense-input coarsen bootstrap: the rough field solve is the expensive
    // piece (it scales with V), so when the input sits far below the pre-pass
    // target, BK-coarsen once field-free before the loop — exactly what the
    // --solve decimate does (skipped when that already ran).
    bool coarsen = !decimated && es.mean > 0.0f && es.mean < 0.5f * L_pre;
    if (coarsen) {
      decimateForSolve(*work, L_pre, params.seed);
    }

    PreRemeshParams pp;
    pp.iters = iters;
    pp.target = L_pre;
    pp.density = params.pre_remesh_density;
    pp.gradation = params.pre_remesh_gradation;
    pp.gradation_iters = params.pre_remesh_gradation_iters;
    pp.align = params.pre_remesh_align;
    pp.field_cadence = params.pre_remesh_field_cadence;
    pp.bootstrap_iters = bootstrap;
    pp.seed = params.seed;
    pp.smooth_iters = params.pre_remesh_smooth_iters;
    pp.smooth_lambda = params.pre_remesh_smooth_lambda;
    pp.density_min = params.density_min;
    if (budgeted) {
      // Same budget for local coarsening: the density floor bounds the largest
      // local BK target (L_pre / sqrt(density_min)) at L_budget.
      float r = L_pre / L_budget;
      pp.density_min = std::fmax(params.density_min, r * r);
    }
    pp.density_max = params.density_max;
    pp.converge_eps = params.pre_remesh_converge_eps;
    pp.preserve_features = params.pre_remesh_preserve_features;
    pp.sharp_angle = params.pre_remesh_sharp_angle;
    dyntopo::DynTopoTrace trace;
    if (params.pre_remesh_trace) {
      pp.trace = &trace;
    }
    PreRemeshStats ps;
    preRemesh(*work, pp, &ps);
    pre_remeshed = true;
    if (params.pre_remesh_trace) {
      dyntopo::printTraceSummary(trace, "pre-conv");
    }

    if (report) {
      report->pre_remesh = StageStatus::Ok;
      auto &pe = report->pre_remesh_effect;
      pe.ran = true;
      pe.verts_in = verts_in;
      pe.faces_in = faces_in;
      pe.verts_out = work->v.count;
      pe.faces_out = work->f.count;
      pe.mean_edge_in = es.mean;
      pe.edge_cv_in = es.cv;
      pe.mean_edge_out = measureEdges(*work).mean;
      pe.fold90_in = fin.fold90;
      pe.fold180_in = fin.fold180;
      pe.degen_in = fin.degenerate_faces;
      mesh::FoldCounts fout = mesh::countGeometricFolds(*work);
      pe.fold90_out = fout.fold90;
      pe.fold180_out = fout.fold180;
      pe.degen_out = fout.degenerate_faces;
      pe.iters_run = ps.iters_run;
      pe.converged = ps.converged;
      pe.coarsen_bootstrap = coarsen;
      pe.target_resolved = L_pre;
      pe.iters_resolved = iters;
      pe.bootstrap_resolved = bootstrap;
      pe.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t_pre)
                           .count();
    }
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
  cp.curvature_smooth_iters = params.curvature_smooth_iters;
  cp.curvature_smooth_lambda = params.curvature_smooth_lambda;
  cp.field_smoothness = params.field_smoothness;
  cp.curvature_weight = params.curvature_weight;
  CrossFieldStats cfs = computeCrossField(*work, cp);
  if (report) {
    report->cross_field = StageStatus::Ok;
    report->num_singularities = cfs.num_singularities;
    report->index_sum = cfs.index_sum;
    report->field_solved_eigen = cfs.solved_eigen;
    countSingularityClutter(*work, report->field_close_pairs,
                            report->field_clutter_verts);
  }

  PROG(45, "singularity");
  SingularityAdjustParams sap;
  sap.seed = params.seed;
  adjustSingularities(*work, sap);
  if (report)
    report->singularity = StageStatus::Ok;

  // Tier 3: build / bound the sizing field before the seamless param reads it.
  // 3a auto-density generates .remesh.v.density from the smoothed curvature; 3b
  // bounds its gradient. Both default off (no field / no limiting). auto_density
  // implies density consumption (ORed into qp.use_density below).
  const bool consume_density = params.use_density || params.auto_density;
  if (params.auto_density) {
    DensityParams dpa;
    dpa.target_edge_length = L_quad;
    dpa.density_min = params.density_min;
    dpa.density_max = params.density_max;
    dpa.curvature_smooth_iters = params.curvature_smooth_iters;
    dpa.curvature_smooth_lambda = params.curvature_smooth_lambda;
    generateAutoDensity(*work, dpa);
    if (count_mode) {
      // L and the auto field are mutually dependent (s = k*L vs
      // L = sqrt(integral d dA / N)) — iterate the scalar fixed point.
      for (int it = 0; it < 3; it++) {
        float L_new = countDerivedLength(*work, params.target_quad_count, true);
        if (L_new <= 0.0f) {
          break;
        }
        bool settled = std::fabs(L_new - L_quad) <= 0.02f * L_quad;
        L_quad = L_new;
        if (settled) {
          break;
        }
        dpa.target_edge_length = L_quad;
        generateAutoDensity(*work, dpa);
      }
    }
  } else if (count_mode) {
    // Re-derive on the current geometry: decimate / pre-remesh shifted the
    // surface area (and a painted density field) the initial estimate used.
    float L_new = countDerivedLength(*work, params.target_quad_count,
                                     consume_density);
    if (L_new > 0.0f) {
      L_quad = L_new;
    }
  }
  if (params.density_gradation > 0.0f) {
    limitDensityGradation(*work, L_quad, params.density_gradation,
                          params.density_gradation_iters, params.density_min,
                          params.density_max);
    // The limiter only ever raises density — recompute L once so the count
    // target still holds under the gradation-widened field.
    if (count_mode && consume_density) {
      float L_new = countDerivedLength(*work, params.target_quad_count, true);
      if (L_new > 0.0f) {
        L_quad = L_new;
      }
    }
  }

  PROG(65, "quantize");
  QuantizeParams qp;
  qp.target_edge_length = L_quad;
  // auto_density implies use_density — the seamless param / quantizer are gated
  // on use_density, so a generated field would otherwise be silently ignored.
  qp.use_density = consume_density;
  QuantizeStats qs = computeQuantization(*work, qp);
  if (report) {
    report->quantize = StageStatus::Ok;
    report->parametrization_folds = qs.parametrization_folds;
    report->min_jacobian = qs.min_jacobian;
    report->quantize_feasible = qs.feasible;
    report->derived_edge_length = L_quad;
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
  if (report) {
    report->extract = StageStatus::Ok;
    report->quad_count_actual = out->f.count;
  }

  if (params.reproject) {
    PROG(92, "reproject");
    ReprojectParams rp;
    rp.smooth_iterations = params.smooth_iterations;
    rp.smooth_lambda = params.smooth_strength;
    // One extra [smooth -> snap] pass when smoothing, else a single pure snap.
    rp.iterations = params.smooth_iterations > 0 ? 2 : 1;
    if (decimated || pre_remeshed) {
      // The work geometry diverged from the input (decimate and/or pre-remesh
      // moved every vertex) — snap onto the ORIGINAL full-res surface so the
      // output recovers the detail those passes smoothed away.
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
