#pragma once

/* Tier-0 run report for QuadRemesh — an optional out-param carrying per-stage
 * status, the field/quantize solver stats, the pre-extraction parametrization
 * fold count, the output validation block, the wall-clock duration, and a
 * failure reason. C++-only: it never crosses the WASM/N-API seam (the C entry
 * Mesh_quadRemesh still returns only Mesh*). Tier-8's in-process retry loop and
 * the CLI/manifest read it; the host gets at most a compact summary via a future
 * query, not this struct. See documentation/plans/quad-remeshing-filtering.md. */

#include "mesh/utils/mesh_validate.h"

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
  StageStatus decimate = StageStatus::Skipped;
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

  // Quantization stats (M5). parametrization_folds = pre-extraction folded faces
  // on the solve mesh's (u,v); quantize_feasible = the integer-grid map was
  // reached (no spirals). min_jacobian <= 0 ⇒ folds remain.
  int parametrization_folds = 0;
  double min_jacobian = 0.0;
  bool quantize_feasible = false;

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
};

} // namespace sculptcore::remesh
