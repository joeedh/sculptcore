#include "../roughness.h"
#include "../scene.h"
#include "../state_dump.h"

#include "brush/brush_executor.h"
#include "brush/brushes/all.h"
#include "brush/grid_executor.h"
#include "brush/grid_gpu_session.h"
#include "brush/stroke_driver.h"
#include "brush/stroke_spacing.h"
#include "displace/compositor.h"
#include "displace/frames.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"
#include "mesh/attr_weights.h"
#include "mesh/attribute_builtin.h"
#include "mesh/mesh_serialize.h"
#include "mesh/mesh_shapes.h"
#include "mesh/utils/closest_point.h"
#include "mesh/utils/mesh_validate.h"
#include "mesh/utils/triangulate.h"
#include "remesh/field/cross_field.h"
#include "remesh/field/curvature.h"
#include "remesh/field/feature_tag.h"
#include "remesh/field/singularity_adjust.h"
#include "remesh/param/seamless_param.h"
#include "remesh/quantize/quantize_ilp.h"
#include "remesh/remesh.h"
#include "remesh/remesh_params.h"
#include "spatial/spatial.h"
#include "stb/stb_image.h"
#include "vdm/vdm_promote.h"
#include "vdm/vdm_splat.h"
#include "vdm/vdm_undo.h"
#ifdef SBRUSH_WEBGPU_COMPUTE
#include "webgpu/wgpu_compute.h"
#include "webgpu/wgpu_context.h"
#endif
#ifdef SBRUSH_GPU_DISPATCH
#include "vulkan/vk_compute.h"
#include "vulkan/vk_context.h"
#endif
#include "subdiv/grid_domain.h"
#include "subdiv/grid_stroke_log.h"
#include "subdiv/grid_tree.h"
#include "subdiv/grids.h"
#include "subdiv/multires.h"

#ifdef SBRUSH_GPU_DISPATCH
#include "../gpu_stroke.h"
#endif

#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include "script_util.h"

