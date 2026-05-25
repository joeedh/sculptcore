#include "script.h"

#include "scene.h"
#include "state_dump.h"

#include "brush/brush_executor.h"
#include "brush/stroke_spacing.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"
#include "mesh/mesh_shapes.h"
#include "spatial/spatial.h"

#ifdef SBRUSH_GPU_DISPATCH
#include "mesh/mesh_iter.h"
#include "spatial/node.h"
#include "spatial/spatial_enums.h"
#include "vulkan/vk_compute.h"
#include "vulkan/vk_context.h"
#endif

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

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
// Execute a brush stroke on the GPU via the SPIR-V compute kernel. Marshals the
// full mesh co/no/mask once, dispatches one ≤64-vert workgroup per node-chunk
// per dab (reading the previous dab's result, like the C++ executor), reads co
// back, and snapshots the touched nodes into the meshlog for undo. Geometry
// must match the C++ path bit-modulo-fp; that is what `make.mjs sbrush-verify`
// asserts via the <brush>_ab.txt A/B scripts. Supports the local brushes wired
// below (DRAW, CLAY, SMOOTH), including a bound brush texture (uploaded to
// binding 8, sampled in-shader with the same bilinear as the C++ path); SMOOTH
// uploads a CSR neighbor topology so its for_neighbor kernel can read the
// Jacobi snapshot.
bool runBrushStrokeGPU(Scene &scene, const Vector<float3> &origins, float3 normal,
                       std::string &err)
{
  using litestl::math::float3;
  if (!scene.ensureGPU() || !scene.context) {
    err = "stroke(wgsl): GPU device init failed";
    return false;
  }

  const char *kernel = nullptr;
  bool needsNeighbors = false;
  bool writesMask = false;  // Mask paints mask_buf; everyone else leaves it as-is.
  switch (scene.currentTool) {
  case brush::SculptBrushes::DRAW: kernel = "draw"; break;
  case brush::SculptBrushes::CLAY: kernel = "clay"; break;
  case brush::SculptBrushes::INFLATE: kernel = "inflate"; break;
  case brush::SculptBrushes::PINCH: kernel = "pinch"; break;
  case brush::SculptBrushes::SHARP: kernel = "sharp"; break;
  case brush::SculptBrushes::MASK: kernel = "mask"; writesMask = true; break;
  case brush::SculptBrushes::SMOOTH: kernel = "smooth"; needsNeighbors = true; break;
  default:
    err = "stroke(wgsl): tool has no GPU kernel";
    return false;
  }

  mesh::Mesh *m = scene.mesh;
  const int vcount = m->v.count;

  Vector<float> co, no, mask;
  co.resize(size_t(vcount) * 3);
  no.resize(size_t(vcount) * 3);
  mask.resize(size_t(vcount));
  for (int i = 0; i < vcount; i++) {
    float3 c = m->v.co[i], n = m->v.no[i];
    co[i * 3 + 0] = c[0]; co[i * 3 + 1] = c[1]; co[i * 3 + 2] = c[2];
    no[i * 3 + 0] = n[0]; no[i * 3 + 1] = n[1]; no[i * 3 + 2] = n[2];
    mask[i] = scene.tree->treeMesh.v.mask[i];
  }

  vulkan::BrushComputeDispatch disp(scene.context);
  std::string spv = std::string(SBRUSH_SPV_DIR) + "/" + kernel + ".spv";
  if (!disp.loadSpirv(spv.c_str())) {
    err = "stroke(wgsl): failed to load " + spv;
    return false;
  }
  if (!disp.beginStroke(co.data(), no.data(), mask.data(), vcount)) {
    err = "stroke(wgsl): vertex upload failed";
    return false;
  }

  // CSR neighbor topology for for_neighbor kernels. Build it in the same
  // EdgeOfVertIter order the C++ kernel walks so the per-vertex `avg += nb.co`
  // accumulates identically — keeping the GPU result bit-modulo-fp identical.
  if (needsNeighbors) {
    Vector<vulkan::ComputeVertNbr> meta;
    Vector<uint32_t> flat;
    meta.resize(vcount);
    for (int v = 0; v < vcount; v++) {
      uint32_t off = uint32_t(flat.size());
      uint32_t cnt = 0;
      int e0 = m->v.e[v];
      if (e0 != ELEM_NONE) {
        for (int e : mesh::EdgeOfVertIter(m, v, e0)) {
          int nb = (m->e.vs[e][0] == v) ? m->e.vs[e][1] : m->e.vs[e][0];
          flat.append(uint32_t(nb));
          cnt++;
        }
      }
      meta[v].offset = off;
      meta[v].count = cnt;
    }
    if (!disp.setNeighbors(meta.data(), vcount, flat.data(), int(flat.size()))) {
      err = "stroke(wgsl): neighbor upload failed";
      return false;
    }
  }

  // Brush texture (binding 8). The kernel multiplies strength by
  // sampleBrushTex; with no texture bound the dummy 1x1 white returns 1.0, so
  // only upload when one is set. coord_space/tex_repeat ride in on the per-dab
  // uniforms below.
  if (scene.brush.tex_width > 0 && scene.brush.tex_height > 0 &&
      scene.brush.tex_pixels.size() > 0) {
    if (!disp.setBrushTexture(scene.brush.tex_pixels.data(),
                              scene.brush.tex_width, scene.brush.tex_height)) {
      err = "stroke(wgsl): brush texture upload failed";
      return false;
    }
  }

  scene.meshLog.beginStep();
  scene.brush.resetStrokePath();

  Vector<spatial::SpatialNode *> touched;
  for (size_t di = 0; di < origins.size(); di++) {
    float3 origin = origins[di];
    Vector<spatial::SpatialNode *> nodes;
    scene.tree->filterNodes(origin, scene.brush.radius, nodes);
    if (nodes.size() == 0) {
      continue;
    }
    scene.brush.pushStrokeSample(origin, normal);

    Vector<uint32_t> uverts;
    Vector<vulkan::ComputeNodeMeta> chunks;
    for (auto *node : nodes) {
      auto &uv = node->unique_verts();
      Vector<int> idx;
      for (int gi : uv) {
        idx.append(gi);
      }
      int n = int(idx.size());
      for (int written = 0; written < n; written += 64) {
        int cnt = (n - written < 64) ? (n - written) : 64;
        vulkan::ComputeNodeMeta meta;
        meta.vert_offset = uint32_t(uverts.size());
        meta.vert_count = uint32_t(cnt);
        for (int k = 0; k < cnt; k++) {
          uverts.append(uint32_t(idx[written + k]));
        }
        chunks.append(meta);
      }
      touched.append(node);
    }

    vulkan::ComputeBrushUniforms bu;
    bu.strength = scene.brush.strength;
    bu.radius = scene.brush.radius;
    bu.spacing = scene.brush.spacing;
    bu.invert = scene.brush.invert ? 1u : 0u;
    bu.falloff_kind = uint32_t(scene.brush.falloff_kind);
    bu.falloff_shape = uint32_t(scene.brush.falloff_shape);
    bu.falloff_dir[0] = scene.brush.falloff_dir[0];
    bu.falloff_dir[1] = scene.brush.falloff_dir[1];
    bu.falloff_dir[2] = scene.brush.falloff_dir[2];
    bu.coord_space = uint32_t(scene.brush.coord_space);
    bu.tex_repeat = scene.brush.tex_repeat;
    bu.stroke_path_count = uint32_t(scene.brush.strokePathCount);

    vulkan::ComputeCtxUniforms cu;
    cu.surfacePos[0] = origin[0]; cu.surfacePos[1] = origin[1]; cu.surfacePos[2] = origin[2];
    cu.surfaceNo[0] = normal[0]; cu.surfaceNo[1] = normal[1]; cu.surfaceNo[2] = normal[2];
    // VIEWPLANE/VIEWREPEAT sample brush_tex in render_matrix space; Global/
    // StrokeCurved ignore it. litestl::math::Matrix::operator*(vec) consumes its
    // backing buffer row-major (result[i] = dot(row_i, v) + row_i[3]), but WGSL
    // mat4x4<f32> / std140 storage is column-major — so transpose on the way out
    // or the two backends read different matrices.
    {
      const float *rm = scene.renderMatrix;  // row-major
      for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
          cu.render_matrix[c * 4 + r] = rm[r * 4 + c];
        }
      }
    }

    Vector<vulkan::ComputeStrokeSample> sp;
    for (int i = 0; i < scene.brush.strokePathCount; i++) {
      const auto &s = scene.brush.strokePath[i];
      vulkan::ComputeStrokeSample o;
      o.pos[0] = s.pos[0]; o.pos[1] = s.pos[1]; o.pos[2] = s.pos[2];
      o.normal[0] = s.normal[0]; o.normal[1] = s.normal[1]; o.normal[2] = s.normal[2];
      o.arclen = s.arclen;
      sp.append(o);
    }

    if (!disp.dab(bu, cu, uverts.data(), int(uverts.size()), chunks.data(),
                  int(chunks.size()), scene.brush.falloff_curve.data(), sp.data(),
                  int(sp.size()))) {
      err = "stroke(wgsl): compute dispatch failed";
      return false;
    }
  }

  Vector<float> coOut, maskOut;
  coOut.resize(size_t(vcount) * 3);
  if (writesMask) {
    maskOut.resize(size_t(vcount));
  }
  disp.endStroke(coOut.data(), nullptr, writesMask ? maskOut.data() : nullptr);

  // Snapshot pre-stroke node state for undo (mesh.v.co is still pre-stroke
  // here), mirroring the emitted `*Pre` stage, then write the GPU result.
  for (auto *node : touched) {
    if (scene.meshLog.hasSimpleChunk(node->id)) {
      continue;
    }
    auto *simple = scene.meshLog.getSimpleChunk(
        node->id, node->unique_verts().size(), 0, 0, node->unique_faces().size());
    auto *mm = node->data->m;
    simple->v.ensureAttr(mm->v.attrs, mm->v.co);
    simple->v.ensureAttr(mm->v.attrs, mm->v.no);
    simple->f.ensureAttr(mm->f.attrs, mm->f.no);
    simple->v.cpyFrom(mm->v.attrs, node->unique_verts());
    simple->f.cpyFrom(mm->f.attrs, node->unique_faces());
  }

  for (int i = 0; i < vcount; i++) {
    m->v.co[i] = float3(coOut[i * 3 + 0], coOut[i * 3 + 1], coOut[i * 3 + 2]);
  }
  // Mask paints the .spatial.v.mask attribute, not geometry; write it back to
  // the tree mesh. (Matches the C++ Mask brush, whose *Pre snapshots co/no but
  // not mask, so neither path restores mask on undo.)
  if (writesMask) {
    for (int i = 0; i < vcount; i++) {
      scene.tree->treeMesh.v.mask[i] = maskOut[i];
    }
  }
  for (auto *node : touched) {
    node->update(spatial::Spatial_UpdateNormals | spatial::Spatial_UpdateGPU |
                 spatial::Spatial_RegenBounds);
  }

  scene.meshLog.endStep();
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
    int leaf = getInt(args, "leaf_limit", 512);
    int depth = getInt(args, "depth_limit", 16);
    int gpu_tri_target = getInt(args, "gpu_tri_target", 2048);
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
    } else {
      err = std::string("set_backend: unknown backend '") + b + "' (valid: cpp, wgsl)";
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
      } else {
        err = std::string("set_falloff: unknown shape '") + sh +
              "' (valid: spherical, cube, linear)";
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
    // Build a synthetic grayscale brush texture. `pattern=clear` unbinds.
    // Patterns vary the texel value so a golden test can assert the
    // displacement tracks UV (e.g. rampx → value grows with co.x under
    // the Global coord space).
    const char *pat = getArg(args, "pattern", "rampx");
    int w = getInt(args, "width", 64);
    int h = getInt(args, "height", 64);
    std::string ps = pat;
    for (auto &c : ps) c = (char)std::tolower((unsigned char)c);
    if (ps == "clear") {
      scene.brush.tex_width = 0;
      scene.brush.tex_height = 0;
      scene.brush.tex_pixels.clear();
      return true;
    }
    if (w <= 0 || h <= 0) {
      err = "set_texture: width/height must be positive";
      return false;
    }
    scene.brush.tex_width = w;
    scene.brush.tex_height = h;
    scene.brush.tex_pixels.resize(w * h);
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        float u = w > 1 ? (float)x / (float)(w - 1) : 0.0f;
        float v = h > 1 ? (float)y / (float)(h - 1) : 0.0f;
        float val;
        if (ps == "rampx") {
          val = u;
        } else if (ps == "rampy") {
          val = v;
        } else if (ps == "checker") {
          val = ((x ^ y) & 1) ? 1.0f : 0.0f;
        } else if (ps == "constant") {
          val = 1.0f;
        } else {
          err = std::string("set_texture: unknown pattern '") + pat +
                "' (valid: rampx, rampy, checker, constant, clear)";
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
      } else {
        err = std::string("set_coord_space: unknown space '") + sp +
              "' (valid: global, viewplane, viewrepeat, stroke_curved)";
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
                   t == brush::SculptBrushes::CLAY ||
                   t == brush::SculptBrushes::INFLATE ||
                   t == brush::SculptBrushes::PINCH ||
                   t == brush::SculptBrushes::SHARP ||
                   t == brush::SculptBrushes::MASK ||
                   t == brush::SculptBrushes::SMOOTH;
    if (scene.currentBackend == BrushBackend::Wgsl && gpuTool) {
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
                   t == brush::SculptBrushes::CLAY ||
                   t == brush::SculptBrushes::INFLATE ||
                   t == brush::SculptBrushes::PINCH ||
                   t == brush::SculptBrushes::SHARP ||
                   t == brush::SculptBrushes::MASK ||
                   t == brush::SculptBrushes::SMOOTH;
    if (scene.currentBackend == BrushBackend::Wgsl && gpuTool) {
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
