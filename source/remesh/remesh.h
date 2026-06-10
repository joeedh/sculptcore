#pragma once

/* Feature-aligned quad remesher (global, one-shot).
 *
 * QuadRemesh(input, params) runs the MIQ-style pipeline — cross-field (M2) →
 * singularity adjustment (M3) → seamless parametrization (M4) → integer
 * quantization (M5) → quad extraction + reprojection (M6) — and returns a
 * freshly-allocated all-quad mesh. The input is left untouched so the host can
 * keep it for undo (it owns the result and frees it via the existing freeMesh).
 *
 * This is a *global* operation, not a per-dab brush: borrow dyntopo's module
 * packaging, never its local/incremental use-case. See
 * documentation/plans/quad-remeshing.md.
 *
 * Kept free of the binding/Eigen headers; the solver-heavy stages PIMPL Eigen
 * away in their own .cc files (Risks: compile-time cost). */

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::remesh {

struct RemeshParams;
struct RemeshRunReport;

/* Optional coarse-stage progress hook. @p pct is 0..100 (monotonic across the
 * synchronous pipeline stages), @p stage a stable lowercase tag ("copy",
 * "cross_field", "extract", "done", "failed", …). Kept out of RemeshParams (a
 * reflected/bound struct) so the binding system never sees a function pointer;
 * the standalone CLI passes a stdout-streaming callback, the app/N-API path
 * passes none. */
using RemeshProgressFn = void (*)(void *user, int pct, const char *stage);

/* Produce a new all-quad mesh from @p input. Returns a heap-allocated Mesh
 * (caller owns; free via mesh::Mesh's allocator / the freeMesh C-API), or
 * nullptr on a clean failure (e.g. Gauss-Bonnet-infeasible pole pins — the
 * error is surfaced rather than producing a garbage field). @p progress (if
 * non-null) is invoked at each coarse stage boundary. @p report (if non-null)
 * is filled with per-stage status, solver stats, the pre-extraction fold count,
 * a duration + failure reason, and (on success) the output validation block —
 * see remesh_report.h; passing it adds one remeshValidate pass on success. */
mesh::Mesh *QuadRemesh(mesh::Mesh &input, const RemeshParams &params,
                       RemeshProgressFn progress = nullptr, void *user = nullptr,
                       RemeshRunReport *report = nullptr);

/* The operative quad edge length for @p params on @p m: the explicit
 * target_edge_length when > 0, else derived from target_quad_count as
 * L = sqrt(integral of density dA / N) (density-weighted only when params
 * consume a density field; d = 1 otherwise). The pipeline refines this further
 * in count mode (auto-density fixed point, corrective re-quantize); this is
 * the estimate UIs / standalone drivers should mirror. */
float resolveTargetEdgeLength(mesh::Mesh &m, const RemeshParams &params);

/* What the pre_remesh_target = 0 sentinel resolves to on @p m: explicit >
 * solve_edge_length > (count mode) 0.7x the resolved quad edge floored at half
 * the median input edge > explicit target_edge_length. */
float resolvePreRemeshTarget(mesh::Mesh &m, const RemeshParams &params);

} // namespace sculptcore::remesh
