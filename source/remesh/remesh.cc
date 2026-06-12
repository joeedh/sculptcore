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
#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

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

/* Tier 6.4: split @p work into face-connected sub-meshes (union-find over the
 * edge graph), carrying the input constraint layers like buildTriCopy. Verts in
 * face-less components are dropped — they cannot produce quads. */
void splitComponents(Mesh &work, util::Vector<Mesh *> &pieces)
{
  int vcap = int(work.v.capacity());
  util::Vector<int> uf;
  uf.resize(vcap);
  for (int v : work.v) {
    uf[v] = v;
  }
  auto find = [&uf](int v) {
    while (uf[v] != v) {
      uf[v] = uf[uf[v]]; // path halving
      v = uf[v];
    }
    return v;
  };
  for (int e : work.e) {
    int a = find(work.e.vs[e][0]), b = find(work.e.vs[e][1]);
    if (a != b) {
      uf[a] = b;
    }
  }

  util::Map<int, int> rootPiece; // component root -> piece index
  for (int f : work.f) {
    int r = find(work.c.v[work.l.c[work.f.l[f]]]);
    if (!rootPiece.contains(r)) {
      rootPiece.insert(r, int(pieces.size()));
      pieces.append(alloc::New<Mesh>("Mesh QuadRemesh component"));
    }
  }
  if (pieces.size() < 2) {
    return;
  }

  bool have_density =
      work.v.attrs.has(mesh::AttrType::FLOAT, util::string(".remesh.v.density"));
  bool have_pinned =
      work.v.attrs.has(mesh::AttrType::BOOL, util::string(".remesh.v.pole_pinned"));
  bool have_stroke =
      work.f.attrs.has(mesh::AttrType::FLOAT3, util::string(".remesh.f.stroke_dir"));
  mesh::BuiltinAttr<float, ".remesh.v.density"> srcDensity;
  mesh::BuiltinAttr<bool, ".remesh.v.pole_pinned"> srcPinned;
  mesh::BuiltinAttr<math::float3, ".remesh.f.stroke_dir"> srcStroke;
  if (have_density) {
    srcDensity.ensure(work.v.attrs);
  }
  if (have_pinned) {
    srcPinned.ensure(work.v.attrs);
  }
  if (have_stroke) {
    srcStroke.ensure(work.f.attrs);
  }

  util::Vector<int> vmap;
  vmap.resize(vcap);
  util::Vector<int> vs;
  for (int pi = 0; pi < int(pieces.size()); pi++) {
    Mesh *dst = pieces[pi];
    mesh::BuiltinAttr<float, ".remesh.v.density"> dstDensity;
    mesh::BuiltinAttr<bool, ".remesh.v.pole_pinned"> dstPinned;
    mesh::BuiltinAttr<math::float3, ".remesh.f.stroke_dir"> dstStroke;
    if (have_density) {
      dstDensity.ensure(dst->v.attrs);
    }
    if (have_pinned) {
      dstPinned.ensure(dst->v.attrs);
    }
    if (have_stroke) {
      dstStroke.ensure(dst->f.attrs);
    }
    for (int v : work.v) {
      int r = find(v);
      if (!rootPiece.contains(r) || rootPiece.lookup(r) != pi) {
        continue;
      }
      int nv = dst->make_vertex(work.v.co[v]);
      vmap[v] = nv;
      if (have_density) {
        dstDensity[nv] = srcDensity[v];
      }
      if (have_pinned) {
        dstPinned.set(nv, srcPinned[v]);
      }
    }
    for (int f : work.f) {
      int c0 = work.l.c[work.f.l[f]];
      if (rootPiece.lookup(find(work.c.v[c0])) != pi) {
        continue;
      }
      vs.clear();
      int cc = c0;
      do {
        vs.append(vmap[work.c.v[cc]]);
        cc = work.c.next[cc];
      } while (cc != c0);
      int nf = dst->make_face(vs);
      if (have_stroke) {
        dstStroke[nf] = srcStroke[f];
      }
    }
    dst->recalc_normals();
  }
}

/* Append @p src's verts + faces into @p dst (geometry only — the per-component
 * outputs carry no consumed attrs; the caller recalcs normals once). */
