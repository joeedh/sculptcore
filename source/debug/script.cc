#include "script.h"

#include "scene.h"
#include "state_dump.h"

#include "brush/brush_executor.h"
#include "brush/stroke_spacing.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"
#include "mesh/mesh_shapes.h"
#include "spatial/spatial.h"
#include "stb/stb_image.h"

#ifdef SBRUSH_GPU_DISPATCH
#include "gpu_stroke.h"
#endif

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace sculptcore::debug_app::script {

using litestl::math::float3;
using litestl::util::Vector;

namespace {

using ArgMap = std::map<std::string, std::string>;

std::string trim(const std::string &s)
{
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) {
    a++;
  }
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
    b--;
  }
  return s.substr(a, b - a);
}

bool parseLine(const std::string &line, std::string &verb, ArgMap &args)
{
  std::string t = trim(line);
  if (t.empty() || t[0] == '#') {
    return false;
  }
  size_t n = t.size(), i = 0;
  while (i < n && !std::isspace(static_cast<unsigned char>(t[i]))) {
    i++;
  }
  verb = t.substr(0, i);

  while (i < n) {
    while (i < n && std::isspace(static_cast<unsigned char>(t[i]))) {
      i++;
    }
    if (i >= n) {
      break;
    }
    size_t key_start = i;
    while (i < n && t[i] != '=' && !std::isspace(static_cast<unsigned char>(t[i]))) {
      i++;
    }
    std::string key = t.substr(key_start, i - key_start);
    std::string val;
    if (i < n && t[i] == '=') {
      i++;
      size_t val_start = i;
      while (i < n && !std::isspace(static_cast<unsigned char>(t[i]))) {
        i++;
      }
      val = t.substr(val_start, i - val_start);
    }
    args[key] = val;
  }
  return true;
}

const char *getArg(ArgMap &args, const char *key, const char *defv = nullptr)
{
  auto it = args.find(key);
  return it == args.end() ? defv : it->second.c_str();
}

int getInt(ArgMap &args, const char *key, int defv)
{
  const char *s = getArg(args, key);
  return s ? std::atoi(s) : defv;
}

float getFloat(ArgMap &args, const char *key, float defv)
{
  const char *s = getArg(args, key);
  return s ? float(std::atof(s)) : defv;
}

bool getBool(ArgMap &args, const char *key, bool defv)
{
  const char *s = getArg(args, key);
  if (!s) {
    return defv;
  }
  return s[0] == '1' || s[0] == 't' || s[0] == 'T' || s[0] == 'y' || s[0] == 'Y';
}

/* Parse "a,b,c" into ints; returns defv when the arg is absent/empty. */
std::vector<int> parseCsvInts(const char *s, std::vector<int> defv)
{
  if (!s || !s[0]) {
    return defv;
  }
  std::vector<int> out;
  for (const char *p = s; *p;) {
    out.push_back(std::atoi(p));
    while (*p && *p != ',') {
      p++;
    }
    if (*p == ',') {
      p++;
    }
  }
  return out.empty() ? defv : out;
}

bool parseFloat3(const char *s, float3 &out)
{
  if (!s) {
    return false;
  }
  float a = 0, b = 0, c = 0;
  if (std::sscanf(s, "%f,%f,%f", &a, &b, &c) != 3) {
    return false;
  }
  out = float3(a, b, c);
  return true;
}

std::string joinPath(const char *base, const char *rel)
{
  if (!rel) {
    return std::string();
  }
  if (!base || base[0] == 0) {
    return std::string(rel);
  }
  if (rel[0] == '/' || rel[0] == '\\' ||
      (std::strlen(rel) > 1 && rel[1] == ':')) {
    return std::string(rel);
  }
  std::string out(base);
  if (!out.empty()) {
    char last = out.back();
    if (last != '/' && last != '\\') {
      out += '/';
    }
  }
  out += rel;
  return out;
}

