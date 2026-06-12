// Standalone quad-remesh CLI: load one OBJ, run QuadRemesh, write the result
// mesh + a JSON manifest to an output dir, and stream coarse progress + result
// lines to stdout for a parent process (the interactive debug app) to read.
//
// Protocol (one record per line, stdout, unbuffered):
//   PROGRESS <pct> <stage>      monotonic 0..100; stages copy..done, or "failed"
//   RESULT <obj-path>           written quad mesh
//   MANIFEST <json-path>        written manifest
//   STATS k=v k=v ...           summary (verts/quads/tris/manifold/euler/spiral/ms)
//   ERROR <message>             fatal (also exit != 0)
//
// It is a fresh process per run, so it does no global init beyond what the remesh
// libs do statically (mirrors tests/test_remesh_extract.cc, which also just calls
// QuadRemesh directly).

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"

#include "mesh/mesh.h"
#include "mesh/utils/mesh_validate.h"
#include "mesh/utils/obj_io.h"

#include "remesh/remesh.h"
#include "remesh/remesh_params.h"
#include "remesh/remesh_report.h"

#include "asset_quads.h"
#include "remesh_cli_config.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

using namespace sculptcore;
using litestl::math::float3;

namespace {

namespace fs = std::filesystem;

void progressCb(void * /*user*/, int pct, const char *stage)
{
  std::printf("PROGRESS %d %s\n", pct, stage);
}

// "YYYYMMDD-HHMMSS" local time. A fresh CLI process; no determinism concern.
std::string timestamp()
{
  std::time_t t = std::time(nullptr);
  std::tm tmv{};
#ifdef _WIN32
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tmv);
  return buf;
}

// Basename with no extension, e.g. ".../AnimeGirl2.obj" -> "AnimeGirl2".
std::string stem(const std::string &path)
{
  return fs::path(path).stem().string();
}

// Manual min/max AABB — mesh::calcAABB seeds max to FLT_MIN and mis-handles
// all-negative meshes, so compute it explicitly here.
void inputAABB(mesh::Mesh &m, float3 &bmin, float3 &bmax)
{
  bool have = false;
  for (int v : m.v) {
    float3 co = m.v.co[v];
    if (!have) {
      bmin = bmax = co;
      have = true;
      continue;
    }
    for (int i = 0; i < 3; i++) {
      if (co[i] < bmin[i])
        bmin[i] = co[i];
      if (co[i] > bmax[i])
        bmax[i] = co[i];
    }
  }
  if (!have)
    bmin = bmax = float3(0, 0, 0);
}

const char *jb(bool b) { return b ? "true" : "false"; }

const char *ssName(remesh::StageStatus s)
{
  switch (s) {
  case remesh::StageStatus::Ok:
    return "ok";
  case remesh::StageStatus::Failed:
    return "failed";
  default:
    return "skipped";
  }
}

// Escape a string for embedding in a JSON double-quoted value. Critically this
// turns Windows path backslashes into `\\` (a bare `\s` is an invalid JSON
// escape that breaks strict parsers like JS JSON.parse).
std::string jstr(const std::string &s)
{
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
    case '\\': o += "\\\\"; break;
    case '"': o += "\\\""; break;
    case '\n': o += "\\n"; break;
    case '\r': o += "\\r"; break;
    case '\t': o += "\\t"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        o += buf;
      } else {
        o += c;
      }
    }
  }
  return o;
}

