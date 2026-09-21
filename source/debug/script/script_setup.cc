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

/** Vertex-group weight snapshots (save_weights / assert_weights), the same
 * undo-fidelity shape as g_posSnapshots but carrying each vert's whole run:
 * the pool interns by value, so a slot index is not comparable across a
 * sweep. */
std::map<std::string, std::vector<std::pair<int, std::vector<mesh::DeformWeight>>>>
    g_weightSnapshots;

} // namespace

bool execSetupVerb(Scene &scene,
                    const std::string &verb,
                    ArgMap &args,
                    const char *out_dir,
                    std::string &err,
                    bool &handled)
{
  handled = true;

  if (verb == "make_cube") {
    int dimen = getInt(args, "subdivs", 4);
    float size = getFloat(args, "size", 0.5f);
    float sphereFac = getFloat(args, "sphere", 0.0f);
    mesh::Mesh *m = mesh::createCube(dimen, size, sphereFac);
    scene.setMesh(m);
    return true;
  }
  if (verb == "make_shape") {
    /* make_shape kind=grid|cylinder|torus|sphere [n=..] [m=..] [size=..]
     *            [radius=..] [height=..] [capped=1] [minor=..] */
    const char *kind = getArg(args, "kind");
    if (!kind) {
      err = "make_shape: missing kind= (grid|cylinder|torus|sphere)";
      return false;
    }
    std::string ks = kind;
    for (auto &c : ks)
      c = (char)std::tolower((unsigned char)c);
    mesh::Mesh *m = nullptr;
    if (ks == "grid" || ks == "plane") {
      m = mesh::makeGrid(
          getInt(args, "n", 16), getInt(args, "m", 16), getFloat(args, "size", 1.0f));
    } else if (ks == "cylinder") {
      m = mesh::makeCylinder(getInt(args, "n", 24),
                             getInt(args, "m", 8),
                             getFloat(args, "radius", 0.5f),
                             getFloat(args, "height", 2.0f),
                             getBool(args, "capped", true));
    } else if (ks == "torus") {
      m = mesh::makeTorus(getInt(args, "n", 32),
                          getInt(args, "m", 16),
                          getFloat(args, "radius", 1.0f),
                          getFloat(args, "minor", 0.3f));
    } else if (ks == "sphere" || ks == "uvsphere") {
      m = mesh::makeUVSphere(
          getInt(args, "n", 16), getInt(args, "m", 24), getFloat(args, "radius", 1.0f));
    } else {
      err = std::string("make_shape: unknown kind '") + kind +
            "' (grid|cylinder|torus|sphere)";
      return false;
    }
    scene.setMesh(m);
    return true;
  }
  if (verb == "triangulate") {
    if (!scene.mesh) {
      err = "triangulate: no mesh";
      return false;
    }
    scene.mesh->thawTopo();
    mesh::triangulateMesh(*scene.mesh);
    return true;
  }
  if (verb == "build_spatial") {
    // scene.tree is a non-owning view of the level slot's tree under multires;
    // rebuilding it here would free a tree the Multires still owns.
    if (scene.multires) {
      err = "build_spatial: multires is active — level trees belong to the "
            "stack (pass leaf_limit=/depth_limit=/gpu_tri_target= to "
            "multires_init instead)";
      return false;
    }
    /* 0 => auto-derive from mesh size (SpatialTree::autoTuneLimits); any
     * positive value overrides that knob. */
    int leaf = getInt(args, "leaf_limit", 0);
    int depth = getInt(args, "depth_limit", 16);
    int gpu_tri_target = getInt(args, "gpu_tri_target", 0);
    scene.buildSpatial(leaf, depth, gpu_tri_target);
    return true;
  }
  if (verb == "set_brush") {
    scene.brush.radius = getFloat(args, "radius", scene.brush.radius);
    scene.brush.strength = getFloat(args, "strength", scene.brush.strength);
    scene.brush.spacing = getFloat(args, "spacing", scene.brush.spacing);
    scene.brush.invert = getBool(args, "invert", scene.brush.invert);
    scene.brush.pinch = getFloat(args, "pinch", scene.brush.pinch);
    scene.brush.planeoff = getFloat(args, "planeoff", scene.brush.planeoff);
    scene.brush.planeHeight = getFloat(args, "plane_height", scene.brush.planeHeight);
    scene.brush.planeDepth = getFloat(args, "plane_depth", scene.brush.planeDepth);
    scene.brush.rotateAngle = getFloat(args, "rotate_angle", scene.brush.rotateAngle);
    scene.nonAccum = getBool(args, "nonaccum", scene.nonAccum);
    scene.brush.writeProps();
    return true;
  }
  if (verb == "dyntopo") {
    /* Configure dynamic topology for subsequent strokes:
     *   dyntopo enabled=1 detail=F [min=F] [mode=both|subdivide|collapse]
     *           [max_rounds=N] [seed=N]
     * detail sets the target (l_max); min defaults to 0.4*detail. */
    scene.dyntopoEnabled = getBool(args, "enabled", true);
    float detail = getFloat(args, "detail", scene.dyntopoParams.l_max);
    scene.dyntopoParams.l_max = detail;
    scene.dyntopoParams.l_min = getFloat(args, "min", detail * 0.4f);
    scene.dyntopoParams.grade = getFloat(args, "grade", scene.dyntopoParams.grade);
    scene.dyntopoParams.do_flips = getBool(args, "flip", scene.dyntopoParams.do_flips);
    scene.dyntopoParams.max_splits =
        getInt(args, "max_splits", scene.dyntopoParams.max_splits);
    scene.dyntopoParams.do_smooth =
        getBool(args, "smooth", scene.dyntopoParams.do_smooth);
    scene.dyntopoParams.smooth_lambda =
        getFloat(args, "smooth_lambda", scene.dyntopoParams.smooth_lambda);
    scene.dyntopoParams.max_rounds =
        getInt(args, "max_rounds", scene.dyntopoParams.max_rounds);
    scene.dyntopoSeed = (uint32_t)getInt(args, "seed", (int)scene.dyntopoSeed);
    const char *mode = getArg(args, "mode", "both");
    std::string ms = mode;
    for (auto &c : ms)
      c = (char)std::tolower((unsigned char)c);
    if (ms == "subdivide") {
      scene.dyntopoParams.mode = dyntopo::DynTopoMode::Subdivide;
    } else if (ms == "collapse") {
      scene.dyntopoParams.mode = dyntopo::DynTopoMode::Collapse;
    } else if (ms == "both") {
      scene.dyntopoParams.mode = dyntopo::DynTopoMode::Both;
    } else {
      err = std::string("dyntopo: unknown mode '") + mode + "' (both|subdivide|collapse)";
      return false;
    }
    return true;
  }
  if (verb == "assert_manifold") {
    if (!scene.mesh) {
      err = "assert_manifold: no mesh";
      return false;
    }
    /* A brush stroke leaves the mesh topo-frozen (live disk/radial link pages
     * freed, CSR snapshot kept). Walking those links would dereference freed
     * pages, so thaw first (a no-op when not frozen). */
    scene.mesh->thawTopo();
    std::string why;
    if (!mesh::checkTopology(*scene.mesh, why, /*requireTriangles=*/true)) {
      err = "assert_manifold failed: " + why;
      return false;
    }
    return true;
  }
  if (verb == "set_backend") {
    const char *b = getArg(args, "backend");
    if (!b) {
      err = "set_backend: missing backend=";
      return false;
    }
    std::string bs = b;
    for (auto &c : bs)
      c = (char)std::tolower((unsigned char)c);
    if (bs == "cpp") {
      scene.currentBackend = BrushBackend::Cpp;
    } else if (bs == "wgsl") {
#ifdef SBRUSH_BACKEND_WGSL
      // Wave 3: WGSL artifacts exist + tint-validated. There is no
      // WebGPU runtime native, so brushes still execute through the C++
      // path — selecting Wgsl here is the artifact-presence gate that
      // CI uses to verify the WGSL pipeline configured cleanly.
      scene.currentBackend = BrushBackend::Wgsl;
#else
      err = "set_backend: WGSL backend not compiled in (configure with "
            "--backends=cpp,wgsl)";
      return false;
#endif
    } else if (bs == "webgpu") {
#ifdef SBRUSH_WEBGPU_COMPUTE
      // Real GPU compute through webgpu.h / wgpu-native (the .wgsl kernels).
      scene.currentBackend = BrushBackend::WgpuNative;
#else
      err = "set_backend: WebGPU backend not compiled in (configure with "
            "-DSBRUSH_WEBGPU_COMPUTE=ON --backends=cpp,wgsl,spirv)";
      return false;
#endif
    } else {
      err = std::string("set_backend: unknown backend '") + b +
            "' (valid: cpp, wgsl, webgpu)";
      return false;
    }
    return true;
  }
  if (verb == "set_neighbor_mode") {
    const char *mode = getArg(args, "mode");
    if (!mode) {
      err = "set_neighbor_mode: missing mode= (livedisk|csr)";
      return false;
    }
    std::string ms = mode;
    for (auto &c : ms)
      c = (char)std::tolower((unsigned char)c);
    if (ms == "livedisk") {
      scene.useCsrNeighbors = false;
    } else if (ms == "csr") {
      scene.useCsrNeighbors = true;
    } else {
      err = std::string("set_neighbor_mode: unknown mode '") + mode +
            "' (valid: livedisk, csr)";
      return false;
    }
    return true;
  }
  if (verb == "set_brush_tool") {
    const char *t = getArg(args, "tool");
    if (!t) {
      err = "set_brush_tool: missing tool=";
      return false;
    }
    std::string ts = t;
    for (auto &c : ts)
      c = (char)std::tolower((unsigned char)c);
    // Reflect over the generated per-id name table (builtin_brushes.gen.h):
    // every built-in tool is addressable by its lowercased enum name, so a
    // new .sbrush @tool needs no edit here.
    int found = -1;
    for (int id = 0; id < brush::builtinBrushCount; id++) {
      std::string name = brush::kBuiltinBrushNames[id];
      for (auto &c : name)
        c = (char)std::tolower((unsigned char)c);
      if (ts == name) {
        found = id;
        break;
      }
    }
    if (found < 0) {
      err = std::string("set_brush_tool: unknown tool '") + t + "'";
      return false;
    }
    scene.currentTool = brush::SculptBrushes(found);
    return true;
  }
  if (verb == "set_grab") {
    float3 from{0, 0, 0}, to{0, 0, 0};
    if (!parseFloat3(getArg(args, "from"), from)) {
      err = "set_grab: missing from=x,y,z";
      return false;
    }
    if (!parseFloat3(getArg(args, "to"), to)) {
      err = "set_grab: missing to=x,y,z";
      return false;
    }
    scene.brush.grabFrom = from;
    scene.brush.grabTo = to;
    return true;
  }
  if (verb == "set_falloff") {
    const char *k = getArg(args, "kind");
    const char *sh = getArg(args, "shape");
    const char *dir = getArg(args, "dir");
    if (!k && !sh && !dir) {
      err = "set_falloff: need at least one of kind=, shape=, dir=";
      return false;
    }
    if (k) {
      std::string ks = k;
      for (auto &c : ks)
        c = (char)std::tolower((unsigned char)c);
      if (ks == "smoothstep") {
        scene.brush.falloff_kind = brush::FalloffKind::Smoothstep;
      } else if (ks == "linear") {
        scene.brush.falloff_kind = brush::FalloffKind::Linear;
      } else if (ks == "gaussian") {
        scene.brush.falloff_kind = brush::FalloffKind::Gaussian;
      } else if (ks == "curve") {
        scene.brush.falloff_kind = brush::FalloffKind::Curve;
      } else {
        err = std::string("set_falloff: unknown kind '") + k +
              "' (valid: smoothstep, linear, gaussian, curve)";
        return false;
      }
    }
    if (sh) {
      std::string ss = sh;
      for (auto &c : ss)
        c = (char)std::tolower((unsigned char)c);
      if (ss == "spherical") {
        scene.brush.falloff_shape = brush::FalloffShape::Spherical;
      } else if (ss == "cube") {
        scene.brush.falloff_shape = brush::FalloffShape::Cube;
      } else if (ss == "linear") {
        scene.brush.falloff_shape = brush::FalloffShape::Linear;
      } else if (ss == "box") {
        scene.brush.falloff_shape = brush::FalloffShape::Box;
      } else if (ss == "rounded_box") {
        scene.brush.falloff_shape = brush::FalloffShape::RoundedBox;
      } else {
        err = std::string("set_falloff: unknown shape '") + sh +
              "' (valid: spherical, cube, linear, box, rounded_box)";
        return false;
      }
    }
    if (dir) {
      float3 d;
      if (!parseFloat3(dir, d)) {
        err = "set_falloff: dir= must be x,y,z";
        return false;
      }
      scene.brush.falloff_dir = d.normalized();
    }
    if (const char *ext = getArg(args, "extent")) {
      float3 e;
      if (!parseFloat3(ext, e)) {
        err = "set_falloff: extent= must be x,y,z";
        return false;
      }
      scene.brush.falloff_extent = e;
    }
    if (const char *r = getArg(args, "roundness")) {
      scene.brush.falloff_roundness = float(std::atof(r));
    }
    return true;
  }
  if (verb == "set_falloff_curve") {
    const char *p = getArg(args, "preset");
    if (!p) {
      err = "set_falloff_curve: missing preset=smoothstep|linear|inverse|gaussian";
      return false;
    }
    std::string ps = p;
    for (auto &c : ps)
      c = (char)std::tolower((unsigned char)c);
    using CP = brush::Brush::CurvePreset;
    if (ps == "smoothstep")
      scene.brush.setFalloffCurvePreset(CP::Smoothstep);
    else if (ps == "linear")
      scene.brush.setFalloffCurvePreset(CP::Linear);
    else if (ps == "inverse")
      scene.brush.setFalloffCurvePreset(CP::Inverse);
    else if (ps == "gaussian")
      scene.brush.setFalloffCurvePreset(CP::Gaussian);
    else {
      err = std::string("set_falloff_curve: unknown preset '") + p +
            "' (valid: smoothstep, linear, inverse, gaussian)";
      return false;
    }
    return true;
  }
  if (verb == "set_texture") {
    // Bind a brush texture from one of four sources, priority order:
    // script= (JIT'd .stex), image=, proc=, pattern=. `script=clear` /
    // `pattern=clear` unbind. Full semantics: documentation/debugApp.md.
    const char *scriptPath = getArg(args, "script");
    const char *imgPath = getArg(args, "image");
    const char *proc = getArg(args, "proc");
    const char *pat = getArg(args, "pattern", "rampx");

    if (scriptPath) {
      if (std::string(scriptPath) == "clear") {
        scene.brush.clearTextureScript();
        return true;
      }
      std::ifstream f(scriptPath, std::ios::binary);
      if (!f) {
        err = std::string("set_texture: cannot open script '") + scriptPath + "'";
        return false;
      }
      std::string src((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
      if (!scene.brush.setTextureScriptSource(src.c_str(), scriptPath)) {
        err = std::string("set_texture: ") + scene.brush.texture_script_error.c_str();
        return false;
      }
      return true;
    }

    if (imgPath) {
      int iw = 0, ih = 0, comp = 0;
      // Force a single channel: stb collapses RGB(A) to luminance with its
      // fixed integer weights, so the result is deterministic across runs.
      unsigned char *data = stbi_load(imgPath, &iw, &ih, &comp, 1);
      if (!data) {
        err = std::string("set_texture: failed to load image '") + imgPath +
              "': " + stbi_failure_reason();
        return false;
      }
      scene.brush.tex_width = iw;
      scene.brush.tex_height = ih;
      scene.brush.tex_pixels.resize(iw * ih);
      for (int i = 0; i < iw * ih; i++) {
        scene.brush.tex_pixels[i] = (float)data[i] / 255.0f;
      }
      stbi_image_free(data);
      return true;
    }

    std::string ps = pat;
    for (auto &c : ps)
      c = (char)std::tolower((unsigned char)c);
    if (!proc && ps == "clear") {
      scene.brush.tex_width = 0;
      scene.brush.tex_height = 0;
      scene.brush.tex_pixels.clear();
      return true;
    }

    int w = getInt(args, "width", 64);
    int h = getInt(args, "height", 64);
    if (w <= 0 || h <= 0) {
      err = "set_texture: width/height must be positive";
      return false;
    }
    scene.brush.tex_width = w;
    scene.brush.tex_height = h;
    scene.brush.tex_pixels.resize(w * h);

    std::string procName;
    if (proc) {
      procName = proc;
      for (auto &c : procName)
        c = (char)std::tolower((unsigned char)c);
    }
    constexpr float kTwoPi = 6.28318530717958647692f;
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        float u = w > 1 ? (float)x / (float)(w - 1) : 0.0f;
        float v = h > 1 ? (float)y / (float)(h - 1) : 0.0f;
        float val;
        if (proc) {
          // Analytic generators: continuous functions of the normalized UV,
          // distinct in shape from the discrete `pattern` modes.
          if (procName == "radial") {
            float dx = u - 0.5f, dy = v - 0.5f;
            float d = std::sqrt(dx * dx + dy * dy) * 2.0f;
            val = std::max(0.0f, 1.0f - d);
          } else if (procName == "sine") {
            val = 0.5f + 0.5f * std::sin(kTwoPi * u);
          } else if (procName == "gradient") {
            val = 0.5f * (u + v);
          } else {
            err = std::string("set_texture: unknown proc '") + proc +
                  "' (valid: radial, sine, gradient)";
            return false;
          }
        } else if (ps == "rampx") {
          val = u;
        } else if (ps == "rampy") {
          val = v;
        } else if (ps == "checker") {
          val = ((x ^ y) & 1) ? 1.0f : 0.0f;
        } else if (ps == "constant") {
          val = 1.0f;
        } else {
          err = std::string("set_texture: unknown pattern '") + pat +
                "' (valid: rampx, rampy, checker, constant, clear; or use "
                "proc=/image=)";
          return false;
        }
        scene.brush.tex_pixels[y * w + x] = val;
      }
    }
    return true;
  }
  if (verb == "set_coord_space") {
    const char *sp = getArg(args, "space");
    const char *rep = getArg(args, "repeat");
    if (!sp && !rep) {
      err = "set_coord_space: need space= and/or repeat=";
      return false;
    }
    if (sp) {
      std::string ss = sp;
      for (auto &c : ss)
        c = (char)std::tolower((unsigned char)c);
      if (ss == "global") {
        scene.brush.coord_space = brush::TexCoordSpace::Global;
      } else if (ss == "viewplane") {
        scene.brush.coord_space = brush::TexCoordSpace::ViewPlane;
      } else if (ss == "viewrepeat") {
        scene.brush.coord_space = brush::TexCoordSpace::ViewRepeat;
      } else if (ss == "stroke_curved") {
        scene.brush.coord_space = brush::TexCoordSpace::StrokeCurved;
      } else if (ss == "projected") {
        scene.brush.coord_space = brush::TexCoordSpace::Projected;
      } else {
        err = std::string("set_coord_space: unknown space '") + sp +
              "' (valid: global, viewplane, viewrepeat, stroke_curved, "
              "projected)";
        return false;
      }
    }
    if (rep) {
      scene.brush.tex_repeat = (float)std::atof(rep);
    }
    return true;
  }
  if (verb == "set_render_matrix") {
    // m=<16 comma-separated floats>, column-major (matches mat4 / std140
    // mat4x4 storage). Drives VIEWPLANE/VIEWREPEAT texture coords on both
    // backends: uv = (renderMatrix * co).xy. Identity by default.
    const char *m = getArg(args, "m");
    if (!m) {
      err = "set_render_matrix: need m=<16 comma-separated floats>";
      return false;
    }
    float v[16];
    int n = std::sscanf(m,
                        "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f",
                        &v[0],
                        &v[1],
                        &v[2],
                        &v[3],
                        &v[4],
                        &v[5],
                        &v[6],
                        &v[7],
                        &v[8],
                        &v[9],
                        &v[10],
                        &v[11],
                        &v[12],
                        &v[13],
                        &v[14],
                        &v[15]);
    if (n != 16) {
      err = "set_render_matrix: m= must list exactly 16 floats";
      return false;
    }
    float *dst = scene.renderMatrix;
    for (int i = 0; i < 16; i++) {
      dst[i] = v[i];
    }
    return true;
  }
  if (verb == "set_kelvinlet_params") {
    const char *muArg = getArg(args, "mu");
    const char *nuArg = getArg(args, "nu");
    if (muArg)
      scene.brush.mu = float(std::atof(muArg));
    if (nuArg)
      scene.brush.nu = float(std::atof(nuArg));
    return true;
  }
  if (verb == "set_pose_cage_rest" || verb == "set_pose_cage_now") {
    const char *idxArg = getArg(args, "idx");
    if (!idxArg) {
      err = std::string(verb) + ": missing idx=";
      return false;
    }
    int idx = std::atoi(idxArg);
    if (idx < 0 || idx >= 4) {
      err = std::string(verb) + ": idx must be in [0, 4)";
      return false;
    }
    float3 pos;
    if (!parseFloat3(getArg(args, "pos"), pos)) {
      err = std::string(verb) + ": missing pos=x,y,z";
      return false;
    }
    if (verb == "set_pose_cage_rest")
      scene.brush.poseCageRest[idx] = pos;
    else
      scene.brush.poseCageNow[idx] = pos;
    return true;
  }
  if (verb == "layer_add") {
    if (!scene.mesh) {
      err = "layer_add: no mesh";
      return false;
    }
    std::string name = getArg(args, "name", "");
    if (name.empty()) {
      err = "layer_add: missing name=";
      return false;
    }
    int idx = scene.mesh->addSculptLayerNamed(name.c_str());
    displace::setLayerWeight(*scene.mesh, idx, getFloat(args, "weight", 1.0f));
    displace::setLayerEnabled(*scene.mesh, idx, getInt(args, "enabled", 1) != 0);
    displace::setLayerFrozen(*scene.mesh, idx, getInt(args, "frozen", 0) != 0);
    std::printf("layer_add: '%s' -> settings index %d\n",
                scene.mesh->sculptLayers[idx].name.c_str(),
                idx);
    return true;
  }
  // layer_set name=<s> [weight=f] [enabled=0/1] [frozen=0/1] — mutate a sculpt
  // layer's settings through the compositor (evaluated positions stay current),
  // then refresh spatial bounds/normals/GPU state.
  if (verb == "layer_set") {
    if (!scene.mesh) {
      err = "layer_set: no mesh";
      return false;
    }
    std::string name = getArg(args, "name", "");
    int idx = scene.mesh->findSculptLayer(litestl::util::string(name.c_str()));
    if (idx < 0) {
      err = "layer_set: unknown layer '" + name + "'";
      return false;
    }
    if (getArg(args, "weight")) {
      displace::setLayerWeight(*scene.mesh, idx, getFloat(args, "weight", 1.0f));
    }
    if (getArg(args, "enabled")) {
      displace::setLayerEnabled(*scene.mesh, idx, getInt(args, "enabled", 1) != 0);
    }
    if (getArg(args, "frozen")) {
      displace::setLayerFrozen(*scene.mesh, idx, getInt(args, "frozen", 0) != 0);
    }
    if (scene.tree) {
      for (auto *node : scene.tree->leaves()) {
        node->flag |= Spatial_RegenBounds | Spatial_UpdateNormals | Spatial_UpdateGPU;
      }
      scene.tree->update(&scene.gpu);
    }
    return true;
  }
  if (verb == "view") {
    const char *v = getArg(args, "preset", "persp");
    ViewPreset p = ViewPreset::Persp;
    if (std::strcmp(v, "front") == 0)
      p = ViewPreset::Front;
    else if (std::strcmp(v, "top") == 0)
      p = ViewPreset::Top;
    else if (std::strcmp(v, "side") == 0)
      p = ViewPreset::Side;
    else if (std::strcmp(v, "persp") == 0)
      p = ViewPreset::Persp;
    else if (std::strcmp(v, "free") == 0)
      p = ViewPreset::Free;
    scene.applyView(p);
    return true;
  }
  if (verb == "screenshot") {
    const char *v = getArg(args, "view");
    if (v) {
      ArgMap sub;
      sub["preset"] = v;
      std::string subErr;
      bool subHandled;
      execSetupVerb(scene, "view", sub, out_dir, subErr, subHandled);
    }
    scene.showLeafBounds = getBool(args, "leaves", scene.showLeafBounds);
    scene.renderHeadless();
    const char *rel = getArg(args, "out");
    if (!rel) {
      err = "screenshot: missing out=...";
      return false;
    }
    std::string path = joinPath(out_dir, rel);
    if (!scene.screenshot(path.c_str())) {
      err = "screenshot: failed to write " + path;
      return false;
    }
    return true;
  }
  if (verb == "dump_state") {
    const char *rel = getArg(args, "out");
    if (!rel) {
      err = "dump_state: missing out=...";
      return false;
    }
    state_dump::Options opts;
    opts.mesh = getBool(args, "mesh", true);
    opts.spatial = getBool(args, "spatial", true);
    opts.brush = getBool(args, "brush", true);
    std::string path = joinPath(out_dir, rel);
    if (!state_dump::writeJSON(scene, path.c_str(), opts)) {
      err = "dump_state: failed to write " + path;
      return false;
    }
    return true;
  }
  if (verb == "checkpoint") {
    /* Marker only — meshlog step boundaries are managed by stroke verbs.
     * Useful for human readability of script logs. */
    return true;
  }
  if (verb == "echo") {
    const char *msg = getArg(args, "msg", "");
    std::fprintf(stdout, "[script] %s\n", msg);
    return true;
  }
  if (verb == "set_weights") {
    if (!scene.mesh) {
      err = "set_weights: no mesh";
      return false;
    }
    const char *name = getArg(args, "name", "weights");
    const int group = getInt(args, "group", 0);
    const char *value = getArg(args, "value");

    // Default is a z-gradient over the mesh AABB, not a constant: a constant
    // survives any interpolator, correct or not, so it would not catch a merge
    // handler that lost a run across a dyntopo split.
    float zlo = FLT_MAX, zhi = -FLT_MAX;
    for (int v : scene.mesh->v) {
      const float z = scene.mesh->v.co[v][2];
      zlo = std::min(zlo, z);
      zhi = std::max(zhi, z);
    }
    const float span = (zhi > zlo) ? (zhi - zlo) : 1.0f;

    mesh::WeightsRef w = mesh::ensureVertWeights(*scene.mesh, name);
    int n = 0;
    for (int v : scene.mesh->v) {
      const float weight =
          value ? float(std::atof(value)) : (scene.mesh->v.co[v][2] - zlo) / span;
      mesh::DeformWeight dw{group, weight};
      w.setRun(v, litestl::util::span<const mesh::DeformWeight>(&dw, 1));
      n++;
    }
    std::fprintf(stdout,
                 "[script] set_weights name=%s group=%d verts=%d slots=%zu\n",
                 name,
                 group,
                 n,
                 scene.mesh->deformPool().liveSlotCount());
    return true;
  }
  if (verb == "save_weights") {
    if (!scene.mesh) {
      err = "save_weights: no mesh";
      return false;
    }
    const char *name = getArg(args, "name", "weights");
    mesh::WeightsRef w = mesh::findVertWeights(*scene.mesh, name);
    if (!w.exists()) {
      err = std::string("save_weights: no weights layer '") + name + "'";
      return false;
    }
    std::string id = getArg(args, "id", "default");
    auto &snap = g_weightSnapshots[id];
    snap.clear();
    mesh::DeformWeight buf[mesh::DEFORM_MAX_INFLUENCES];
    for (int v : scene.mesh->v) {
      const int cnt = w.getRun(v, buf, mesh::DEFORM_MAX_INFLUENCES);
      snap.emplace_back(v, std::vector<mesh::DeformWeight>(buf, buf + cnt));
    }
    std::fprintf(stdout,
                 "[script] save_weights id=%s name=%s verts=%zu\n",
                 id.c_str(),
                 name,
                 snap.size());
    return true;
  }
  if (verb == "assert_weights") {
    if (!scene.mesh) {
      err = "assert_weights: no mesh";
      return false;
    }
    const char *name = getArg(args, "name", "weights");
    std::string id = getArg(args, "id", "default");
    const float eps = getFloat(args, "eps", 1e-5f);
    auto it = g_weightSnapshots.find(id);
    if (it == g_weightSnapshots.end()) {
      err = "assert_weights: no snapshot '" + id + "'";
      return false;
    }
    mesh::WeightsRef w = mesh::findVertWeights(*scene.mesh, name);
    if (!w.exists()) {
      err = std::string("assert_weights: no weights layer '") + name + "'";
      return false;
    }

    int dead = 0, reshaped = 0, changed = 0, worstIdx = -1, firstBad = -1;
    float worst = 0.0f;
    mesh::DeformWeight buf[mesh::DEFORM_MAX_INFLUENCES];
    for (auto &pr : it->second) {
      const int v = pr.first;
      if (v >= int(scene.mesh->v.capacity()) || scene.mesh->v.freemap[v]) {
        dead++;
        if (firstBad < 0)
          firstBad = v;
        continue;
      }
      const int cnt = w.getRun(v, buf, mesh::DEFORM_MAX_INFLUENCES);
      if (cnt != int(pr.second.size())) {
        reshaped++;
        if (firstBad < 0)
          firstBad = v;
        continue;
      }
      // Both runs are canonicalized group-ascending, so this compares entry for
      // entry; a differing group counts as an unbounded value drift.
      for (int i = 0; i < cnt; i++) {
        const float d = (buf[i].group != pr.second[i].group)
                            ? FLT_MAX
                            : std::fabs(buf[i].weight - pr.second[i].weight);
        if (d > eps) {
          changed++;
          if (firstBad < 0)
            firstBad = v;
          if (d > worst) {
            worst = d;
            worstIdx = v;
          }
          break;
        }
      }
    }
    std::fprintf(stdout,
                 "[script] assert_weights id=%s checked=%zu dead=%d reshaped=%d "
                 "changed=%d worst=%g (vert %d)\n",
                 id.c_str(),
                 it->second.size(),
                 dead,
                 reshaped,
                 changed,
                 worst,
                 worstIdx);
    if (dead > 0 || reshaped > 0 || changed > 0) {
      char buf2[256];
      std::snprintf(buf2,
                    sizeof(buf2),
                    "assert_weights: %d dead, %d reshaped, %d changed (worst %g at "
                    "vert %d, first %d)",
                    dead,
                    reshaped,
                    changed,
                    worst,
                    worstIdx,
                    firstBad);
      err = buf2;
      // soft=1 reports the divergence but lets the script continue.
      return getBool(args, "soft", false);
    }
    return true;
  }

  handled = false;
  return true;
}

} // namespace sculptcore::debug_app::script