void appendMesh(Mesh &dst, Mesh &src)
{
  util::Vector<int> vmap;
  vmap.resize(int(src.v.capacity()));
  for (int v : src.v) {
    vmap[v] = dst.make_vertex(src.v.co[v]);
  }
  util::Vector<int> vs;
  for (int f : src.f) {
    int c0 = src.l.c[src.f.l[f]];
    vs.clear();
    int cc = c0;
    do {
      vs.append(vmap[src.c.v[cc]]);
      cc = src.c.next[cc];
    } while (cc != c0);
    dst.make_face(vs);
  }
}

/* Tier 6.4: fold one successful sub-run's report into the per-component
 * aggregate — statuses max-merge (Failed > Ok > Skipped), counters/timings
 * sum, feasibility flags AND, extrema min/max-merge. */
void mergeComponentReport(RemeshRunReport &dst, const RemeshRunReport &src,
                          bool first)
{
  auto status = [](StageStatus &d, StageStatus s) {
    if (int(s) > int(d)) {
      d = s;
    }
  };
  status(dst.pre_remesh, src.pre_remesh);
  status(dst.cross_field, src.cross_field);
  status(dst.singularity, src.singularity);
  status(dst.quantize, src.quantize);
  status(dst.extract, src.extract);
  status(dst.reproject, src.reproject);

  dst.num_singularities += src.num_singularities;
  dst.index_sum += src.index_sum; // Poincare-Hopf: 4-chi sums over components
  dst.field_solved_eigen = dst.field_solved_eigen || src.field_solved_eigen;
  dst.field_close_pairs += src.field_close_pairs;
  dst.field_clutter_verts += src.field_clutter_verts;
  dst.cancel_attempted_pairs += src.cancel_attempted_pairs;
  dst.cancel_cancelled_pairs += src.cancel_cancelled_pairs;
  dst.cancel_reverted_rounds += src.cancel_reverted_rounds;
  dst.cancel_singularities_after += src.cancel_singularities_after;
  dst.parametrization_folds += src.parametrization_folds;
  dst.solve_faces += src.solve_faces;
  dst.min_jacobian =
      first ? src.min_jacobian : std::fmin(dst.min_jacobian, src.min_jacobian);
  dst.quantize_feasible = first ? src.quantize_feasible
                                : (dst.quantize_feasible && src.quantize_feasible);

  QuantizeStats &dq = dst.quantize_stats;
  const QuantizeStats &sq = src.quantize_stats;
  if (first) {
    dq = sq;
  } else {
    dq.num_faces += sq.num_faces;
    dq.num_corners += sq.num_corners;
    dq.num_classes += sq.num_classes;
    dq.num_cut_edges += sq.num_cut_edges;
    dq.max_integer_residual =
        std::fmax(dq.max_integer_residual, sq.max_integer_residual);
    dq.max_loop_closure = std::fmax(dq.max_loop_closure, sq.max_loop_closure);
    dq.min_jacobian = std::fmin(dq.min_jacobian, sq.min_jacobian);
    dq.parametrization_folds += sq.parametrization_folds;
    dq.iters += sq.iters;
    dq.solved = dq.solved && sq.solved;
    dq.feasible = dq.feasible && sq.feasible;
    dq.num_singularities += sq.num_singularities;
    dq.spurious_pairs += sq.spurious_pairs;
    dq.seamless_folds += sq.seamless_folds;
    dq.seamless_folds_near_pairs += sq.seamless_folds_near_pairs;
    dq.full_refactors += sq.full_refactors;
    dq.updowns += sq.updowns;
    dq.simp_refreshes += sq.simp_refreshes;
    dq.back_solves += sq.back_solves;
    dq.tier1b_probes += sq.tier1b_probes;
    dq.gs_rounds += sq.gs_rounds;
    dq.gs_converged += sq.gs_converged;
    dq.gs_visits += sq.gs_visits;
    dq.gs_touched_total += sq.gs_touched_total;
    dq.gs_touched_max = std::max(dq.gs_touched_max, sq.gs_touched_max);
    dq.resort_full += sq.resort_full;
    dq.resort_incr += sq.resort_incr;
    dq.resort_keys += sq.resort_keys;
    dq.total_ms += sq.total_ms;
    dq.setup_ms += sq.setup_ms;
    dq.initial_factor_ms += sq.initial_factor_ms;
    dq.arap_ms += sq.arap_ms;
    dq.rounding_ms += sq.rounding_ms;
    dq.round_assemble_ms += sq.round_assemble_ms;
    dq.round_refactor_ms += sq.round_refactor_ms;
    dq.round_updown_ms += sq.round_updown_ms;
    dq.round_backsolve_ms += sq.round_backsolve_ms;
    dq.convert_ms += sq.convert_ms;
    dq.gs_ms += sq.gs_ms;
    dq.tier1b_ms += sq.tier1b_ms;
    dq.stiffen_ms += sq.stiffen_ms;
    dq.tier3_ms += sq.tier3_ms;
  }

  ExtractStats &de = dst.extract_stats;
  const ExtractStats &se = src.extract_stats;
  if (first) {
    de = se;
  } else {
    de.num_grid_verts += se.num_grid_verts;
    de.num_quads += se.num_quads;
    de.num_arcs += se.num_arcs;
    de.open_arcs += se.open_arcs;
    de.nonquad_cells += se.nonquad_cells;
    de.holes_capped += se.holes_capped;
    de.holes_capped_odd += se.holes_capped_odd;
    de.odd_rims_paired += se.odd_rims_paired;
    de.holes_pinched_split += se.holes_pinched_split;
    de.holes_open += se.holes_open;
    de.holes_open_border += se.holes_open_border;
    de.holes_open_odd += se.holes_open_odd;
    de.holes_open_size += se.holes_open_size;
    de.holes_open_untraced += se.holes_open_untraced;
    de.ok = de.ok && se.ok;
  }

  // PreRemeshEffect: census counts sum; the per-run scalars (means, resolved
  // targets/iters) keep the first piece's values.
  auto &dp = dst.pre_remesh_effect;
  const auto &sp = src.pre_remesh_effect;
  if (first) {
    dp = sp;
  } else if (sp.ran) {
    dp.ran = true;
    dp.verts_in += sp.verts_in;
    dp.faces_in += sp.faces_in;
    dp.verts_out += sp.verts_out;
    dp.faces_out += sp.faces_out;
    dp.fold90_in += sp.fold90_in;
    dp.fold180_in += sp.fold180_in;
    dp.degen_in += sp.degen_in;
    dp.fold90_out += sp.fold90_out;
    dp.fold180_out += sp.fold180_out;
    dp.degen_out += sp.degen_out;
    dp.iters_run = std::max(dp.iters_run, sp.iters_run);
    dp.converged = dp.converged && sp.converged;
    dp.coarsen_bootstrap = dp.coarsen_bootstrap || sp.coarsen_bootstrap;
    dp.duration_ms += sp.duration_ms;
  }
}