bool writeManifest(const char *path, const std::string &jsonName,
                   const std::string &objPath, const std::string &inName,
                   const std::string &inPath, int inVerts, int inFaces,
                   const float3 &amin, const float3 &amax,
                   const remesh::RemeshParams &p, const mesh::RemeshReport &r,
                   const mesh::RemeshReport &rin,
                   const remesh::RemeshRunReport &rep, long long durationMs)
{
  std::FILE *f = std::fopen(path, "wb");
  if (!f)
    return false;

  auto f3 = [&](const float3 &v) {
    std::fprintf(f, "[%.9g, %.9g, %.9g]", v[0], v[1], v[2]);
  };

  std::fprintf(f, "{\n");
  std::fprintf(f, "  \"schema\": \"remesh-manifest/1\",\n");
  std::fprintf(f, "  \"timestamp\": \"%s\",\n", jstr(jsonName).c_str());
  std::fprintf(f, "  \"git_commit\": \"%s\",\n", jstr(REMESH_CLI_GIT_COMMIT).c_str());
  std::fprintf(f, "  \"duration_ms\": %lld,\n", durationMs);

  std::fprintf(f, "  \"input\": {\n");
  std::fprintf(f, "    \"name\": \"%s\",\n", jstr(inName).c_str());
  std::fprintf(f, "    \"path\": \"%s\",\n", jstr(inPath).c_str());
  std::fprintf(f, "    \"verts\": %d,\n", inVerts);
  std::fprintf(f, "    \"faces\": %d,\n", inFaces);
  std::fprintf(f, "    \"components\": %d,\n", rin.component_count);
  std::fprintf(f, "    \"holes\": %d,\n", rin.boundary_loop_count);
  std::fprintf(f, "    \"manifold\": %s,\n", jb(rin.manifold));
  std::fprintf(f, "    \"aabb_min\": ");
  f3(amin);
  std::fprintf(f, ",\n    \"aabb_max\": ");
  f3(amax);
  std::fprintf(f, "\n  },\n");

  std::fprintf(f, "  \"params\": {\n");
  std::fprintf(f, "    \"target_quad_count\": %d,\n", p.target_quad_count);
  std::fprintf(f, "    \"target_edge_length\": %.9g,\n", p.target_edge_length);
  std::fprintf(f, "    \"solve_edge_length\": %.9g,\n", p.solve_edge_length);
  std::fprintf(f, "    \"use_curvature\": %s,\n", jb(p.use_curvature));
  std::fprintf(f, "    \"use_sharp_features\": %s,\n", jb(p.use_sharp_features));
  std::fprintf(f, "    \"sharp_angle\": %.9g,\n", p.sharp_angle);
  std::fprintf(f, "    \"use_density\": %s,\n", jb(p.use_density));
  std::fprintf(f, "    \"quantize_direct_rounding\": %s,\n",
               jb(p.quantize_direct_rounding));
  std::fprintf(f, "    \"reproject\": %s,\n", jb(p.reproject));
  std::fprintf(f, "    \"cap_odd_holes\": %s,\n", jb(p.cap_odd_holes));
  std::fprintf(f, "    \"smooth_iterations\": %d,\n", p.smooth_iterations);
  std::fprintf(f, "    \"smooth_strength\": %.9g,\n", p.smooth_strength);
  std::fprintf(f, "    \"seed\": %u,\n", p.seed);
  std::fprintf(f, "    \"triage\": %s,\n", jb(p.triage));
  std::fprintf(f, "    \"triage_weld_rel\": %.9g,\n", p.triage_weld_rel);
  std::fprintf(f, "    \"triage_min_component_frac\": %.9g,\n",
               p.triage_min_component_frac);
  std::fprintf(f, "    \"input_hole_fill_max_frac\": %.9g,\n",
               p.input_hole_fill_max_frac);
  std::fprintf(f, "    \"per_component\": %s,\n", jb(p.per_component));
  std::fprintf(f, "    \"curvature_smooth_iters\": %d,\n",
               p.curvature_smooth_iters);
  std::fprintf(f, "    \"curvature_smooth_lambda\": %.9g,\n",
               p.curvature_smooth_lambda);
  std::fprintf(f, "    \"field_smoothness\": %.9g,\n", p.field_smoothness);
  std::fprintf(f, "    \"curvature_weight\": %.9g,\n", p.curvature_weight);
  std::fprintf(f, "    \"singularity_cancel\": %s,\n", jb(p.singularity_cancel));
  std::fprintf(f, "    \"singularity_cancel_max_sep\": %.9g,\n",
               p.singularity_cancel_max_sep);
  std::fprintf(f, "    \"auto_density\": %s,\n", jb(p.auto_density));
  std::fprintf(f, "    \"density_min\": %.9g,\n", p.density_min);
  std::fprintf(f, "    \"density_max\": %.9g,\n", p.density_max);
  std::fprintf(f, "    \"density_gradation\": %.9g,\n", p.density_gradation);
  std::fprintf(f, "    \"density_gradation_iters\": %d,\n",
               p.density_gradation_iters);
  std::fprintf(f, "    \"pre_remesh\": %s,\n", jb(p.pre_remesh));
  std::fprintf(f, "    \"pre_remesh_target\": %.9g,\n", p.pre_remesh_target);
  std::fprintf(f, "    \"pre_remesh_iters\": %d,\n", p.pre_remesh_iters);
  std::fprintf(f, "    \"pre_remesh_density\": %s,\n", jb(p.pre_remesh_density));
  std::fprintf(f, "    \"pre_remesh_gradation\": %.9g,\n", p.pre_remesh_gradation);
  std::fprintf(f, "    \"pre_remesh_gradation_iters\": %d,\n",
               p.pre_remesh_gradation_iters);
  std::fprintf(f, "    \"pre_remesh_align\": %.9g,\n", p.pre_remesh_align);
  std::fprintf(f, "    \"pre_remesh_field_cadence\": %d,\n",
               p.pre_remesh_field_cadence);
  std::fprintf(f, "    \"pre_remesh_bootstrap_iters\": %d,\n",
               p.pre_remesh_bootstrap_iters);
  std::fprintf(f, "    \"pre_remesh_smooth_iters\": %d,\n",
               p.pre_remesh_smooth_iters);
  std::fprintf(f, "    \"pre_remesh_smooth_lambda\": %.9g,\n",
               p.pre_remesh_smooth_lambda);
  std::fprintf(f, "    \"pre_remesh_converge_eps\": %.9g,\n",
               p.pre_remesh_converge_eps);
  std::fprintf(f, "    \"pre_remesh_preserve_features\": %s,\n",
               jb(p.pre_remesh_preserve_features));
  std::fprintf(f, "    \"pre_remesh_sharp_angle\": %.9g,\n",
               p.pre_remesh_sharp_angle);
  std::fprintf(f, "    \"pre_remesh_trace\": %s\n", jb(p.pre_remesh_trace));
  std::fprintf(f, "  },\n");

  std::fprintf(f, "  \"output\": {\n");
  std::fprintf(f, "    \"path\": \"%s\",\n", jstr(objPath).c_str());
  std::fprintf(f, "    \"verts\": %d,\n", r.vert_count);
  std::fprintf(f, "    \"edges\": %d,\n", r.edge_count);
  std::fprintf(f, "    \"faces\": %d,\n", r.face_count);
  std::fprintf(f, "    \"quads\": %d,\n", r.quad_count);
  std::fprintf(f, "    \"tris\": %d,\n", r.tri_count);
  std::fprintf(f, "    \"ngons\": %d\n", r.ngon_count);
  std::fprintf(f, "  },\n");

  std::fprintf(f, "  \"validation\": {\n");
  std::fprintf(f, "    \"manifold\": %s,\n", jb(r.manifold));
  std::fprintf(f, "    \"manifold_error\": \"%s\",\n", jstr(r.manifold_error).c_str());
  std::fprintf(f, "    \"euler\": %d,\n", r.euler);
  std::fprintf(f, "    \"consistent_winding\": %s,\n", jb(r.consistent_winding));
  std::fprintf(f, "    \"non_manifold_edges\": %d,\n", r.non_manifold_edges);
  std::fprintf(f, "    \"boundary_edges\": %d,\n", r.boundary_edges);
  std::fprintf(f, "    \"degenerate_faces\": %d,\n", r.degenerate_faces);
  std::fprintf(f, "    \"inverted_faces\": %d,\n", r.inverted_faces);
  std::fprintf(f, "    \"all_quad\": %s,\n", jb(r.all_quad));
  std::fprintf(f, "    \"irregular_interior_verts\": %d,\n",
               r.irregular_interior_verts);
  std::fprintf(f, "    \"interior_vert_count\": %d,\n", r.interior_vert_count);
  std::fprintf(f, "    \"regular_interior_frac\": %.9g,\n",
               r.regular_interior_frac);
  std::fprintf(f, "    \"component_count\": %d,\n", r.component_count);
  std::fprintf(f, "    \"boundary_loop_count\": %d,\n", r.boundary_loop_count);
  std::fprintf(f, "    \"max_component_irregular\": %d,\n",
               r.max_component_irregular);
  std::fprintf(f, "    \"max_adjacent_area_ratio\": %.9g,\n",
               r.max_adjacent_area_ratio);
  std::fprintf(f, "    \"max_adjacent_edge_ratio\": %.9g,\n",
               r.max_adjacent_edge_ratio);
  std::fprintf(f, "    \"min_interior_angle\": %.9g,\n", r.min_interior_angle);
  std::fprintf(f, "    \"min_angle_hist\": [");
  for (int i = 0; i < 9; i++)
    std::fprintf(f, "%s%d", i ? ", " : "", r.min_angle_hist[i]);
  std::fprintf(f, "],\n");
  std::fprintf(f, "    \"parametrization_folds\": %d,\n",
               r.parametrization_folds);
  std::fprintf(f, "    \"isolines_checked\": %s,\n", jb(r.isolines_checked));
  std::fprintf(f, "    \"spiral_isolines\": %d,\n", r.spiral_isolines);
  std::fprintf(f, "    \"open_isolines\": %d,\n", r.open_isolines);
  std::fprintf(f, "    \"closed_isolines\": %d\n", r.closed_isolines);
  std::fprintf(f, "  },\n");

  // Run report: per-stage status + solver stats (see remesh_report.h). The
  // pre-extraction fold count and min Jacobian come from the solve mesh, so they
  // live here (and are mirrored into validation.parametrization_folds).
  std::fprintf(f, "  \"run\": {\n");
  std::fprintf(f, "    \"success\": %s,\n", jb(rep.success));
  std::fprintf(f, "    \"failure_reason\": \"%s\",\n",
               jstr(rep.failure_reason).c_str());
  std::fprintf(f, "    \"pipeline_ms\": %lld,\n", rep.duration_ms);
  std::fprintf(f, "    \"num_singularities\": %d,\n", rep.num_singularities);
  std::fprintf(f, "    \"index_sum\": %d,\n", rep.index_sum);
  std::fprintf(f, "    \"field_solved_eigen\": %s,\n",
               jb(rep.field_solved_eigen));
  std::fprintf(f, "    \"field_close_pairs\": %d,\n", rep.field_close_pairs);
  std::fprintf(f, "    \"field_clutter_verts\": %d,\n", rep.field_clutter_verts);
  std::fprintf(f, "    \"cancel_attempted_pairs\": %d,\n",
               rep.cancel_attempted_pairs);
  std::fprintf(f, "    \"cancel_cancelled_pairs\": %d,\n",
               rep.cancel_cancelled_pairs);
  std::fprintf(f, "    \"cancel_reverted_rounds\": %d,\n",
               rep.cancel_reverted_rounds);
  std::fprintf(f, "    \"cancel_singularities_after\": %d,\n",
               rep.cancel_singularities_after);
  std::fprintf(f, "    \"parametrization_folds\": %d,\n",
               rep.parametrization_folds);
  std::fprintf(f, "    \"min_jacobian\": %.9g,\n", rep.min_jacobian);
  std::fprintf(f, "    \"quantize_feasible\": %s,\n", jb(rep.quantize_feasible));
  std::fprintf(f, "    \"derived_edge_length\": %.9g,\n", rep.derived_edge_length);
  std::fprintf(f, "    \"quad_count_actual\": %d,\n", rep.quad_count_actual);
  std::fprintf(f, "    \"components_total\": %d,\n", rep.components_total);
  std::fprintf(f, "    \"components_remeshed\": %d,\n", rep.components_remeshed);
  std::fprintf(f, "    \"components_failed\": %d,\n", rep.components_failed);
  // Quantize-stage profile (plans/miq.md Q0): rounding rounds, solver-primitive
  // counts, and per-phase wall-clocks. Timing lives here (results.json), never
  // in the corpus metrics.csv (deterministic columns only).
  const remesh::QuantizeStats &qz = rep.quantize_stats;
  std::fprintf(f, "    \"quantize\": {\n");
  std::fprintf(f, "      \"rounds\": %d,\n", qz.iters);
  std::fprintf(f, "      \"classes\": %d,\n", qz.num_classes);
  std::fprintf(f, "      \"cut_sides\": %d,\n", qz.num_cut_edges);
  std::fprintf(f, "      \"residual\": %.9g,\n", qz.max_integer_residual);
  std::fprintf(f, "      \"num_singularities\": %d,\n", qz.num_singularities);
  std::fprintf(f, "      \"spurious_pairs\": %d,\n", qz.spurious_pairs);
  std::fprintf(f, "      \"seamless_folds\": %d,\n", qz.seamless_folds);
  std::fprintf(f, "      \"seamless_folds_near_pairs\": %d,\n",
               qz.seamless_folds_near_pairs);
  std::fprintf(f, "      \"full_refactors\": %d,\n", qz.full_refactors);
  std::fprintf(f, "      \"updowns\": %d,\n", qz.updowns);
  std::fprintf(f, "      \"simp_refreshes\": %d,\n", qz.simp_refreshes);
  std::fprintf(f, "      \"back_solves\": %d,\n", qz.back_solves);
  std::fprintf(f, "      \"tier1b_probes\": %d,\n", qz.tier1b_probes);
  std::fprintf(f, "      \"gs_rounds\": %d,\n", qz.gs_rounds);
  std::fprintf(f, "      \"gs_converged\": %d,\n", qz.gs_converged);
  std::fprintf(f, "      \"gs_visits\": %d,\n", qz.gs_visits);
  std::fprintf(f, "      \"gs_touched_total\": %d,\n", qz.gs_touched_total);
  std::fprintf(f, "      \"gs_touched_max\": %d,\n", qz.gs_touched_max);
  std::fprintf(f, "      \"resort_full\": %d,\n", qz.resort_full);
  std::fprintf(f, "      \"resort_incr\": %d,\n", qz.resort_incr);
  std::fprintf(f, "      \"resort_keys\": %d,\n", qz.resort_keys);
  std::fprintf(f, "      \"total_ms\": %.9g,\n", qz.total_ms);
  std::fprintf(f, "      \"setup_ms\": %.9g,\n", qz.setup_ms);
  std::fprintf(f, "      \"initial_factor_ms\": %.9g,\n", qz.initial_factor_ms);
  std::fprintf(f, "      \"arap_ms\": %.9g,\n", qz.arap_ms);
  std::fprintf(f, "      \"rounding_ms\": %.9g,\n", qz.rounding_ms);
  std::fprintf(f, "      \"round_assemble_ms\": %.9g,\n", qz.round_assemble_ms);
  std::fprintf(f, "      \"round_refactor_ms\": %.9g,\n", qz.round_refactor_ms);
  std::fprintf(f, "      \"round_updown_ms\": %.9g,\n", qz.round_updown_ms);
  std::fprintf(f, "      \"round_backsolve_ms\": %.9g,\n", qz.round_backsolve_ms);
  std::fprintf(f, "      \"convert_ms\": %.9g,\n", qz.convert_ms);
  std::fprintf(f, "      \"gs_ms\": %.9g,\n", qz.gs_ms);
  std::fprintf(f, "      \"tier1b_ms\": %.9g,\n", qz.tier1b_ms);
  std::fprintf(f, "      \"stiffen_ms\": %.9g,\n", qz.stiffen_ms);
  std::fprintf(f, "      \"tier3_ms\": %.9g\n", qz.tier3_ms);
  std::fprintf(f, "    },\n");
  // Extraction hole accounting (Tier 6): why each residual output boundary rim
  // was capped or left open.
  const remesh::ExtractStats &ex = rep.extract_stats;
  std::fprintf(f, "    \"extract\": {\n");
  std::fprintf(f, "      \"grid_verts\": %d,\n", ex.num_grid_verts);
  std::fprintf(f, "      \"arcs\": %d,\n", ex.num_arcs);
  std::fprintf(f, "      \"open_arcs\": %d,\n", ex.open_arcs);
  std::fprintf(f, "      \"nonquad_cells\": %d,\n", ex.nonquad_cells);
  std::fprintf(f, "      \"holes_capped\": %d,\n", ex.holes_capped);
  std::fprintf(f, "      \"holes_capped_odd\": %d,\n", ex.holes_capped_odd);
  std::fprintf(f, "      \"odd_rims_paired\": %d,\n", ex.odd_rims_paired);
  std::fprintf(f, "      \"holes_pinched_split\": %d,\n", ex.holes_pinched_split);
  std::fprintf(f, "      \"holes_open\": %d,\n", ex.holes_open);
  std::fprintf(f, "      \"holes_open_border\": %d,\n", ex.holes_open_border);
  std::fprintf(f, "      \"holes_open_odd\": %d,\n", ex.holes_open_odd);
  std::fprintf(f, "      \"holes_open_size\": %d,\n", ex.holes_open_size);
  std::fprintf(f, "      \"holes_open_untraced\": %d\n", ex.holes_open_untraced);
  std::fprintf(f, "    },\n");
  std::fprintf(f, "    \"stages\": {\n");
  std::fprintf(f, "      \"copy\": \"%s\",\n", ssName(rep.copy));
  std::fprintf(f, "      \"triage\": \"%s\",\n", ssName(rep.triage));
  std::fprintf(f, "      \"decimate\": \"%s\",\n", ssName(rep.decimate));
  std::fprintf(f, "      \"pre_remesh\": \"%s\",\n", ssName(rep.pre_remesh));
  std::fprintf(f, "      \"cross_field\": \"%s\",\n", ssName(rep.cross_field));
  std::fprintf(f, "      \"singularity\": \"%s\",\n", ssName(rep.singularity));
  std::fprintf(f, "      \"quantize\": \"%s\",\n", ssName(rep.quantize));
  std::fprintf(f, "      \"extract\": \"%s\",\n", ssName(rep.extract));
  std::fprintf(f, "      \"reproject\": \"%s\"\n", ssName(rep.reproject));
  std::fprintf(f, "    }\n");
  std::fprintf(f, "  },\n");

  // Tier-1 input-triage counts (remesh/triage.h). `ran` is false when triage was
  // gated off; the count fields are then all zero.
  const remesh::TriageReport &tg = rep.triage_report;
  std::fprintf(f, "  \"triage\": {\n");
  std::fprintf(f, "    \"ran\": %s,\n", jb(tg.ran));
  std::fprintf(f, "    \"welded_verts\": %d,\n", tg.welded_verts);
  std::fprintf(f, "    \"removed_degenerate_faces\": %d,\n",
               tg.removed_degenerate_faces);
  std::fprintf(f, "    \"removed_duplicate_faces\": %d,\n",
               tg.removed_duplicate_faces);
  std::fprintf(f, "    \"removed_wire_edges\": %d,\n", tg.removed_wire_edges);
  std::fprintf(f, "    \"removed_components\": %d,\n", tg.removed_components);
  std::fprintf(f, "    \"removed_component_verts\": %d,\n",
               tg.removed_component_verts);
  std::fprintf(f, "    \"non_manifold_edges\": %d,\n", tg.non_manifold_edges);
  std::fprintf(f, "    \"non_manifold_verts\": %d,\n", tg.non_manifold_verts);
  std::fprintf(f, "    \"input_holes_filled\": %d,\n", tg.input_holes_filled);
  std::fprintf(f, "    \"input_holes_kept\": %d,\n", tg.input_holes_kept);
  std::fprintf(f, "    \"input_hole_fill_faces\": %d,\n", tg.input_hole_fill_faces);
  std::fprintf(f, "    \"thin_sampled_faces\": %d,\n", tg.thin_sampled_faces);
  std::fprintf(f, "    \"thin_paired_faces\": %d,\n", tg.thin_paired_faces);
  std::fprintf(f, "    \"thin_area_frac\": %.9g,\n", tg.thin_area_frac);
  std::fprintf(f, "    \"thin_thickness\": %.9g,\n", tg.thin_thickness);
  std::fprintf(f, "    \"thin_sheet\": %s\n", jb(tg.thin_sheet));
  std::fprintf(f, "  },\n");

  // Tier-9 pre-remesh A/B effect (remesh_report.h::PreRemeshEffect). `ran` is
  // false when the pre-pass was gated off; the fields are then all zero.
  const remesh::RemeshRunReport::PreRemeshEffect &pe = rep.pre_remesh_effect;
  std::fprintf(f, "  \"pre_remesh\": {\n");
  std::fprintf(f, "    \"ran\": %s,\n", jb(pe.ran));
  std::fprintf(f, "    \"verts_in\": %d,\n", pe.verts_in);
  std::fprintf(f, "    \"verts_out\": %d,\n", pe.verts_out);
  std::fprintf(f, "    \"faces_in\": %d,\n", pe.faces_in);
  std::fprintf(f, "    \"faces_out\": %d,\n", pe.faces_out);
  std::fprintf(f, "    \"mean_edge_in\": %.9g,\n", pe.mean_edge_in);
  std::fprintf(f, "    \"mean_edge_out\": %.9g,\n", pe.mean_edge_out);
  std::fprintf(f, "    \"edge_cv_in\": %.9g,\n", pe.edge_cv_in);
  std::fprintf(f, "    \"fold90_in\": %d,\n", pe.fold90_in);
  std::fprintf(f, "    \"fold90_out\": %d,\n", pe.fold90_out);
  std::fprintf(f, "    \"fold180_in\": %d,\n", pe.fold180_in);
  std::fprintf(f, "    \"fold180_out\": %d,\n", pe.fold180_out);
  std::fprintf(f, "    \"degen_in\": %d,\n", pe.degen_in);
  std::fprintf(f, "    \"degen_out\": %d,\n", pe.degen_out);
  std::fprintf(f, "    \"iters_run\": %d,\n", pe.iters_run);
  std::fprintf(f, "    \"converged\": %s,\n", jb(pe.converged));
  std::fprintf(f, "    \"coarsen_bootstrap\": %s,\n", jb(pe.coarsen_bootstrap));
  std::fprintf(f, "    \"target_resolved\": %.9g,\n", pe.target_resolved);
  std::fprintf(f, "    \"iters_resolved\": %d,\n", pe.iters_resolved);
  std::fprintf(f, "    \"bootstrap_resolved\": %d,\n", pe.bootstrap_resolved);
  std::fprintf(f, "    \"duration_ms\": %lld\n", pe.duration_ms);
  std::fprintf(f, "  }\n");
  std::fprintf(f, "}\n");

  std::fclose(f);
  return true;
}

