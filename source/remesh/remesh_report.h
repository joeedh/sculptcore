#pragma once

/* Tier-0 run report for QuadRemesh — an optional out-param carrying per-stage
 * status, the field/quantize solver stats, the pre-extraction parametrization
 * fold count, the output validation block, the wall-clock duration, and a
 * failure reason. C++-only: it never crosses the WASM/N-API seam (the C entry
 * Mesh_quadRemesh still returns only Mesh*). Tier-8's in-process retry loop and
 * the CLI/manifest read it; the host gets at most a compact summary via a future
 * query, not this struct. See documentation/plans/quad-remeshing-filtering.md. */

#include "mesh/utils/mesh_validate.h"
#include "remesh/extract/quad_extract.h"
#include "remesh/quantize/quantize_ilp.h"
#include "remesh/remesh_params.h"
#include "remesh/triage.h"

#include <string>

namespace sculptcore::remesh {

enum class StageStatus : unsigned char {
  Skipped = 0, // stage did not run (gated off)
  Ok = 1,      // stage ran and succeeded
  Failed = 2,  // stage ran and failed (pipeline aborted)
};

struct RemeshRunReport {
  // Per-stage execution status, in pipeline order.
  StageStatus copy = StageStatus::Skipped;
  StageStatus triage = StageStatus::Skipped;
  StageStatus pre_remesh = StageStatus::Skipped;
  StageStatus cross_field = StageStatus::Skipped;
  StageStatus singularity = StageStatus::Skipped;
  StageStatus quantize = StageStatus::Skipped;
  StageStatus extract = StageStatus::Skipped;
  StageStatus reproject = StageStatus::Skipped;

  // Cross-field solver stats (M2). index_sum == 4χ; field_solved_eigen = the
  // solve fell back to the smoothest eigenvector.
  int num_singularities = 0;
  int index_sum = 0;
  bool field_solved_eigen = false;

  // Singularity clutter (Tier-5 gate diagnostic): opposite-index pole pairs
  // within 2 vertex hops — what a pair-cancellation pass could annihilate —
  // and the number of poles participating in at least one such pair.
  int field_close_pairs = 0;
  int field_clutter_verts = 0;

  // Tier-5 pair cancellation (singularity_cancel): pairs flipped / annihilated /
  // rounds rolled back, and the post-cancel pole count (num_singularities above
  // keeps the M2 count so the before/after delta is readable from one report).
  int cancel_attempted_pairs = 0;
  int cancel_cancelled_pairs = 0;
  int cancel_reverted_rounds = 0;
  int cancel_singularities_after = 0;

  // Quantization stats (M5). parametrization_folds = pre-extraction folded faces
  // on the solve mesh's (u,v); quantize_feasible = the integer-grid map was
  // reached (no spirals). min_jacobian <= 0 ⇒ folds remain.
  int parametrization_folds = 0;
  double min_jacobian = 0.0;
  bool quantize_feasible = false;

  // Solve-mesh face count at field time (post pre-remesh) — the
  // fold-fraction denominator for the Tier-8 retry rules.
  int solve_faces = 0;

  // Full quantize-stage profile (plans/miq.md Q0): rounds, solver-primitive
  // counts, and per-phase wall-clocks. Surfaced via the manifest "run" block.
  QuantizeStats quantize_stats;

  // Extraction stats (M6), incl. the cap-path hole accounting (Tier 6) — why
  // each residual output boundary rim was capped or left open.
  ExtractStats extract_stats;

  // Count-mode sizing (target_edge_length == 0): the edge length derived from
  // target_quad_count and the extracted face count. derived_edge_length is also
  // filled in explicit mode (= target_edge_length) so manifests always record
  // the operative scale.
  float derived_edge_length = 0.0f;
  int quad_count_actual = 0;

  // Tier 6.4 per-component runs (per_component): face-bearing pieces seen by
  // the splitter (0 = splitter didn't run — single component or gated off);
  // failed sub-runs are dropped from the merged output and counted here.
  int components_total = 0;
  int components_remeshed = 0;
  int components_failed = 0;

  // Outcome + timing. failure_reason is a stable lowercase tag on failure
  // ("extract_no_lattice", ...), empty on success. duration_ms is the QuadRemesh
  // pipeline wall-clock (excludes the optional output validation below).
  bool success = false;
  std::string failure_reason;
  long long duration_ms = 0;

  // Output-mesh validation (mesh::remeshValidate). Filled only when a report was
  // requested AND extraction produced a mesh; parametrization_folds is copied in
  // (remeshValidate cannot derive it — the (u,v) lives on the solve mesh). Reads
  // are gated on validation_filled.
  bool validation_filled = false;
  mesh::RemeshReport validation;

  // Tier-1 input triage counts (weld / degenerate-face / tiny-component drops +
  // detect-only non-manifold). triage_report.ran reflects whether triage ran.
  TriageReport triage_report;

  // Tier-9 pre-remesh effect: the working mesh before vs after the pre-pass
  // (geometry census, edge statistics, 0d fold metrics), the driver's run
  // outcome, and what the auto-sentinel knobs resolved to — the A/B record for
  // "how did the pre-pass change what the field solve saw". The "_in" side is
  // measured after triage, i.e. exactly what the pre-pass received.
  struct PreRemeshEffect {
    bool ran = false;
    int verts_in = 0, faces_in = 0;
    int verts_out = 0, faces_out = 0;
    float mean_edge_in = 0.0f, mean_edge_out = 0.0f;
    float edge_cv_in = 0.0f; // edge-length coeff. of variation (irregularity)
    int fold90_in = 0, fold180_in = 0, degen_in = 0;
    int fold90_out = 0, fold180_out = 0, degen_out = 0;
    int iters_run = 0;
    bool converged = false;         // converge_eps early-out fired
    bool coarsen_bootstrap = false; // dense input BK-coarsened before the loop
    float target_resolved = 0.0f;   // what the pre_remesh_target sentinel became
    int iters_resolved = 0;
    int bootstrap_resolved = 0;
    long long duration_ms = 0;
  };
  PreRemeshEffect pre_remesh_effect;

  // Tier 8a: per-attempt retry trail. Filled only when auto_retry engaged the
  // loop (attempts_run > 0); attempts[winner] produced the returned mesh, and
  // its full report is this struct's top-level fields. from_original is always
  // true today — every attempt restarts from the caller's untouched input.
  struct RetryAttempt {
    RemeshParams params;                // the exact knob vector this attempt ran
    const char *escalation = "initial"; // stable tag: which rung set the knobs
    bool from_original = true;
    bool success = false;
    std::string failure_reason; // empty on success
    int parametrization_folds = 0;
    int num_singularities = 0; // post-cancel when singularity_cancel ran
    int inverted_faces = 0;    // from output validation (0 when no output)
    int odd_residuals = 0;     // open odd rims + unpaired fan caps
    float max_adjacent_edge_ratio = 0.0f;
    long long duration_ms = 0;
  };
  static constexpr int MAX_RETRY_ATTEMPTS = 8;
  RetryAttempt attempts[MAX_RETRY_ATTEMPTS];
  int attempts_run = 0;
  int winner = -1;
};

} // namespace sculptcore::remesh