/* Maps a sub-run's local 0..100 progress into the global [8,99] band for
 * component idx of total. */
struct CompProgress {
  RemeshProgressFn fn = nullptr;
  void *user = nullptr;
  int idx = 0, total = 1;
};

void compProgressThunk(void *user, int pct, const char *stage)
{
  auto *cp = static_cast<CompProgress *>(user);
  int g = 8 + (cp->idx * 100 + pct) * 91 / (cp->total * 100);
  cp->fn(cp->user, g, stage);
}

/* Tier 6.4 driver: run QuadRemesh on each piece with the shared global scale
 * and merge the outputs. Failed pieces are dropped + counted; returns nullptr
 * only when every piece fails (failure_reason = the first piece's tag). */
Mesh *remeshPerComponent(util::Vector<Mesh *> &pieces, const RemeshParams &params,
                         float L_quad, RemeshProgressFn progress, void *user,
                         RemeshRunReport *report)
{
  RemeshParams sub = params;
  sub.per_component = false;
  sub.triage = false;                  // ran globally on the work mesh
  sub.input_hole_fill_max_frac = 0.0f; // ran globally on the work mesh
  sub.target_edge_length = L_quad;     // one shared global scale, no count mode
  sub.auto_retry = false;              // Tier 8 retries the whole run, not pieces

  Mesh *merged = alloc::New<Mesh>("Mesh QuadRemesh merged");
  int total = int(pieces.size()), done = 0, failed = 0;
  std::string firstFailure;
  bool first = true;
  for (int i = 0; i < total; i++) {
    CompProgress cp{progress, user, i, total};
    RemeshRunReport sr;
    Mesh *piece_out =
        QuadRemesh(*pieces[i], sub, progress ? compProgressThunk : nullptr,
                   progress ? &cp : nullptr, report ? &sr : nullptr);
    if (!piece_out) {
      failed++;
      if (firstFailure.empty()) {
        firstFailure = sr.failure_reason;
      }
      continue;
    }
    appendMesh(*merged, *piece_out);
    alloc::Delete<Mesh>(piece_out);
    done++;
    if (report) {
      mergeComponentReport(*report, sr, first);
      first = false;
    }
  }
  if (report) {
    report->components_total = total;
    report->components_remeshed = done;
    report->components_failed = failed;
  }
  if (done == 0) {
    alloc::Delete<Mesh>(merged);
    if (report) {
      report->failure_reason =
          firstFailure.empty() ? "per_component_all_failed" : firstFailure;
    }
    return nullptr;
  }
  merged->recalc_normals();
  return merged;
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

const char *remeshPresetName(int i)
{
  static const char *names[] = {"organic-clean", "organic-noisy",
                                "messy-character", "scan", "hard-surface"};
  return (i >= 0 && i < int(sizeof(names) / sizeof(names[0]))) ? names[i]
                                                               : nullptr;
}

/* Preset deltas encode the tier sweep results: feature_min_chain=3 everywhere
 * (the gate-7 "safe quality knob"), hysteresis only paired with it on the
 * noisy/scan bundles, cap_odd_holes only where watertightness beats cap
 * quality (gate 6), and auto_retry only on inputs expected to misbehave. */
bool applyRemeshPreset(RemeshParams &params, const char *name)
{
  RemeshParams base;
  // Sizing + determinism are orthogonal to the preset character.
  base.target_quad_count = params.target_quad_count;
  base.target_edge_length = params.target_edge_length;
  base.seed = params.seed;
  base.feature_min_chain = 3;
  base.auto_density = true;

  if (std::strcmp(name, "organic-clean") == 0) {
    base.curvature_smooth_iters = 1;
  } else if (std::strcmp(name, "organic-noisy") == 0) {
    base.curvature_smooth_iters = 2;
    base.field_smoothness = 2.0f;
    base.feature_hysteresis = 0.2618f; // ~15deg, paired with min_chain
    base.pre_remesh = true;
    base.auto_retry = true;
  } else if (std::strcmp(name, "messy-character") == 0) {
    base.curvature_smooth_iters = 2;
    base.triage_min_component_frac = 0.01f;
    base.input_hole_fill_max_frac = 0.05f;
    base.per_component = true;
    base.cap_odd_holes = true;
    base.pre_remesh = true;
    base.auto_retry = true;
  } else if (std::strcmp(name, "scan") == 0) {
    base.curvature_smooth_iters = 2;
    base.field_smoothness = 2.0f;
    base.feature_hysteresis = 0.2618f;
    base.triage_min_component_frac = 0.01f;
    base.input_hole_fill_max_frac = 0.05f;
    base.cap_odd_holes = true;
    base.pre_remesh = true;
    base.auto_retry = true;
  } else if (std::strcmp(name, "hard-surface") == 0) {
    base.sharp_angle = 0.5235988f; // 30deg: catch real shallow bevels
    base.density_gradation = 0.3f; // smooth size flow around fillets
  } else {
    return false;
  }
  params = base;
  return true;
}

/* One full pipeline run. The public QuadRemesh below wraps this in the Tier-8
 * retry loop; with auto_retry off it is called exactly once (legacy path). */
static mesh::Mesh *quadRemeshAttempt(mesh::Mesh &input,
                                     const RemeshParams &params,
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
  // faces / tiny components, detect non-manifold). Defaults on; on clean input
  // it is a no-op (byte-identical), so the merge stays behavior-preserving.
  TriageReport tr;
  if (params.triage) {
    PROG(6, "triage");
    TriageParams tp;
    tp.weld_rel = params.triage_weld_rel;
    tp.min_component_frac = params.triage_min_component_frac;
    triageMesh(*work, tp, tr);
    if (report)
      report->triage = StageStatus::Ok;
  }

  // Tier 6.3: pre-solve input hole policy — fill tiny input boundary loops so
  // they don't seed spurious boundary constraints in the field solve. Large
  // boundaries stay open (the extract cap path classifies them as border).
  if (params.input_hole_fill_max_frac > 0.0f) {
    PROG(7, "hole_fill");
    fillInputHoles(*work, params.input_hole_fill_max_frac, tr);
  }

  // Resolve the operative quad edge length: explicit target_edge_length, or
  // derived from target_quad_count (L = sqrt(integral of density dA / N)).
  // Count mode refines L further below as the work mesh / density evolve.
  const bool count_mode = params.target_edge_length <= 0.0f;
  float L_quad = resolveTargetEdgeLength(*work, params);

  // Tier 6.6: detect-only thin double-sided sheet check. Opposing sheets closer
  // than half a quad edge can't be resolved at L_quad (the field/param see both
  // sides in one cell) — flagged as known-poor in the report, never repaired.
  if (params.triage) {
    detectThinSheets(*work, 0.5f * L_quad, tr);
  }
  if (report)
    report->triage_report = tr;

  // Tier 6.4: remesh disconnected components independently so one component's
  // field/singularities can't leak into another's solve. Sub-runs inherit the
  // globally resolved L_quad (explicit-length mode — triage / hole fill /
  // count-mode sizing already ran globally above); failed pieces are dropped.
  if (params.per_component) {
    util::Vector<Mesh *> pieces;
    splitComponents(*work, pieces);
    if (pieces.size() > 1) {
      PROG(8, "components");
      Mesh *merged =
          remeshPerComponent(pieces, params, L_quad, progress, user, report);
      for (Mesh *p : pieces) {
        alloc::Delete<Mesh>(p);
      }
      alloc::Delete<Mesh>(work);
      PROG(100, merged ? "done" : "failed");
      if (report) {
        report->duration_ms = elapsedMs();
        if (merged) {
          report->success = true;
          report->derived_edge_length = L_quad;
          report->quad_count_actual = merged->f.count;
          report->validation = mesh::remeshValidate(*merged);
          report->validation.parametrization_folds = report->parametrization_folds;
          report->validation_filled = true;
        }
      }
      return merged;
    }
    for (Mesh *p : pieces) {
      alloc::Delete<Mesh>(p);
    }
  }

  // Tier 9 (9d): field-aligned input pre-remesh — clean the working
  // triangulation's flow before the field solve. Sentinel knobs (target=0,
  // iters=0, bootstrap=-1) auto-resolve from the measured input; explicit
  // values always win.
  bool pre_remeshed = false;
  if (params.pre_remesh) {
    PROG(14, "pre_remesh");
    auto t_pre = std::chrono::steady_clock::now();

    EdgeStats es = measureEdges(*work);

    // Target: explicit > output resolution (count mode: 0.7x the quad edge,
    // floored at half the median input edge — see resolvePreRemeshTarget;
    // inlined here to reuse L_quad and es).
    float L_pre = params.pre_remesh_target > 0.0f
                      ? params.pre_remesh_target
                      : (count_mode ? std::fmax(0.7f * L_quad, 0.5f * es.median)
                                    : params.target_edge_length);
    // Edge budget (auto targets only — explicit pre_remesh_target wins):
    // the pre-pass may not coarsen away more than 20% of the edges it
    // receives. E scales ~1/L^2, so E_out/E_in >= 0.8 ==> L <= mean/sqrt(0.8).
    const bool budgeted =
        params.pre_remesh_target <= 0.0f && es.mean > 0.0f;
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
    // target, BK-coarsen once field-free before the loop.
    bool coarsen = es.mean > 0.0f && es.mean < 0.5f * L_pre;
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
  cp.feature_hysteresis = params.feature_hysteresis;
  cp.feature_min_chain = params.feature_min_chain;
  cp.seed = params.seed;
  cp.curvature_smooth_iters = params.curvature_smooth_iters;
  cp.curvature_smooth_lambda = params.curvature_smooth_lambda;
  cp.field_smoothness = params.field_smoothness;
  cp.curvature_weight = params.curvature_weight;
  CrossFieldStats cfs = computeCrossField(*work, cp);
  if (report) {
    report->cross_field = StageStatus::Ok;
    report->solve_faces = work->f.count;
    report->num_singularities = cfs.num_singularities;
    report->index_sum = cfs.index_sum;
    report->field_solved_eigen = cfs.solved_eigen;
    SingularityPairStats sps = findSingularityPairs(*work, 2);
    report->field_close_pairs = sps.close_pairs;
    report->field_clutter_verts = sps.clutter_verts;
  }

  PROG(45, "singularity");
  SingularityAdjustParams sap;
  sap.seed = params.seed;
  adjustSingularities(*work, sap);
  if (report)
    report->singularity = StageStatus::Ok;

  // Tier 5: annihilate sub-resolution noise pairs while the field is still
  // cheap to edit. L_quad is the pre-density estimate; the gate needs scale only.
  if (params.singularity_cancel) {
    SingularityCancelParams scp;
    scp.target_edge_length = L_quad;
    scp.max_sep = params.singularity_cancel_max_sep;
    scp.seed = params.seed;
    SingularityCancelStats scs = cancelSingularityPairs(*work, scp);
    if (report) {
      report->cancel_attempted_pairs = scs.attempted_pairs;
      report->cancel_cancelled_pairs = scs.cancelled_pairs;
      report->cancel_reverted_rounds = scs.reverted_rounds;
      report->cancel_singularities_after = scs.num_singularities;
    }
  }

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
    // Re-derive on the current geometry: the pre-remesh shifted the surface
    // area (and a painted density field) the initial estimate used.
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
  qp.rounding = params.quantize_direct_rounding ? RoundingStrategy::DIRECT
                                                : RoundingStrategy::GREEDY;
  qp.untangle_field_max_dev = double(params.untangle_field_max_dev);
  QuantizeStats qs = computeQuantization(*work, qp);
  if (report) {
    report->quantize = StageStatus::Ok;
    report->parametrization_folds = qs.parametrization_folds;
    report->min_jacobian = qs.min_jacobian;
    report->quantize_feasible = qs.feasible;
    report->quantize_stats = qs;
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
    report->extract_stats = st;
  }

  if (params.reproject) {
    PROG(92, "reproject");
    ReprojectParams rp;
    rp.smooth_iterations = params.smooth_iterations;
    rp.smooth_lambda = params.smooth_strength;
    // One extra [smooth -> snap] pass when smoothing, else a single pure snap.
    rp.iterations = params.smooth_iterations > 0 ? 2 : 1;
    if (pre_remeshed) {
      // The work geometry diverged from the input (the pre-remesh moved every
      // vertex) — snap onto the ORIGINAL full-res surface so the output
      // recovers the detail the pass smoothed away.
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

/* Tier 8a retry machinery. Each escalation rung fires at most once per run;
 * the rules read the previous attempt's report and bump exactly one knob. */
namespace {

struct RetryLadder {
  bool field_smoothness = false;
  bool curvature_smooth = false;
  bool density_gradation = false;
  bool pre_remesh = false;
  bool coarser_target = false;
};

void coarsenTarget(RemeshParams &cur)
{
  if (cur.target_edge_length > 0.0f) {
    cur.target_edge_length *= 1.25f;
  } else {
    cur.target_quad_count =
        std::max(500, int(0.7f * float(cur.target_quad_count)));
  }
}

void fillRetryAttempt(RemeshRunReport::RetryAttempt &a, const RemeshParams &p,
                      const char *escalation, const RemeshRunReport &rep)
{
  a.params = p;
  a.escalation = escalation;
  a.from_original = true;
  a.success = rep.success;
  a.failure_reason = rep.failure_reason;
  a.parametrization_folds = rep.parametrization_folds;
  a.num_singularities = p.singularity_cancel ? rep.cancel_singularities_after
                                             : rep.num_singularities;
  a.inverted_faces = rep.validation_filled ? rep.validation.inverted_faces : 0;
  a.odd_residuals =
      rep.extract_stats.holes_open_odd +
      (rep.extract_stats.holes_capped_odd - rep.extract_stats.odd_rims_paired);
  a.max_adjacent_edge_ratio =
      rep.validation_filled ? rep.validation.max_adjacent_edge_ratio : 0.0f;
  a.duration_ms = rep.duration_ms;
}

/* Lexicographic: success, then fewer pre-extraction folds, then fewer inverted
 * output faces, then fewer singularities. Strict — first best wins ties. */
bool retryBetter(const RemeshRunReport::RetryAttempt &a,
                 const RemeshRunReport::RetryAttempt &b)
{
  if (a.success != b.success) {
    return a.success;
  }
  if (a.parametrization_folds != b.parametrization_folds) {
    return a.parametrization_folds < b.parametrization_folds;
  }
  if (a.inverted_faces != b.inverted_faces) {
    return a.inverted_faces < b.inverted_faces;
  }
  return a.num_singularities < b.num_singularities;
}

/* Pick the next escalation from the last attempt's outcome: mutate `cur` and
 * return the rung tag, or nullptr to stop (result acceptable / ladder spent). */
const char *escalateParams(RemeshParams &cur, RetryLadder &used,
                           const RemeshRunReport::RetryAttempt &last,
                           const RemeshRunReport &rep)
{
  // Fold tolerance scales with the solve mesh; the floor keeps tiny meshes sane.
  const int fold_limit =
      std::max(10, rep.solve_faces > 0 ? rep.solve_faces / 100 : 10);
  const bool noisy_folds = last.parametrization_folds > fold_limit;
  // Pole budget: ~5% of the realized (or requested) quad count.
  const int pole_denom =
      rep.quad_count_actual > 0 ? rep.quad_count_actual : cur.target_quad_count;

  if (noisy_folds && !used.field_smoothness) {
    used.field_smoothness = true;
    cur.field_smoothness *= 2.0f;
    return "field_smoothness";
  }
  if (last.num_singularities > std::max(4, pole_denom / 20) &&
      !used.curvature_smooth) {
    used.curvature_smooth = true;
    cur.curvature_smooth_iters = std::max(2, cur.curvature_smooth_iters * 2);
    cur.singularity_cancel = true;
    return "curvature_smooth";
  }
  if (last.success && last.max_adjacent_edge_ratio > 4.0f &&
      !used.density_gradation) {
    // Steeper limiting = LOWER growth-rate cap; the field must also be consumed
    // (auto_density) or the tightened gradation would never reach the quantizer.
    used.density_gradation = true;
    cur.density_gradation =
        cur.density_gradation > 0.0f ? 0.6f * cur.density_gradation : 0.3f;
    cur.auto_density = true;
    return "density_gradation";
  }
  if (noisy_folds && !used.pre_remesh) {
    // Folds persist after the smoothness rung — clean the input triangulation.
    used.pre_remesh = true;
    cur.pre_remesh = true;
    return "pre_remesh";
  }
  if (last.success && last.odd_residuals > 2 && !used.coarser_target) {
    used.coarser_target = true;
    coarsenTarget(cur);
    return "coarser_target";
  }
  if (!last.success) {
    // Outright failure with no metric rule left: walk the fallback ladder.
    if (!used.field_smoothness) {
      used.field_smoothness = true;
      cur.field_smoothness *= 2.0f;
      return "field_smoothness";
    }
    if (!used.pre_remesh) {
      used.pre_remesh = true;
      cur.pre_remesh = true;
      return "pre_remesh";
    }
    if (!used.coarser_target) {
      used.coarser_target = true;
      coarsenTarget(cur);
      return "coarser_target";
    }
  }
  return nullptr;
}

} // namespace

mesh::Mesh *QuadRemesh(mesh::Mesh &input, const RemeshParams &params,
                       RemeshProgressFn progress, void *user,
                       RemeshRunReport *report)
{
  if (!params.auto_retry) {
    return quadRemeshAttempt(input, params, progress, user, report);
  }

  const int cap =
      std::clamp(params.max_attempts, 1, RemeshRunReport::MAX_RETRY_ATTEMPTS);
  RemeshParams cur = params;
  cur.auto_retry = false;
  RetryLadder used;
  const char *escalation = "initial";

  RemeshRunReport::RetryAttempt trail[RemeshRunReport::MAX_RETRY_ATTEMPTS];
  int n = 0, best_idx = -1;
  Mesh *best = nullptr;
  RemeshRunReport best_rep;

  while (n < cap) {
    RemeshRunReport rep;
    Mesh *out = quadRemeshAttempt(input, cur, progress, user, &rep);
    const int idx = n++;
    fillRetryAttempt(trail[idx], cur, escalation, rep);
    if (best_idx < 0 || retryBetter(trail[idx], trail[best_idx])) {
      if (best) {
        alloc::Delete<Mesh>(best);
      }
      best = out;
      best_rep = rep;
      best_idx = idx;
    } else if (out) {
      alloc::Delete<Mesh>(out);
    }
    if (n >= cap) {
      break;
    }
    escalation = escalateParams(cur, used, trail[idx], rep);
    if (!escalation) {
      break;
    }
  }

  if (report) {
    *report = best_rep;
    for (int i = 0; i < n; i++) {
      report->attempts[i] = trail[i];
    }
    report->attempts_run = n;
    report->winner = best_idx;
  }
  return best;
}

} // namespace sculptcore::remesh