namespace sculptcore::debug_app::script {

using litestl::math::float3;
using litestl::util::Vector;

bool execRemeshVerb(Scene &scene,
                    const std::string &verb,
                    ArgMap &args,
                    const char *out_dir,
                    std::string &err,
                    bool &handled)
{
  handled = true;

  if (verb == "save_mesh") {
    /* save_mesh path=FILE — serialize scene.mesh (serial::writeMesh blob). */
    if (!scene.mesh) {
      err = "save_mesh: no mesh";
      return false;
    }
    std::string path = getArg(args, "path", "");
    if (path.empty()) {
      err = "save_mesh: missing path=";
      return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out || !mesh::serial::writeMesh(*scene.mesh, out)) {
      err = "save_mesh: write failed: " + path;
      return false;
    }
    std::printf("[save_mesh] %s verts=%d edges=%d faces=%d\n",
                path.c_str(),
                scene.mesh->v.count,
                scene.mesh->e.count,
                scene.mesh->f.count);
    return true;
  }
  if (verb == "load_mesh") {
    /* load_mesh path=FILE — replace scene.mesh with a serial::readMesh blob
     * (running any format migrations), then validateAndRepair. */
    std::string path = getArg(args, "path", "");
    if (path.empty()) {
      err = "load_mesh: missing path=";
      return false;
    }
    std::ifstream in(path, std::ios::binary);
    mesh::Mesh *nm = litestl::alloc::New<mesh::Mesh>("Mesh load_mesh");
    if (!in || !mesh::serial::readMesh(*nm, in)) {
      litestl::alloc::Delete(nm);
      err = "load_mesh: read failed: " + path;
      return false;
    }
    int problems = nm->validateAndRepair();
    scene.setMesh(nm);
    std::printf("[load_mesh] %s verts=%d edges=%d faces=%d problems=%d\n",
                path.c_str(),
                nm->v.count,
                nm->e.count,
                nm->f.count,
                problems);
    return true;
  }
  if (verb == "remesh") {
    /* remesh [target=..] [target_quads=N] [curvature=1] [sharp=1]
     *        [sharp_angle=..] [smoothness=..] [curvature_weight=..]
     *        [curvature_smooth_iters=..] [curvature_smooth_lambda=..]
     *        [density=0] [reproject=1] [smooth=N] [smooth_strength=..] [seed=N]
     * Replaces scene.mesh with the feature-aligned quad remesh of it. */
    if (!scene.mesh) {
      err = "remesh: no mesh";
      return false;
    }
    scene.mesh->thawTopo();
    remesh::RemeshParams params;
    params.target_edge_length = getFloat(args, "target", params.target_edge_length);
    params.target_quad_count = getInt(args, "target_quads", params.target_quad_count);
    params.use_curvature = getBool(args, "curvature", params.use_curvature);
    params.use_sharp_features = getBool(args, "sharp", params.use_sharp_features);
    params.sharp_angle = getFloat(args, "sharp_angle", params.sharp_angle);
    params.field_smoothness = getFloat(args, "smoothness", params.field_smoothness);
    params.curvature_weight = getFloat(args, "curvature_weight", params.curvature_weight);
    params.curvature_smooth_iters =
        getInt(args, "curvature_smooth_iters", params.curvature_smooth_iters);
    params.curvature_smooth_lambda =
        getFloat(args, "curvature_smooth_lambda", params.curvature_smooth_lambda);
    params.use_density = getBool(args, "density", params.use_density);
    params.reproject = getBool(args, "reproject", params.reproject);
    params.smooth_iterations = getInt(args, "smooth", params.smooth_iterations);
    params.smooth_strength = getFloat(args, "smooth_strength", params.smooth_strength);
    params.seed = (uint32_t)getInt(args, "seed", (int)params.seed);
    mesh::Mesh *out = remesh::QuadRemesh(*scene.mesh, params);
    if (!out) {
      err = "remesh: QuadRemesh returned null";
      return false;
    }
    scene.setMesh(out);
    return true;
  }
  if (verb == "remesh_validate") {
    /* Print a structural report; with assert=1 fail on any structural problem,
     * and with all_quad=1 additionally require a pure-quad mesh. */
    if (!scene.mesh) {
      err = "remesh_validate: no mesh";
      return false;
    }
    scene.mesh->thawTopo();
    mesh::RemeshReport rep = mesh::remeshValidate(*scene.mesh);
    std::printf("[remesh_validate] V=%d E=%d F=%d euler=%d | tris=%d quads=%d "
                "ngons=%d all_quad=%d | manifold=%d winding=%d nonmanifold_e=%d "
                "boundary_e=%d degenerate_f=%d inverted_f=%d | irregular_v=%d\n",
                rep.vert_count,
                rep.edge_count,
                rep.face_count,
                rep.euler,
                rep.tri_count,
                rep.quad_count,
                rep.ngon_count,
                rep.all_quad,
                rep.manifold,
                rep.consistent_winding,
                rep.non_manifold_edges,
                rep.boundary_edges,
                rep.degenerate_faces,
                rep.inverted_faces,
                rep.irregular_interior_verts);
    if (!rep.manifold) {
      std::printf("[remesh_validate]   topology error: %s\n", rep.manifold_error.c_str());
    }
    std::fflush(stdout);
    if (getBool(args, "assert", false) && !rep.structurallyOk()) {
      err = "remesh_validate: structural check failed";
      if (!rep.manifold)
        err += " (" + rep.manifold_error + ")";
      return false;
    }
    if (getBool(args, "all_quad", false) && !rep.all_quad) {
      err = "remesh_validate: mesh is not all-quad";
      return false;
    }
    return true;
  }
  if (verb == "remesh_curvature") {
    /* remesh_curvature — estimate principal curvatures into .remesh.v.* and
     * print mean magnitudes (sanity: cylinder kmax~1/R, sphere kmin~kmax). */
    if (!scene.mesh) {
      err = "remesh_curvature: no mesh";
      return false;
    }
    mesh::Mesh &m = *scene.mesh;
    remesh::computeCurvature(m);

    mesh::BuiltinAttr<litestl::math::float2, ".remesh.v.k"> kval;
    kval.ensure(m.v.attrs);
    double sKmin = 0, sKmax = 0;
    int n = 0;
    for (int v : m.v) {
      sKmin += kval[v][0];
      sKmax += kval[v][1];
      n++;
    }
    std::printf("[remesh_curvature] verts=%d mean kmin=%.4f kmax=%.4f\n",
                n,
                n > 0 ? sKmin / n : 0.0,
                n > 0 ? sKmax / n : 0.0);
    std::fflush(stdout);
    return true;
  }
  if (verb == "remesh_feature_tag") {
    /* remesh_feature_tag [sharp_angle=..] — tag sharp/boundary edges into
     * .remesh.e.* and print counts. */
    if (!scene.mesh) {
      err = "remesh_feature_tag: no mesh";
      return false;
    }
    mesh::Mesh &m = *scene.mesh;
    float sharp_angle = getFloat(args, "sharp_angle", 0.7853982f);
    remesh::computeFeatureTags(m, sharp_angle);

    mesh::BuiltinAttr<bool, ".remesh.e.is_sharp"> is_sharp;
    mesh::BuiltinAttr<bool, ".remesh.e.is_boundary"> is_boundary;
    is_sharp.ensure(m.e.attrs);
    is_boundary.ensure(m.e.attrs);
    int sharp = 0, boundary = 0, n = 0;
    for (int e : m.e) {
      if (is_sharp[e]) {
        sharp++;
      }
      if (is_boundary[e]) {
        boundary++;
      }
      n++;
    }
    std::printf(
        "[remesh_feature_tag] edges=%d sharp=%d boundary=%d\n", n, sharp, boundary);
    std::fflush(stdout);
    return true;
  }
  if (verb == "remesh_closest_point") {
    /* remesh_closest_point [x=..] [y=..] [z=..] — closest surface point to the
     * query via the BVH; prints distance + face. */
    if (!scene.mesh) {
      err = "remesh_closest_point: no mesh";
      return false;
    }
    mesh::Mesh &m = *scene.mesh;
    m.thawTopo();
    spatial::SpatialTree tree(&m);
    tree.buildAll();
    for (auto *node : tree.leaves()) {
      tree.ensure_node_tris(node);
    }
    float3 p(
        getFloat(args, "x", 0.0f), getFloat(args, "y", 0.0f), getFloat(args, "z", 0.0f));
    mesh::ClosestPointResult res = mesh::findClosestPoint(tree, p);
    std::printf("[remesh_closest_point] q=(%.3f,%.3f,%.3f) hit=%d dist=%.5f "
                "face=%d point=(%.4f,%.4f,%.4f)\n",
                p[0],
                p[1],
                p[2],
                res.hit,
                res.dist,
                res.face,
                res.point[0],
                res.point[1],
                res.point[2]);
    std::fflush(stdout);
    return true;
  }
  if (verb == "remesh_cross_field") {
    /* remesh_cross_field [curvature=..] [sharp=..] [sharp_angle=..]
     * [curvature_weight=..] [smoothness=..] [curvature_smooth_iters=..]
     * [curvature_smooth_lambda=..] — solve the 4-RoSy cross field; prints face
     * count, singularity count, Σ index (== 4χ) and whether the eigen fallback
     * ran. */
    if (!scene.mesh) {
      err = "remesh_cross_field: no mesh";
      return false;
    }
    mesh::Mesh &m = *scene.mesh;
    remesh::CrossFieldParams cfp;
    cfp.use_curvature = getBool(args, "curvature", cfp.use_curvature);
    cfp.use_sharp_features = getBool(args, "sharp", cfp.use_sharp_features);
    cfp.sharp_angle = getFloat(args, "sharp_angle", cfp.sharp_angle);
    cfp.curvature_weight = getFloat(args, "curvature_weight", cfp.curvature_weight);
    cfp.field_smoothness = getFloat(args, "smoothness", cfp.field_smoothness);
    cfp.curvature_smooth_iters =
        getInt(args, "curvature_smooth_iters", cfp.curvature_smooth_iters);
    cfp.curvature_smooth_lambda =
        getFloat(args, "curvature_smooth_lambda", cfp.curvature_smooth_lambda);
    cfp.seed = (uint32_t)getInt(args, "seed", (int)cfp.seed);
    remesh::CrossFieldStats st = remesh::computeCrossField(m, cfp);
    long chi = long(m.v.count) - long(m.e.count) + long(m.f.count);
    std::printf("[remesh_cross_field] faces=%d singularities=%d index_sum=%d "
                "4chi=%ld eigen=%d\n",
                st.num_faces,
                st.num_singularities,
                st.index_sum,
                4 * chi,
                st.solved_eigen);
    std::fflush(stdout);
    return true;
  }
  if (verb == "remesh_adjust_singularities") {
    /* remesh_adjust_singularities [gauge_eps=..] [seed=..] [cancel=0|1]
     * [target_edge_length=..] [cancel_max_sep=..] — fixed-period curl reduction
     * (M3); runs computeCrossField first if no field is present. cancel=1 then
     * runs the Tier-5 pair cancellation (needs target_edge_length for its
     * geodesic gate). Prints totals and the curl before/after. */
    if (!scene.mesh) {
      err = "remesh_adjust_singularities: no mesh";
      return false;
    }
    mesh::Mesh &m = *scene.mesh;
    remesh::SingularityAdjustParams sap;
    sap.gauge_eps = getFloat(args, "gauge_eps", sap.gauge_eps);
    sap.seed = (uint32_t)getInt(args, "seed", (int)sap.seed);
    remesh::SingularityAdjustStats st = remesh::adjustSingularities(m, sap);
    long chi = long(m.v.count) - long(m.e.count) + long(m.f.count);
    std::printf("[remesh_adjust_singularities] faces=%d singularities=%d "
                "index_sum=%d 4chi=%ld curl_before=%.6f curl_after=%.6f\n",
                st.num_faces,
                st.num_singularities,
                st.index_sum,
                4 * chi,
                st.curl_before,
                st.curl_after);
    if (getBool(args, "cancel", false)) {
      remesh::SingularityCancelParams scp;
      scp.target_edge_length =
          getFloat(args, "target_edge_length", scp.target_edge_length);
      scp.max_sep = getFloat(args, "cancel_max_sep", scp.max_sep);
      scp.gauge_eps = sap.gauge_eps;
      scp.seed = sap.seed;
      remesh::SingularityCancelStats cs = remesh::cancelSingularityPairs(m, scp);
      std::printf("[remesh_cancel_pairs] rounds=%d attempted=%d cancelled=%d "
                  "reverted=%d singularities=%d index_sum=%d curl_after=%.6f\n",
                  cs.rounds,
                  cs.attempted_pairs,
                  cs.cancelled_pairs,
                  cs.reverted_rounds,
                  cs.num_singularities,
                  cs.index_sum,
                  cs.curl_after);
    }
    std::fflush(stdout);
    return true;
  }
  if (verb == "remesh_seamless") {
    /* remesh_seamless [target_edge_length=..] [use_density=..] [gauge_eps=..] —
     * seamless (u,v) parametrization (M4); runs the cross field first if absent.
     * Prints face/corner/class/cut-edge counts and the alignment diagnostics. */
    if (!scene.mesh) {
      err = "remesh_seamless: no mesh";
      return false;
    }
    mesh::Mesh &m = *scene.mesh;
    remesh::SeamlessParamParams spp;
    spp.target_edge_length = getFloat(args, "target_edge_length", spp.target_edge_length);
    spp.use_density = getBool(args, "use_density", spp.use_density);
    spp.gauge_eps = getFloat(args, "gauge_eps", spp.gauge_eps);
    remesh::SeamlessParamStats st = remesh::computeSeamlessParam(m, spp);
    std::printf("[remesh_seamless] faces=%d corners=%d classes=%d cut_edges=%d "
                "grad_angle_err=%.6f max_seam_translation=%.6e min_jacobian=%.6f "
                "solved=%d\n",
                st.num_faces,
                st.num_corners,
                st.num_classes,
                st.num_cut_edges,
                st.grad_angle_err,
                st.max_seam_translation,
                st.min_jacobian,
                st.solved);
    std::fflush(stdout);
    return true;
  }
  if (verb == "remesh_quantize") {
    /* remesh_quantize [target_edge_length=..] [use_density=..] [gauge_eps=..]
     *   [integer_tol=..] [max_lambda=..] [local_gs=..] [direct=..]
     *   [updown_max_cols=..] [supernodal=..] [seam_relax_iters=..]
     *   [seam_relax_min_folds=..] [untangle_threshold=..]
     *   [untangle_max_dev=..] (radians) —
     * integer-grid quantization (M5); builds the field/cut/seamless system
     * internally. Prints the integer residual, one-ring loop-closure residual,
     * feasibility and phase/op profile lines (local_gs=0 disables the Q1 GS
     * tier for A/B; direct=1 selects one-shot DIRECT rounding, miq.md Q4;
     * seam_relax_min_folds=1 + untangle_threshold=1 force the Tier-1b probe
     * path for A/B). */
    if (!scene.mesh) {
      err = "remesh_quantize: no mesh";
      return false;
    }
    mesh::Mesh &m = *scene.mesh;
    remesh::QuantizeParams qp;
    qp.target_edge_length = getFloat(args, "target_edge_length", qp.target_edge_length);
    qp.use_density = getBool(args, "use_density", qp.use_density);
    qp.gauge_eps = getFloat(args, "gauge_eps", qp.gauge_eps);
    qp.integer_tol = getFloat(args, "integer_tol", float(qp.integer_tol));
    qp.max_lambda = getFloat(args, "max_lambda", float(qp.max_lambda));
    qp.use_local_gs = getBool(args, "local_gs", qp.use_local_gs);
    qp.rounding = getBool(args, "direct", false) ? remesh::RoundingStrategy::DIRECT
                                                 : remesh::RoundingStrategy::GREEDY;
    qp.updown_max_cols =
        int(getFloat(args, "updown_max_cols", float(qp.updown_max_cols)));
    qp.use_supernodal = getBool(args, "supernodal", qp.use_supernodal);
    qp.seam_relax_iters =
        int(getFloat(args, "seam_relax_iters", float(qp.seam_relax_iters)));
    qp.seam_relax_min_folds =
        int(getFloat(args, "seam_relax_min_folds", float(qp.seam_relax_min_folds)));
    qp.untangle_fold_threshold =
        getFloat(args, "untangle_threshold", float(qp.untangle_fold_threshold));
    qp.untangle_field_max_dev =
        getFloat(args, "untangle_max_dev", float(qp.untangle_field_max_dev));
    remesh::QuantizeStats st = remesh::computeQuantization(m, qp);
    std::printf("[remesh_quantize] faces=%d corners=%d classes=%d cut_edges=%d "
                "int_residual=%.6e loop_closure=%.6e min_jacobian=%.6f folds=%d "
                "iters=%d solved=%d feasible=%d\n",
                st.num_faces,
                st.num_corners,
                st.num_classes,
                st.num_cut_edges,
                st.max_integer_residual,
                st.max_loop_closure,
                st.min_jacobian,
                st.parametrization_folds,
                st.iters,
                st.solved,
                st.feasible);
    std::printf("[remesh_quantize:profile] total_ms=%.1f setup=%.1f init=%.1f "
                "arap=%.1f rounding=%.1f (assemble=%.1f refactor=%.1f "
                "updown=%.1f backsolve=%.1f) convert=%.1f tier1b=%.1f "
                "stiffen=%.1f tier3=%.1f refactors=%d updowns=%d refreshes=%d "
                "back_solves=%d probes=%d\n",
                st.total_ms,
                st.setup_ms,
                st.initial_factor_ms,
                st.arap_ms,
                st.rounding_ms,
                st.round_assemble_ms,
                st.round_refactor_ms,
                st.round_updown_ms,
                st.round_backsolve_ms,
                st.convert_ms,
                st.tier1b_ms,
                st.stiffen_ms,
                st.tier3_ms,
                st.full_refactors,
                st.updowns,
                st.simp_refreshes,
                st.back_solves,
                st.tier1b_probes);
    std::printf("[remesh_quantize:gs] rounds=%d converged=%d visits=%d "
                "touched_total=%d touched_max=%d gs_ms=%.1f resort_full=%d "
                "resort_incr=%d resort_keys=%d\n",
                st.gs_rounds,
                st.gs_converged,
                st.gs_visits,
                st.gs_touched_total,
                st.gs_touched_max,
                st.gs_ms,
                st.resort_full,
                st.resort_incr,
                st.resort_keys);
    std::printf("[remesh_quantize:pairs] singularities=%d spurious_pairs=%d "
                "seamless_folds=%d near_pairs=%d\n",
                st.num_singularities,
                st.spurious_pairs,
                st.seamless_folds,
                st.seamless_folds_near_pairs);
    std::printf("[remesh_quantize:align] field_dev_mean_deg=%.2f "
                "field_dev_max_deg=%.2f field_dev_frac=%.4f\n",
                st.field_dev_mean_deg,
                st.field_dev_max_deg,
                st.field_dev_frac);
    std::fflush(stdout);
    return true;
  }

  handled = false;
  return true;
}

} // namespace sculptcore::debug_app::script
