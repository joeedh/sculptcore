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

namespace {

/* VDM texel snapshots (save_vdm / assert_vdm): every live tile's texels,
 * keyed by snapshot name — the texel analogue of g_posSnapshots. */
std::map<std::string, std::map<uint64_t, std::vector<float3>>> g_vdmSnapshots;

} // namespace

bool execVdmVerb(Scene &scene,
                    const std::string &verb,
                    ArgMap &args,
                    const char *out_dir,
                    std::string &err,
                    bool &handled)
{
  handled = true;

  // layer_add name=<s> [weight=f] [enabled=0/1] [frozen=0/1] — create a sculpt
  // layer (a VERTEX FLOAT3 SCULPT_LAYER attr + settings row). The name is
  // uniquified if taken, so scripts should pick fresh names.
  // vdm_init [resolution=1024] [tile=64] [planar_uv=1] [alpha=0.5] — create the
  // scene VdmStore, tag every face `.detail.carrier = VDM`, optionally build a
  // planar corner-UV atlas from the mesh's xy bbox, and compute the F3 frames.
  if (verb == "vdm_init") {
    if (!scene.mesh || !scene.tree) {
      err = "vdm_init: no mesh/tree (build_spatial first)";
      return false;
    }
    if (scene.vdm) {
      litestl::alloc::Delete(scene.vdm);
    }
    vdm::VdmStoreParams vp;
    vp.resolution = getInt(args, "resolution", 1024);
    vp.tile_size = getInt(args, "tile", 64);
    scene.vdm = litestl::alloc::New<vdm::VdmStore>("VdmStore", vp);

    mesh::Mesh *m = scene.mesh;
    if (getInt(args, "planar_uv", 0)) {
      // Project vertex xy onto [0,1]² and write per-corner UVs (continuous
      // across faces — no seams, so the splatter needs no skirts).
      float3 mn(FLT_MAX), mx(-FLT_MAX);
      for (int v : m->v) {
        mn.min(m->v.co[v]);
        mx.max(m->v.co[v]);
      }
      float sx = mx[0] - mn[0] > 1e-12f ? 1.0f / (mx[0] - mn[0]) : 1.0f;
      float sy = mx[1] - mn[1] > 1e-12f ? 1.0f / (mx[1] - mn[1]) : 1.0f;
      mesh::AttrRef &uvRef =
          m->c.attrs.ensure(mesh::AttrType::FLOAT2, litestl::util::string("uv"), true);
      uvRef.use = uvRef.use | mesh::AttrUse::UV;
      auto *uv = static_cast<mesh::AttrData<litestl::math::float2> *>(uvRef.data);
      for (int c : m->c) {
        float3 co = m->v.co[m->c.v[c]];
        (*uv)[c] = litestl::math::float2((co[0] - mn[0]) * sx, (co[1] - mn[1]) * sy);
      }
    }

    for (int f : m->f) {
      scene.tree->treeMesh.f.carrier.get_data()->materialize(f);
      scene.tree->treeMesh.f.carrier[f] = int(spatial::DetailCarrier::VDM);
    }

    m->recalc_normals();
    displace::FrameProviderParams fp;
    displace::updateFramesAll(*m, fp);
    std::printf("vdm_init: resolution=%d tile=%d faces=%d\n",
                vp.resolution,
                vp.tile_size,
                int(m->f.count));
    return true;
  }
  // vdm_stroke origin=x,y,z [normal=x,y,z] [radius=] [strength=] [alpha=]
  // [invert=0] [repeat=1] — one meshlog step of `repeat` splatted dabs, the
  // tile deltas bracketed into the step via VdmLogChunk.
  if (verb == "vdm_stroke") {
    if (!scene.mesh || !scene.tree || !scene.vdm) {
      err = "vdm_stroke: run vdm_init first";
      return false;
    }
    vdm::VdmSplatParams sp;
    if (!parseFloat3(getArg(args, "origin"), sp.center)) {
      err = "vdm_stroke: missing origin=x,y,z";
      return false;
    }
    parseFloat3(getArg(args, "normal"), sp.normal);
    sp.radius = getFloat(args, "radius", scene.brush.radius);
    sp.strength = getFloat(args, "strength", 0.5f);
    sp.alpha = getFloat(args, "alpha", 0.5f);
    sp.invert = getInt(args, "invert", 0) != 0;
    int repeat = getInt(args, "repeat", 1);

    scene.meshLog.setActiveMesh(scene.mesh);
    scene.meshLog.beginStep(false);
    scene.vdm->beginDelta();
    vdm::VdmSplatStats total;
    for (int i = 0; i < repeat; i++) {
      vdm::VdmSplatStats s = vdm::splatDab(*scene.mesh, *scene.tree, *scene.vdm, sp);
      total.facesTouched += s.facesTouched;
      total.texelsTouched += s.texelsTouched;
      total.texelsClamped += s.texelsClamped;
    }
    vdm::VdmDelta *delta = scene.vdm->endDelta();
    if (delta) {
      auto *chunk = litestl::alloc::New<vdm::VdmLogChunk>(
          "VdmLogChunk", scene.vdm, std::move(*delta));
      litestl::alloc::Delete(delta);
      scene.meshLog.appendChunk(chunk);
    }
    scene.meshLog.endStep();
    std::printf("vdm_stroke: faces=%d texels=%d clamped=%d tiles=%d\n",
                total.facesTouched,
                total.texelsTouched,
                total.texelsClamped,
                scene.vdm->tileCount());
    return true;
  }
  // vdm_promote [alpha=0.6] [theta=60] [cuts=1] [force=0] — evaluate the V4
  // eligibility predicate over the VDM faces (force=1: every face with stored
  // displacement) and promote the candidates to geometry, as one undo step.
  if (verb == "vdm_promote") {
    if (!scene.mesh || !scene.tree || !scene.vdm) {
      err = "vdm_promote: run vdm_init first";
      return false;
    }
    vdm::VdmPromoteParams pp;
    pp.alpha_promote = getFloat(args, "alpha", 0.6f);
    pp.theta_max_deg = getFloat(args, "theta", 60.0f);
    pp.subdiv_cuts = getInt(args, "cuts", 1);
    pp.force = getInt(args, "force", 0) != 0;

    // Candidate pool: force=1 restricts to faces carrying stored displacement
    // (their exported bound is nonzero); else every VDM face runs the predicate.
    Vector<int> pool;
    for (int f : scene.mesh->f) {
      if (scene.tree->treeMesh.f.carrier[f] == int(spatial::DetailCarrier::VDM)) {
        pool.append(f);
      }
    }
    if (pp.force) {
      Vector<float> bounds;
      vdm::exportFaceBounds(*scene.vdm,
                            *scene.mesh,
                            std::span<const int>(pool.data(), pool.size()),
                            bounds);
      Vector<int> bounded;
      for (int i = 0; i < int(pool.size()); i++) {
        if (bounds[i] > 1e-8f) {
          bounded.append(pool[i]);
        }
      }
      pool = std::move(bounded);
    }
    Vector<int> candidates;
    vdm::collectPromotionCandidates(*scene.mesh,
                                    *scene.tree,
                                    *scene.vdm,
                                    std::span<const int>(pool.data(), pool.size()),
                                    pp,
                                    candidates);
    if (candidates.size() == 0) {
      std::printf("vdm_promote: no candidates (pool=%d)\n", int(pool.size()));
      return true;
    }

    // Combined callbacks: meshlog capture + spatial currency (the same pairing
    // applyDynTopoDab composes for dyntopo).
    mesh::MeshCallbacks *logCb = scene.meshLog.callbacks();
    mesh::MeshCallbacks *spatialCb = scene.tree->getSpatialCallbacks();
    mesh::MeshCallbacks combined = *logCb;
    auto chain = [](litestl::util::function<void(int)> &dst,
                    litestl::util::function<void(int)> a,
                    litestl::util::function<void(int)> b) {
      dst = [a, b](int i) {
        if (a) {
          a(i);
        }
        if (b) {
          b(i);
        }
      };
    };
    chain(combined.onVertCreate, logCb->onVertCreate, spatialCb->onVertCreate);
    chain(combined.onVertChange, logCb->onVertChange, spatialCb->onVertChange);
    chain(combined.onVertKill, logCb->onVertKill, spatialCb->onVertKill);
    chain(combined.onEdgeCreate, logCb->onEdgeCreate, spatialCb->onEdgeCreate);
    chain(combined.onEdgeChange, logCb->onEdgeChange, spatialCb->onEdgeChange);
    chain(combined.onEdgeKill, logCb->onEdgeKill, spatialCb->onEdgeKill);
    chain(combined.onCornerCreate, logCb->onCornerCreate, spatialCb->onCornerCreate);
    chain(combined.onCornerChange, logCb->onCornerChange, spatialCb->onCornerChange);
    chain(combined.onCornerKill, logCb->onCornerKill, spatialCb->onCornerKill);
    chain(combined.onListCreate, logCb->onListCreate, spatialCb->onListCreate);
    chain(combined.onListChange, logCb->onListChange, spatialCb->onListChange);
    chain(combined.onListKill, logCb->onListKill, spatialCb->onListKill);
    chain(combined.onFaceCreate, logCb->onFaceCreate, spatialCb->onFaceCreate);
    chain(combined.onFaceChange, logCb->onFaceChange, spatialCb->onFaceChange);
    chain(combined.onFaceKill, logCb->onFaceKill, spatialCb->onFaceKill);

    scene.meshLog.setActiveMesh(scene.mesh);
    scene.meshLog.beginStep(/*hasDyntopo=*/true);
    scene.vdm->beginDelta();
    vdm::VdmPromoteStats ps =
        vdm::promoteRegion(*scene.mesh,
                           *scene.tree,
                           *scene.vdm,
                           std::span<const int>(candidates.data(), candidates.size()),
                           pp,
                           &combined,
                           &scene.meshLog);
    vdm::VdmDelta *delta = scene.vdm->endDelta();
    if (delta) {
      auto *chunk = litestl::alloc::New<vdm::VdmLogChunk>(
          "VdmLogChunk", scene.vdm, std::move(*delta));
      litestl::alloc::Delete(delta);
      scene.meshLog.appendChunk(chunk);
    }
    scene.meshLog.endStep();
    scene.mesh->recomputeBoundary();
    scene.tree->update(&scene.gpu);
    std::printf(
        "vdm_promote: candidates=%d promoted=%d seeded=%d cleared=%d regionEdges=%d\n",
        int(candidates.size()),
        ps.promoted,
        ps.seededVerts,
        ps.clearedTexels,
        ps.regionEdges);
    return true;
  }
  // save_vdm [id=default] — snapshot every live tile's texels.
  if (verb == "save_vdm") {
    if (!scene.vdm) {
      err = "save_vdm: no VdmStore (vdm_init first)";
      return false;
    }
    std::string name = getArg(args, "id", "default");
    auto &snap = g_vdmSnapshots[name];
    snap.clear();
    scene.vdm->foreachTile([&](const vdm::VdmTile &t) {
      auto &texels = snap[vdm::VdmStore::tileKey(t.tx, t.ty)];
      texels.resize(t.texels.size());
      for (size_t i = 0; i < t.texels.size(); i++) {
        texels[i] = t.texels[int(i)];
      }
    });
    std::printf("save_vdm: '%s' %zu tiles\n", name.c_str(), snap.size());
    return true;
  }
  // assert_vdm [id=default] [eps=1e-6] — every texel matches the snapshot
  // (tile sets equal, values within eps).
  if (verb == "assert_vdm") {
    if (!scene.vdm) {
      err = "assert_vdm: no VdmStore";
      return false;
    }
    std::string name = getArg(args, "id", "default");
    auto it = g_vdmSnapshots.find(name);
    if (it == g_vdmSnapshots.end()) {
      err = "assert_vdm: unknown snapshot '" + name + "'";
      return false;
    }
    float eps = getFloat(args, "eps", 1e-6f);
    int liveTiles = 0, missing = 0, changed = 0;
    scene.vdm->foreachTile([&](const vdm::VdmTile &t) {
      liveTiles++;
      auto st = it->second.find(vdm::VdmStore::tileKey(t.tx, t.ty));
      if (st == it->second.end()) {
        missing++;
        return;
      }
      const auto &sv = st->second;
      for (int i = 0; i < int(t.texels.size()); i++) {
        float3 d = t.texels[i] - sv[size_t(i)];
        if (std::fabs(d[0]) > eps || std::fabs(d[1]) > eps || std::fabs(d[2]) > eps) {
          changed++;
          return;
        }
      }
    });
    int snapTiles = int(it->second.size());
    std::printf("assert_vdm: live=%d snap=%d extra=%d changed=%d\n",
                liveTiles,
                snapTiles,
                missing,
                changed);
    if (missing != 0 || changed != 0 || liveTiles != snapTiles) {
      err = "assert_vdm: store differs from snapshot '" + name + "'";
      return false;
    }
    return true;
  }

  handled = false;
  return true;
}

} // namespace sculptcore::debug_app::script