#ifdef SBRUSH_GPU_DISPATCH
// Execute a brush stroke on the GPU via the SPIR-V compute kernel. Thin driver
// over GpuStrokeSession (source/debug/gpu_stroke.{h,cc}): begin() uploads the
// mesh and loads the kernel resolved from scene.currentTool, each origin is one
// dab, and end() reads the result back and snapshots the touched nodes for undo.
// Geometry must match the C++ path bit-modulo-fp; that is what
// `make.mjs sbrush-verify` asserts via the <brush>_ab.txt A/B scripts.
// --gpu-capture writes a JSON fixture per stroke for the Dawn/WebGPU harness.
bool runBrushStrokeGPU(Scene &scene, const Vector<float3> &origins, float3 normal,
                       std::string &err)
{
  GpuStrokeSession session;
  if (!scene.gpuCapturePrefix.empty()) {
    session.enableCapture(scene.gpuCapturePrefix);
  }
  if (!session.begin(scene, err)) {
    return false;
  }
  for (size_t di = 0; di < origins.size(); di++) {
    if (!session.dab(scene, origins[di], normal, err)) {
      return false;
    }
  }
  session.end(scene);
  return true;
}
#endif // SBRUSH_GPU_DISPATCH

bool execVerb(Scene &scene,
              const std::string &verb,
              ArgMap &args,
              const char *out_dir,
              std::string &err)
{
  if (verb == "make_cube") {
    int dimen = getInt(args, "subdivs", 4);
    float size = getFloat(args, "size", 0.5f);
    float sphereFac = getFloat(args, "sphere", 0.0f);
    mesh::Mesh *m = mesh::createCube(dimen, size, sphereFac);
    scene.setMesh(m);
    return true;
  }
  if (verb == "build_spatial") {
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
    scene.brush.writeProps();
    return true;
  }
  if (verb == "set_backend") {
    const char *b = getArg(args, "backend");
    if (!b) {
      err = "set_backend: missing backend=";
      return false;
    }
    std::string bs = b;
    for (auto &c : bs) c = (char)std::tolower((unsigned char)c);
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
      err = "set_backend: WGSL backend not compiled in (configure with --backends=cpp,wgsl)";
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
      err = std::string("set_backend: unknown backend '") + b + "' (valid: cpp, wgsl, webgpu)";
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
    for (auto &c : ms) c = (char)std::tolower((unsigned char)c);
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
    for (auto &c : ts) c = (char)std::tolower((unsigned char)c);
    if (ts == "draw") {
      scene.currentTool = brush::SculptBrushes::DRAW;
    } else if (ts == "inflate") {
      scene.currentTool = brush::SculptBrushes::INFLATE;
    } else if (ts == "clay") {
      scene.currentTool = brush::SculptBrushes::CLAY;
    } else if (ts == "pinch") {
      scene.currentTool = brush::SculptBrushes::PINCH;
    } else if (ts == "sharp") {
      scene.currentTool = brush::SculptBrushes::SHARP;
    } else if (ts == "mask") {
      scene.currentTool = brush::SculptBrushes::MASK;
    } else if (ts == "smooth") {
      scene.currentTool = brush::SculptBrushes::SMOOTH;
    } else if (ts == "kelvinlet") {
      scene.currentTool = brush::SculptBrushes::KELVINLET;
    } else if (ts == "pose") {
      scene.currentTool = brush::SculptBrushes::POSE;
    } else if (ts == "texdraw") {
      scene.currentTool = brush::SculptBrushes::TEXDRAW;
    } else {
      err = std::string("set_brush_tool: unknown tool '") + t + "'";
      return false;
    }
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
      for (auto &c : ks) c = (char)std::tolower((unsigned char)c);
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
      for (auto &c : ss) c = (char)std::tolower((unsigned char)c);
      if (ss == "spherical") {
        scene.brush.falloff_shape = brush::FalloffShape::Spherical;
      } else if (ss == "cube") {
        scene.brush.falloff_shape = brush::FalloffShape::Cube;
      } else if (ss == "linear") {
        scene.brush.falloff_shape = brush::FalloffShape::Linear;
      } else if (ss == "box") {
        scene.brush.falloff_shape = brush::FalloffShape::Box;
      } else {
        err = std::string("set_falloff: unknown shape '") + sh +
              "' (valid: spherical, cube, linear, box)";
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
    return true;
  }
  if (verb == "set_falloff_curve") {
    const char *p = getArg(args, "preset");
    if (!p) {
      err = "set_falloff_curve: missing preset=smoothstep|linear|inverse|gaussian";
      return false;
    }
    std::string ps = p;
    for (auto &c : ps) c = (char)std::tolower((unsigned char)c);
    using CP = brush::Brush::CurvePreset;
    if      (ps == "smoothstep") scene.brush.setFalloffCurvePreset(CP::Smoothstep);
    else if (ps == "linear")     scene.brush.setFalloffCurvePreset(CP::Linear);
    else if (ps == "inverse")    scene.brush.setFalloffCurvePreset(CP::Inverse);
    else if (ps == "gaussian")   scene.brush.setFalloffCurvePreset(CP::Gaussian);
    else {
      err = std::string("set_falloff_curve: unknown preset '") + p +
            "' (valid: smoothstep, linear, inverse, gaussian)";
      return false;
    }
    return true;
  }
  if (verb == "set_texture") {
    // Bind a grayscale brush texture from one of three sources, checked in
    // priority order: image=<path> (decoded via stb_image to luminance),
    // proc=<name> (an analytic function baked onto the UV grid), or
    // pattern=<name> (the simple synthetic test patterns). `pattern=clear`
    // unbinds. The texel value varies across the surface so a golden test can
    // assert the displacement tracks UV (e.g. rampx → value grows with co.x
    // under the Global coord space).
    const char *imgPath = getArg(args, "image");
    const char *proc = getArg(args, "proc");
    const char *pat = getArg(args, "pattern", "rampx");

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
    for (auto &c : ps) c = (char)std::tolower((unsigned char)c);
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
      for (auto &c : procName) c = (char)std::tolower((unsigned char)c);
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
      for (auto &c : ss) c = (char)std::tolower((unsigned char)c);
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
    int n = std::sscanf(
        m, "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1],
        &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11],
        &v[12], &v[13], &v[14], &v[15]);
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
    if (muArg) scene.brush.mu = float(std::atof(muArg));
    if (nuArg) scene.brush.nu = float(std::atof(nuArg));
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
    if (verb == "set_pose_cage_rest") scene.brush.poseCageRest[idx] = pos;
    else                              scene.brush.poseCageNow[idx]  = pos;
    return true;
  }
  if (verb == "stroke") {
    if (!scene.mesh || !scene.tree) {
      err = "stroke: no mesh/tree";
      return false;
    }
    float3 origin, normal{0, 0, 1};
    if (!parseFloat3(getArg(args, "origin"), origin)) {
      err = "stroke: missing origin=x,y,z";
      return false;
    }
    parseFloat3(getArg(args, "normal"), normal);

#ifdef SBRUSH_GPU_DISPATCH
    // GPU dispatch covers the local per-vertex brushes, with or without a bound
    // brush texture (sampled in-shader to match the C++ bilinear). Tools not
    // listed here fall back to the C++ executor below; the set must stay in
    // sync with runBrushStrokeGPU's kernel switch.
    brush::SculptBrushes t = scene.currentTool;
    bool gpuTool = t == brush::SculptBrushes::DRAW ||
                   t == brush::SculptBrushes::TEXDRAW ||
                   t == brush::SculptBrushes::CLAY ||
                   t == brush::SculptBrushes::INFLATE ||
                   t == brush::SculptBrushes::PINCH ||
                   t == brush::SculptBrushes::SHARP ||
                   t == brush::SculptBrushes::MASK ||
                   t == brush::SculptBrushes::SMOOTH ||
                   t == brush::SculptBrushes::KELVINLET ||
                   t == brush::SculptBrushes::POSE;
    if ((scene.currentBackend == BrushBackend::Wgsl ||
         scene.currentBackend == BrushBackend::WgpuNative) &&
        gpuTool) {
      Vector<float3> origins;
      origins.append(origin);
      if (!runBrushStrokeGPU(scene, origins, normal, err)) {
        return false;
      }
    } else
#endif
    {
      Vector<spatial::SpatialNode *> nodes;
      scene.tree->filterNodes(origin, scene.brush.radius, nodes);
      if (nodes.size() != 0) {
        brush::CommandExecutor exec(scene.tree, &scene.brush);
        exec.meshLog = &scene.meshLog;
        exec.ctx.renderMatrix = scene.renderMatrix;
        if (scene.useCsrNeighbors) {
          exec.neighborMode = brush::CommandExecutor::NeighborMode::Csr;
        }
        exec.beginStep();
        exec.execBrush(scene.currentTool, &nodes, origin, normal);
        exec.endStep();
      }
    }

    scene.lastStroke.valid = true;
    scene.lastStroke.origin = origin;
    scene.lastStroke.normal = normal;
    scene.lastStroke.radius = scene.brush.radius;
    return true;
  }
  if (verb == "stroke_path") {
    if (!scene.mesh || !scene.tree) {
      err = "stroke_path: no mesh/tree";
      return false;
    }
    float3 p1, p2, normal{0, 0, 1};
    if (!parseFloat3(getArg(args, "p1"), p1) ||
        !parseFloat3(getArg(args, "p2"), p2)) {
      err = "stroke_path: missing p1/p2";
      return false;
    }
    parseFloat3(getArg(args, "normal"), normal);

    // Collect dab origins first so both backends drive the identical sequence.
    Vector<float3> origins;
    const char *spacingArg = getArg(args, "spacing");
    if (spacingArg) {
      /* spacing= overrides fixed-step mode: emit dabs every
       * radius * spacing world-space units along the segment. */
      float spacingFrac = float(std::atof(spacingArg));
      brush::StrokeSpacer spacer;
      spacer.spacing = scene.brush.radius * spacingFrac;
      auto collect = [&](float3 o) { origins.append(o); };
      spacer.advance(p1, collect);
      spacer.advance(p2, collect);
    } else {
      int steps = getInt(args, "steps", 8);
      if (steps < 1) {
        steps = 1;
      }
      for (int i = 0; i < steps; i++) {
        float t = (steps == 1) ? 0.0f : float(i) / float(steps - 1);
        origins.append(p1 * (1.0f - t) + p2 * t);
      }
    }

#ifdef SBRUSH_GPU_DISPATCH
    // GPU dispatch covers the local per-vertex brushes, with or without a bound
    // brush texture (sampled in-shader to match the C++ bilinear). Tools not
    // listed here fall back to the C++ executor below; the set must stay in
    // sync with runBrushStrokeGPU's kernel switch.
    brush::SculptBrushes t = scene.currentTool;
    bool gpuTool = t == brush::SculptBrushes::DRAW ||
                   t == brush::SculptBrushes::TEXDRAW ||
                   t == brush::SculptBrushes::CLAY ||
                   t == brush::SculptBrushes::INFLATE ||
                   t == brush::SculptBrushes::PINCH ||
                   t == brush::SculptBrushes::SHARP ||
                   t == brush::SculptBrushes::MASK ||
                   t == brush::SculptBrushes::SMOOTH ||
                   t == brush::SculptBrushes::KELVINLET ||
                   t == brush::SculptBrushes::POSE;
    if ((scene.currentBackend == BrushBackend::Wgsl ||
         scene.currentBackend == BrushBackend::WgpuNative) &&
        gpuTool) {
      if (!runBrushStrokeGPU(scene, origins, normal, err)) {
        return false;
      }
    } else
#endif
    {
      brush::CommandExecutor exec(scene.tree, &scene.brush);
      exec.meshLog = &scene.meshLog;
      exec.ctx.renderMatrix = scene.renderMatrix;
      exec.beginStep();
      for (size_t i = 0; i < origins.size(); i++) {
        Vector<spatial::SpatialNode *> nodes;
        scene.tree->filterNodes(origins[i], scene.brush.radius, nodes);
        if (nodes.size() == 0) {
          continue;
        }
        exec.execBrush(scene.currentTool, &nodes, origins[i], normal);
        exec.clearIsFirstOfStep();
      }
      exec.endStep();
    }

    scene.lastStroke.valid = true;
    scene.lastStroke.origin = p2;
    scene.lastStroke.normal = normal;
    scene.lastStroke.radius = scene.brush.radius;
    return true;
  }
  if (verb == "view") {
    const char *v = getArg(args, "preset", "persp");
    ViewPreset p = ViewPreset::Persp;
    if (std::strcmp(v, "front") == 0) p = ViewPreset::Front;
    else if (std::strcmp(v, "top") == 0) p = ViewPreset::Top;
    else if (std::strcmp(v, "side") == 0) p = ViewPreset::Side;
    else if (std::strcmp(v, "persp") == 0) p = ViewPreset::Persp;
    else if (std::strcmp(v, "free") == 0) p = ViewPreset::Free;
    scene.applyView(p);
    return true;
  }
  if (verb == "screenshot") {
    const char *v = getArg(args, "view");
    if (v) {
      ArgMap sub;
      sub["preset"] = v;
      std::string subErr;
      execVerb(scene, "view", sub, out_dir, subErr);
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
  if (verb == "assert_verts") {
    int n = getInt(args, "n", -1);
    int actual = scene.mesh ? scene.mesh->v.count : 0;
    if (n != actual) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "assert_verts: expected %d got %d", n, actual);
      err = buf;
      return false;
    }
    return true;
  }
  if (verb == "assert_aabb") {
    if (!scene.mesh) {
      err = "assert_aabb: no mesh";
      return false;
    }
    float3 emn, emx;
    if (!parseFloat3(getArg(args, "min"), emn) ||
        !parseFloat3(getArg(args, "max"), emx)) {
      err = "assert_aabb: need min=x,y,z max=x,y,z";
      return false;
    }
    float eps = getFloat(args, "eps", 1e-4f);
    float3 amn, amx;
    scene.mesh->calcAABB(amn, amx);
    for (int i = 0; i < 3; i++) {
      if (std::fabs(amn[i] - emn[i]) > eps || std::fabs(amx[i] - emx[i]) > eps) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "assert_aabb mismatch: got [%g,%g,%g]..[%g,%g,%g]",
                      amn[0], amn[1], amn[2], amx[0], amx[1], amx[2]);
        err = buf;
        return false;
      }
    }
    return true;
  }
  if (verb == "undo") {
    if (scene.mesh && scene.tree) {
      scene.meshLog.undo(scene.mesh, scene.tree);
    }
    return true;
  }
  if (verb == "redo") {
    if (scene.mesh && scene.tree) {
      scene.meshLog.redo(scene.mesh, scene.tree);
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
  if (verb == "bench_spatial") {
    /* Sweep (leaf_limit x gpu_tri_target) on the current mesh and report the
     * cost curves that drive the two tunables:
     *   build_ms   — tree build time (depends on leaf_limit; gpu_tri irrelevant)
     *   leaves     — leaf count (query granularity)
     *   gpunodes   — GPU node count == draw-call count (depends on gpu_tri_target)
     *   filter_us  — avg filterNodes() time per query
     *   wset_v     — avg verts in the brush working set (sum of hit leaves'
     *                unique_verts) — the brush kernel touches all of these
     *   inr_v      — avg verts actually within the brush radius
     *   waste      — wset_v / inr_v: culling tightness (1.0 = perfect; lower
     *                leaf_limit -> tighter -> less wasted brush work)
     * Pure query benchmark — does not sculpt, so the mesh stays pristine and
     * every config is measured against identical geometry. */
    if (!scene.mesh) {
      err = "bench_spatial: no mesh (run make_cube first)";
      return false;
    }
    std::vector<int> leafs =
        parseCsvInts(getArg(args, "leafs"), {64, 128, 256, 512, 1024});
    std::vector<int> gputris =
        parseCsvInts(getArg(args, "gputris"), {512, 2048, 8192, 32768});
    int depth = getInt(args, "depth", 22);
    int dabs = getInt(args, "dabs", 16);
    float radius = getFloat(args, "radius", scene.brush.radius);
    if (radius <= 0.0f) {
      radius = 0.25f;
    }

    int vc = scene.mesh->v.count;
    if (vc <= 0 || dabs <= 0) {
      err = "bench_spatial: empty mesh or dabs<1";
      return false;
    }
    /* Sample dab origins from verts spread across the index range. */
    Vector<float3> origins;
    for (int k = 0; k < dabs; k++) {
      int idx = int((long long)k * vc / dabs);
      if (idx >= vc) {
        idx = vc - 1;
      }
      origins.append(scene.mesh->v.co[idx]);
    }
    float r2 = radius * radius;

    std::printf("[bench] mesh verts=%d faces=%d  radius=%.4f dabs=%d depth=%d\n",
                scene.mesh->v.count, scene.mesh->f.count, radius, dabs, depth);
    std::printf("[bench] %-6s %-7s | %9s %7s %8s | %10s %9s %9s %6s\n", "leaf",
                "gputri", "build_ms", "leaves", "gpunodes", "filter_us",
                "wset_v", "inr_v", "waste");

    for (int leaf : leafs) {
      for (int gt : gputris) {
        auto t0 = std::chrono::steady_clock::now();
        scene.buildSpatial(leaf, depth, gt);
        double build_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();

        int leafCount = int(scene.tree->leaves().size());
        /* buildAll doesn't assign GPU nodes (update() does); drive it here. */
        scene.tree->recompute_subtree_tri_counts();
        scene.tree->assign_gpu_nodes();
        int gpuNodeCount = int(scene.tree->gpu_nodes().size());

        long wset = 0, inr = 0;
        auto q0 = std::chrono::steady_clock::now();
        for (const float3 &o : origins) {
          Vector<spatial::SpatialNode *> hit;
          scene.tree->filterNodes(o, radius, hit);
          for (spatial::SpatialNode *nd : hit) {
            for (int v : nd->unique_verts()) {
              wset++;
              float3 d = scene.mesh->v.co[v] - o;
              if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= r2) {
                inr++;
              }
            }
          }
        }
        double filter_us = std::chrono::duration<double, std::micro>(
                               std::chrono::steady_clock::now() - q0)
                               .count() /
                           dabs;
        double waste = inr > 0 ? double(wset) / double(inr) : 0.0;
        std::printf("[bench] %-6d %-7d | %9.2f %7d %8d | %10.1f %9ld %9ld %6.2f\n",
                    leaf, gt, build_ms, leafCount, gpuNodeCount, filter_us,
                    wset / dabs, inr / dabs, waste);
        std::fflush(stdout);
      }
    }
    return true;
  }

  err = "unknown verb: " + verb;
  return false;
}

} // namespace