void usage()
{
  std::printf(
      "remesh_cli --input <obj> [options]\n"
      "  --input <path>          input OBJ (bare name resolves against assets dir)\n"
      "  --outdir <dir>          output dir (default: tests/remesher-results)\n"
      "  --name <base>           output basename (default: input stem)\n"
      "  --target-quads <int>    target quad count (default 15000; per-asset\n"
      "                          quad-counts.txt overrides when neither this\n"
      "                          nor --target is given)\n"
      "  --target <float>        explicit quad edge length; overrides\n"
      "                          --target-quads (default 0 = derive from count)\n"
      "  --solve <float>         solve-mesh edge length, 0=off (default 0)\n"
      "  --curvature <0|1>       align field to curvature (default 1)\n"
      "  --sharp <0|1>           pin field to sharp edges/boundaries (default 1)\n"
      "  --sharp-angle <float>   sharp dihedral threshold, radians (default 0.785)\n"
      "  --density <0|1>         use per-vertex density map (default 0)\n"
      "  --quant-direct <0|1>    one-shot DIRECT rounding, no greedy rounds "
      "(default 0)\n"
      "  --reproject <0|1>       snap output onto input surface (default 1)\n"
      "  --cap-odd <0|1>         close odd holes too, paired all-quad (default 0)\n"
      "  --smooth <int>          reprojection smoothing iterations (default 2)\n"
      "  --smooth-strength <f>   per-iteration smoothing step 0..1 (default 0.5)\n"
      "  --seed <uint>           determinism seed (default 1)\n"
      "  --triage <0|1>          run input triage before solve (default 1)\n"
      "  --triage-weld-rel <f>   weld tol as frac of bbox diag (default 1e-5)\n"
      "  --triage-min-component-frac <f>  drop components below frac of verts "
      "(default 0)\n"
      "  --hole-fill <f>         fill input holes with rim < frac of total "
      "boundary (default 0)\n"
      "  --per-component <0|1>   remesh disconnected components independently "
      "(default 0)\n"
      "  --curvature-smooth-iters <int>   tensor-field Jacobi sweeps (default 0)\n"
      "  --curvature-smooth-lambda <f>    per-sweep blend 0..1 (default 0.5)\n"
      "  --field-smoothness <f>   cross-field smoothness weight (default 1)\n"
      "  --curvature-weight <f>   soft curvature-alignment scale (default 1)\n"
      "  --singularity-cancel <0|1>  cancel sub-resolution pole pairs (default 0)\n"
      "  --singularity-cancel-max-sep <f>  pair gate, quad-edge units "
      "(default 1.5)\n"
      "  --auto-density <0|1>     curvature-driven sizing field (default 0)\n"
      "  --density-min <f>        density clamp floor (default 0.25)\n"
      "  --density-max <f>        density clamp ceiling (default 4)\n"
      "  --density-gradation <f>  bound size growth rate; 0=off (default 0.5)\n"
      "  --density-gradation-iters <int>  limiter sweep cap (default 10)\n"
      "  --pre-remesh <0|1>       field-aligned input pre-remesh (default 0)\n"
      "  --pre-remesh-target <f>  pre-pass edge length; 0=auto (default 0)\n"
      "  --pre-remesh-iters <int> outer iterations; 0=auto (default 0)\n"
      "  --pre-remesh-density <0|1>  curvature size field drives BK band "
      "(default 1)\n"
      "  --pre-remesh-gradation <f>  size-field growth cap; 0=off (default 0.5)\n"
      "  --pre-remesh-gradation-iters <int>  limiter sweep cap (default 10)\n"
      "  --pre-remesh-align <f>   smooth blend isotropic 0..1 aligned (default 1)\n"
      "  --pre-remesh-field-cadence <int>  field refresh every N iters "
      "(default 2)\n"
      "  --pre-remesh-bootstrap-iters <int>  isotropic denoise sweeps; -1=auto "
      "(default -1)\n"
      "  --pre-remesh-smooth-iters <int>  smooth sweeps per outer iter "
      "(default 5)\n"
      "  --pre-remesh-smooth-lambda <f>   smooth relaxation 0..1 (default 0.5)\n"
      "  --pre-remesh-converge-eps <f>    early-out threshold; 0=off "
      "(default 0.05)\n"
      "  --pre-remesh-preserve-features <0|1>  pin boundaries+creases "
      "(default 1)\n"
      "  --pre-remesh-sharp-angle <f>     crease dihedral, radians "
      "(default 0.785)\n"
      "  --pre-remesh-trace <0|1>  print per-iter convergence summary to "
      "stderr (default 0)\n");
}