RunResult run(Scene &scene, const char *source, const char *out_dir)
{
  RunResult r;
  if (!source) {
    r.ok = false;
    r.error = "null source";
    return r;
  }
  const char *p = source;
  int line_no = 0;
  while (*p) {
    line_no++;
    const char *eol = std::strchr(p, '\n');
    size_t len = eol ? size_t(eol - p) : std::strlen(p);
    std::string line(p, len);

    std::string verb;
    ArgMap args;
    if (parseLine(line, verb, args)) {
      std::string err;
      if (!execVerb(scene, verb, args, out_dir, err)) {
        r.ok = false;
        r.line_no = line_no;
        r.error = err.c_str();
        return r;
      }
    }

    if (!eol) {
      break;
    }
    p = eol + 1;
  }
  return r;
}

RunResult runFile(Scene &scene, const char *path, const char *out_dir)
{
  RunResult r;
  std::FILE *f = std::fopen(path, "rb");
  if (!f) {
    r.ok = false;
    std::string e = std::string("cannot open script: ") + path;
    r.error = e.c_str();
    return r;
  }
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::string buf;
  if (n > 0) {
    buf.resize(size_t(n));
    size_t got = std::fread(buf.data(), 1, size_t(n), f);
    buf.resize(got);
  }
  std::fclose(f);
  return run(scene, buf.c_str(), out_dir);
}

} // namespace sculptcore::debug_app::script