bool toBool(const char *s) { return std::atoi(s) != 0; }

} // namespace

int main(int argc, char **argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0); // parent reads progress live

  std::string input, outdir = REMESH_CLI_RESULTS_DIR, name;
  remesh::RemeshParams params;
  bool sizing_given = false; // --target or --target-quads on the command line

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&](const char *flag) -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "ERROR missing value for %s\n", flag);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else if (a == "--input")
      input = next("--input");
    else if (a == "--outdir")
      outdir = next("--outdir");
    else if (a == "--name")
      name = next("--name");
    else if (a == "--target") {
      params.target_edge_length = float(std::atof(next("--target")));
      sizing_given = true;
    } else if (a == "--target-quads") {
      params.target_quad_count = std::atoi(next("--target-quads"));
      sizing_given = true;
    } else if (a == "--solve")
      params.solve_edge_length = float(std::atof(next("--solve")));
    else if (a == "--curvature")
      params.use_curvature = toBool(next("--curvature"));
    else if (a == "--sharp")
      params.use_sharp_features = toBool(next("--sharp"));
    else if (a == "--sharp-angle")
      params.sharp_angle = float(std::atof(next("--sharp-angle")));
    else if (a == "--density")
      params.use_density = toBool(next("--density"));
    else if (a == "--quant-direct")
      params.quantize_direct_rounding = toBool(next("--quant-direct"));
    else if (a == "--reproject")
      params.reproject = toBool(next("--reproject"));
    else if (a == "--cap-odd")
      params.cap_odd_holes = toBool(next("--cap-odd"));
    else if (a == "--smooth")
      params.smooth_iterations = std::atoi(next("--smooth"));
    else if (a == "--smooth-strength")
      params.smooth_strength = float(std::atof(next("--smooth-strength")));
    else if (a == "--seed")
      params.seed = uint32_t(std::strtoul(next("--seed"), nullptr, 10));
    else if (a == "--triage")
      params.triage = toBool(next("--triage"));
    else if (a == "--triage-weld-rel")
      params.triage_weld_rel = float(std::atof(next("--triage-weld-rel")));
    else if (a == "--triage-min-component-frac")
      params.triage_min_component_frac =
          float(std::atof(next("--triage-min-component-frac")));
    else if (a == "--hole-fill")
      params.input_hole_fill_max_frac = float(std::atof(next("--hole-fill")));
    else if (a == "--per-component")
      params.per_component = toBool(next("--per-component"));
    else if (a == "--curvature-smooth-iters")
      params.curvature_smooth_iters = std::atoi(next("--curvature-smooth-iters"));
    else if (a == "--curvature-smooth-lambda")
      params.curvature_smooth_lambda =
          float(std::atof(next("--curvature-smooth-lambda")));
    else if (a == "--field-smoothness")
      params.field_smoothness = float(std::atof(next("--field-smoothness")));
    else if (a == "--curvature-weight")
      params.curvature_weight = float(std::atof(next("--curvature-weight")));
    else if (a == "--singularity-cancel")
      params.singularity_cancel = toBool(next("--singularity-cancel"));
    else if (a == "--singularity-cancel-max-sep")
      params.singularity_cancel_max_sep =
          float(std::atof(next("--singularity-cancel-max-sep")));
    else if (a == "--auto-density")
      params.auto_density = toBool(next("--auto-density"));
    else if (a == "--density-min")
      params.density_min = float(std::atof(next("--density-min")));
    else if (a == "--density-max")
      params.density_max = float(std::atof(next("--density-max")));
    else if (a == "--density-gradation")
      params.density_gradation = float(std::atof(next("--density-gradation")));
    else if (a == "--density-gradation-iters")
      params.density_gradation_iters =
          std::atoi(next("--density-gradation-iters"));
    else if (a == "--pre-remesh")
      params.pre_remesh = toBool(next("--pre-remesh"));
    else if (a == "--pre-remesh-target")
      params.pre_remesh_target = float(std::atof(next("--pre-remesh-target")));
    else if (a == "--pre-remesh-iters")
      params.pre_remesh_iters = std::atoi(next("--pre-remesh-iters"));
    else if (a == "--pre-remesh-density")
      params.pre_remesh_density = toBool(next("--pre-remesh-density"));
    else if (a == "--pre-remesh-gradation")
      params.pre_remesh_gradation =
          float(std::atof(next("--pre-remesh-gradation")));
    else if (a == "--pre-remesh-gradation-iters")
      params.pre_remesh_gradation_iters =
          std::atoi(next("--pre-remesh-gradation-iters"));
    else if (a == "--pre-remesh-align")
      params.pre_remesh_align = float(std::atof(next("--pre-remesh-align")));
    else if (a == "--pre-remesh-field-cadence")
      params.pre_remesh_field_cadence =
          std::atoi(next("--pre-remesh-field-cadence"));
    else if (a == "--pre-remesh-bootstrap-iters")
      params.pre_remesh_bootstrap_iters =
          std::atoi(next("--pre-remesh-bootstrap-iters"));
    else if (a == "--pre-remesh-smooth-iters")
      params.pre_remesh_smooth_iters =
          std::atoi(next("--pre-remesh-smooth-iters"));
    else if (a == "--pre-remesh-smooth-lambda")
      params.pre_remesh_smooth_lambda =
          float(std::atof(next("--pre-remesh-smooth-lambda")));
    else if (a == "--pre-remesh-converge-eps")
      params.pre_remesh_converge_eps =
          float(std::atof(next("--pre-remesh-converge-eps")));
    else if (a == "--pre-remesh-preserve-features")
      params.pre_remesh_preserve_features =
          toBool(next("--pre-remesh-preserve-features"));
    else if (a == "--pre-remesh-sharp-angle")
      params.pre_remesh_sharp_angle =
          float(std::atof(next("--pre-remesh-sharp-angle")));
    else if (a == "--pre-remesh-trace")
      params.pre_remesh_trace = toBool(next("--pre-remesh-trace"));
    else {
      std::fprintf(stderr, "ERROR unknown arg %s\n", a.c_str());
      return 2;
    }
  }

  if (input.empty()) {
    std::printf("ERROR no --input given\n");
    usage();
    return 2;
  }

  // Bare filename resolves against the assets dir.
  std::string inPath = input;
  if (!fs::exists(inPath)) {
    std::string alt = std::string(REMESH_CLI_ASSETS_DIR) + "/" + input;
    if (fs::exists(alt))
      inPath = alt;
  }
  if (name.empty())
    name = stem(inPath);

  // No explicit sizing → the asset's recorded quad count (quad-counts.txt in
  // the assets dir) overrides the default target_quad_count.
  if (!sizing_given) {
    int rec = remesh::cli::lookupAssetQuadCount(REMESH_CLI_ASSETS_DIR,
                                                stem(inPath));
    if (rec > 0)
      params.target_quad_count = rec;
  }

  // Load with quads preserved so the manifest records the asset's real face
  // count; QuadRemesh triangulates its own internal copy anyway.
  mesh::Mesh *in = mesh::loadObj(inPath.c_str(), /*keepNgons=*/true);
  if (!in) {
    std::printf("ERROR could not open input %s\n", inPath.c_str());
    return 1;
  }
  int inVerts = in->v.count, inFaces = in->f.count;
  float3 amin, amax;
  inputAABB(*in, amin, amax);

  // Validate the input too (component + hole counts for the input/output
  // comparison in the manifest). Cheap, and QuadRemesh leaves `in` intact —
  // run it before the pipeline, which thaws/triangulates its own copy.
  mesh::RemeshReport rin = mesh::remeshValidate(*in);

  remesh::RemeshRunReport rep;
  auto t0 = std::chrono::steady_clock::now();
  mesh::Mesh *out = remesh::QuadRemesh(*in, params, progressCb, nullptr, &rep);
  auto t1 = std::chrono::steady_clock::now();
  long long durationMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

  std::string ts = timestamp();
  std::string base = name + "_" + ts;
  std::error_code ec;
  fs::create_directories(outdir, ec);
  // generic_string() → forward slashes, so the emitted paths are valid in JSON
  // and uniform for the parent process regardless of platform separator.
  std::string objPath = (fs::path(outdir) / (base + ".obj")).generic_string();
  std::string jsonPath = (fs::path(outdir) / (base + ".json")).generic_string();

  // Clean failure (e.g. no integer lattice): no output mesh, but still emit a
  // manifest with an empty validation block + populated run block so the corpus
  // runner records the run instead of losing it, then exit non-zero.
  if (!out) {
    mesh::RemeshReport r;
    if (writeManifest(jsonPath.c_str(), ts, objPath, name, inPath, inVerts,
                      inFaces, amin, amax, params, r, rin, rep, durationMs))
      std::printf("MANIFEST %s\n", jsonPath.c_str());
    std::printf("ERROR QuadRemesh produced no mesh reason=%s\n",
                rep.failure_reason.empty() ? "unknown"
                                           : rep.failure_reason.c_str());
    litestl::alloc::Delete<mesh::Mesh>(in);
    return 1;
  }

  // Reuse the validation QuadRemesh already ran on the report path (it carries
  // the pre-extraction fold count copied in from quantize) rather than a second
  // remeshValidate pass.
  mesh::RemeshReport r = rep.validation;

  if (!mesh::writeObj(*out, objPath.c_str()))
    std::printf("ERROR could not write %s\n", objPath.c_str());
  else
    std::printf("RESULT %s\n", objPath.c_str());

  if (writeManifest(jsonPath.c_str(), ts, objPath, name, inPath, inVerts,
                    inFaces, amin, amax, params, r, rin, rep, durationMs))
    std::printf("MANIFEST %s\n", jsonPath.c_str());
  else
    std::printf("ERROR could not write %s\n", jsonPath.c_str());

  const remesh::TriageReport &tg = rep.triage_report;
  std::printf("STATS verts=%d edges=%d faces=%d quads=%d tris=%d ngons=%d "
              "allquad=%d manifold=%d euler=%d inverted=%d boundary=%d "
              "spiral=%d irr=%d regular_frac=%.4g components=%d holes=%d "
              "folds=%d min_angle=%.4g area_ratio=%.4g "
              "triage=%d triage_welded=%d triage_degenerate=%d "
              "triage_components=%d triage_nonmanifold_edges=%d "
              "holes_filled=%d comp_runs=%d/%d thin_frac=%.3g thin_sheet=%d "
              "derived_edge=%.4g duration_ms=%lld\n",
              r.vert_count, r.edge_count, r.face_count, r.quad_count,
              r.tri_count, r.ngon_count, int(r.all_quad), int(r.manifold),
              r.euler, r.inverted_faces, r.boundary_edges, r.spiral_isolines,
              r.irregular_interior_verts, r.regular_interior_frac,
              r.component_count, r.boundary_loop_count, r.parametrization_folds,
              r.min_interior_angle, r.max_adjacent_area_ratio, int(tg.ran),
              tg.welded_verts, tg.removed_degenerate_faces, tg.removed_components,
              tg.non_manifold_edges, tg.input_holes_filled,
              rep.components_remeshed, rep.components_total, tg.thin_area_frac,
              int(tg.thin_sheet), rep.derived_edge_length, durationMs);

  // Pre-remesh A/B one-liner (only when the pre-pass ran); full record is in
  // the manifest "pre_remesh" block.
  const remesh::RemeshRunReport::PreRemeshEffect &pe = rep.pre_remesh_effect;
  if (pe.ran) {
    std::printf("PRESTATS verts=%d/%d faces=%d/%d mean_edge=%.4g/%.4g "
                "edge_cv_in=%.4g fold90=%d/%d fold180=%d/%d degen=%d/%d "
                "iters=%d/%d converged=%d coarsen=%d target=%.4g bootstrap=%d "
                "duration_ms=%lld\n",
                pe.verts_in, pe.verts_out, pe.faces_in, pe.faces_out,
                pe.mean_edge_in, pe.mean_edge_out, pe.edge_cv_in, pe.fold90_in,
                pe.fold90_out, pe.fold180_in, pe.fold180_out, pe.degen_in,
                pe.degen_out, pe.iters_run, pe.iters_resolved,
                int(pe.converged), int(pe.coarsen_bootstrap),
                pe.target_resolved, pe.bootstrap_resolved, pe.duration_ms);
  }

  litestl::alloc::Delete<mesh::Mesh>(out);
  litestl::alloc::Delete<mesh::Mesh>(in);
  return 0;
}
