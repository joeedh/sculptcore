#include "script.h"

#include "roughness.h"
#include "scene.h"
#include "state_dump.h"

#include "brush/brush_executor.h"
#include "brush/stroke_driver.h"
#include "brush/stroke_spacing.h"
#include "displace/compositor.h"
#include "displace/frames.h"
#include "vdm/vdm_promote.h"
#include "vdm/vdm_splat.h"
#include "vdm/vdm_undo.h"
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
#include "brush/grid_executor.h"
#include "brush/grid_gpu_session.h"
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
#include "gpu_stroke.h"
#endif

#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <fstream>
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

bool parseFloat2(const char *s, litestl::math::float2 &out)
{
  if (!s) {
    return false;
  }
  float a = 0, b = 0;
  if (std::sscanf(s, "%f,%f", &a, &b) != 2) {
    return false;
  }
  out = litestl::math::float2(a, b);
  return true;
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

/* Partition-independent consistency check of the incrementally-maintained
 * spatial tree against the live mesh. Returns true (consistent) or false with
 * @p msg naming the FIRST divergence (kind, index, owner id, invariant). Used
 * to localize the dyntopo undo/redo bug without a masking rebuild — see
 * documentation/plans/diff-tree-check.md. */
bool firstTreeDivergence(spatial::SpatialTree *tree, mesh::Mesh *m, std::string &msg)
{
  auto &fnode = tree->treeMesh.f.node;
  auto &vnode = tree->treeMesh.v.node;
  char buf[256];

  /* 1. Stale-owned: a dead (freed) element still owned by a leaf. Leading hang
   *    suspect — a killed face/vert whose slot is reused while the tree still
   *    owns the old occupant. */
  for (int f = 0; f < int(m->f.capacity()); f++) {
    if (m->f.freemap[f] && fnode[f] != 0) {
      std::snprintf(buf, sizeof(buf),
                    "dead face %d still owned by leaf %d", f, fnode[f]);
      msg = buf;
      return false;
    }
  }
  for (int v = 0; v < int(m->v.capacity()); v++) {
    if (m->v.freemap[v] && vnode[v] != 0) {
      std::snprintf(buf, sizeof(buf),
                    "dead vert %d still owned by leaf %d", v, vnode[v]);
      msg = buf;
      return false;
    }
  }

  /* 2. Dangling owner id: an owned element whose owner id resolves to no live
   *    leaf with data. */
  for (int f : m->f) {
    int id = fnode[f];
    if (id == 0) {
      continue; /* unowned handled in coverage */
    }
    spatial::SpatialNode *n = tree->node_from_id(id);
    if (!n || !n->data) {
      std::snprintf(buf, sizeof(buf),
                    "live face %d owner id %d resolves to no live leaf", f, id);
      msg = buf;
      return false;
    }
  }
  for (int v : m->v) {
    int id = vnode[v];
    if (id == 0) {
      continue;
    }
    spatial::SpatialNode *n = tree->node_from_id(id);
    if (!n || !n->data) {
      std::snprintf(buf, sizeof(buf),
                    "live vert %d owner id %d resolves to no live leaf", v, id);
      msg = buf;
      return false;
    }
  }

  /* 3. Leaf set <-> array agreement: every element a leaf claims must be live
   *    and point its owner array back at that leaf. */
  auto leaves = tree->leaves();
  for (auto *leaf : leaves) {
    if (!leaf->data) {
      continue;
    }
    for (int f : leaf->data->unique_faces) {
      if (m->f.freemap[f]) {
        std::snprintf(buf, sizeof(buf), "leaf %d claims dead face %d", leaf->id, f);
        msg = buf;
        return false;
      }
      if (fnode[f] != leaf->id) {
        std::snprintf(buf, sizeof(buf),
                      "leaf %d claims face %d but owner array says %d",
                      leaf->id, f, fnode[f]);
        msg = buf;
        return false;
      }
    }
    for (int v : leaf->data->unique_verts) {
      if (m->v.freemap[v]) {
        std::snprintf(buf, sizeof(buf), "leaf %d claims dead vert %d", leaf->id, v);
        msg = buf;
        return false;
      }
      if (vnode[v] != leaf->id) {
        std::snprintf(buf, sizeof(buf),
                      "leaf %d claims vert %d but owner array says %d",
                      leaf->id, v, vnode[v]);
        msg = buf;
        return false;
      }
    }
  }

  /* 4. Coverage: every live element is owned, and per-leaf set totals match the
   *    mesh's live counts (no dropped/duplicated elements). */
  for (int f : m->f) {
    if (fnode[f] == 0) {
      std::snprintf(buf, sizeof(buf), "live face %d unowned (dropped)", f);
      msg = buf;
      return false;
    }
  }
  for (int v : m->v) {
    if (vnode[v] == 0) {
      std::snprintf(buf, sizeof(buf), "live vert %d unowned (dropped)", v);
      msg = buf;
      return false;
    }
  }
  int ownedF = 0, ownedV = 0;
  for (auto *leaf : leaves) {
    if (!leaf->data) {
      continue;
    }
    ownedF += int(leaf->data->unique_faces.size());
    ownedV += int(leaf->data->unique_verts.size());
  }
  if (ownedF != m->f.count) {
    std::snprintf(buf, sizeof(buf),
                  "leaf faces sum to %d but mesh has %d live", ownedF, m->f.count);
    msg = buf;
    return false;
  }
  if (ownedV != m->v.count) {
    std::snprintf(buf, sizeof(buf),
                  "leaf verts sum to %d but mesh has %d live", ownedV, m->v.count);
    msg = buf;
    return false;
  }
  return true;
}

/* Check the incremental tree after an undo/redo step. On a clean step does NOT
 * rebuild (the tree persists and cascades, the faithful Electron repro). Only
 * on the first divergence runs a throwaway rebuild() as a positive-control
 * oracle: if the rebuilt tree is consistent the bug is in the incremental
 * undo/redo path; if it also diverges the fault is mesh-level. */
bool checkTreeVsRebuild(Scene &scene, const char *tag, std::string &err)
{
  std::string incMsg;
  if (firstTreeDivergence(scene.tree, scene.mesh, incMsg)) {
    return true; /* clean — no rebuild, let the incremental tree persist */
  }
  scene.tree->rebuild();
  std::string rebMsg;
  bool rebOk = firstTreeDivergence(scene.tree, scene.mesh, rebMsg);
  err = "incremental tree diverged after " + std::string(tag) + ": " + incMsg +
        " | rebuild oracle: " +
        (rebOk ? "CONSISTENT -> bug is in incremental undo/redo"
               : "ALSO INCONSISTENT: " + rebMsg + " -> mesh-level");
  return false;
}

/* Vertex-position snapshots for undo-fidelity checks (save_pos / assert_pos).
 * Keyed by name; mesh indices are persistent ids (IDMap is disabled), so a
 * vertex restored by undo lands back at the same index. */
std::map<std::string, std::vector<std::pair<int, float3>>> g_posSnapshots;

/** Vertex-group weight snapshots (save_weights / assert_weights), the same
 * undo-fidelity shape as g_posSnapshots but carrying each vert's whole run:
 * the pool interns by value, so a slot index is not comparable across a
 * sweep. */
std::map<std::string, std::vector<std::pair<int, std::vector<mesh::DeformWeight>>>>
    g_weightSnapshots;

/* Grids-store displacement snapshots (save_disp / assert_disp): one level's
 * disp channel flattened in (grid, v, u, component) order. The S4 ride-along
 * gate compares these bitwise. */
std::map<std::string, std::vector<float>> g_dispSnapshots;

/* Flatten a level's disp channel (deterministic order) for the verbs above. */
static void gatherDisp(subdiv::Multires &mr, int level, std::vector<float> &out)
{
  out.clear();
  int w = subdiv::GridsStore::sideForLevel(level) + 1;
  for (int g = 0; g < mr.store.gridCount(); g++) {
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        const float *d = mr.store.elem(level, 0, g, u, v);
        out.push_back(d[0]);
        out.push_back(d[1]);
        out.push_back(d[2]);
      }
    }
  }
}

// SBRUSH_WGSL_DIR arrives unquoted from CMake (see gpu_stroke.cc's twin).
#define SBRUSH_SCRIPT_STRINGIZE_(x) #x
#define SBRUSH_SCRIPT_STRINGIZE(x) SBRUSH_SCRIPT_STRINGIZE_(x)

/* Grids-native stroke session for the grid_* verbs, owned by the Scene
 * (torn down with the multires stack). Rebuilt when the level changes;
 * re-synced to the current domain per use. */
static bool ensureGridSession(Scene &scene, int level, std::string &err)
{
  if (!scene.multires) {
    err = "grid verbs: multires not active (run multires_init)";
    return false;
  }
  if (level < 1 || level > scene.multires->maxLevel()) {
    err = "grid verbs: bad level";
    return false;
  }
  if (scene.gridExec && scene.gridLevel != level) {
    scene.clearGridSession();
  }
  if (!scene.gridExec) {
    scene.gridLevel = level;
    scene.gridLog = litestl::alloc::New<subdiv::GridStrokeLog>("grid verb log");
    scene.gridExec = litestl::alloc::New<brush::GridBrushExecutor>(
        "grid verb exec", scene.multires->gridDomain(level), &scene.brush,
        scene.gridLog);
  } else {
    // Fold points (mesh-path writeback, level ops) drop the domain — re-bind.
    subdiv::GridLevelDomain *d = scene.multires->gridDomain(level);
    if (d != scene.gridExec->domain) {
      scene.gridExec->attach(d);
    }
  }
  return true;
}

/* Interim ride-along mirror — the shared brush::gridsMirrorToSlot helper. */
static void gridMirrorSync(Scene &scene, int level, litestl::util::Vector<int> &verts)
{
  brush::gridsMirrorToSlot(scene.multires, level,
                           std::span<const int>(verts.data(), verts.size()));
}

/* Stroke-verb epilogue when multires is active: fold the stroke's positions
 * into the store (frame-relative deltas) and record the level for the
 * level-aware undo/redo verbs. */
static void multiresStrokeEnd(Scene &scene)
{
  if (!scene.multires) {
    return;
  }
  int level = scene.multires->activeLevel();
  int changed = scene.multires->writeback(level);
  scene.mrUndoLevels.append(level);
  scene.mrRedoLevels.clear();
  std::fprintf(stdout, "[script] multires writeback level=%d changed=%d\n", level,
               changed);
}
/** Print the brush-noise metrics over the region swept by `centers`/`radius`
 * (plan 2026-07-26-0909 §9.1): one-ring normal roughness of the live surface
 * *and* of the derived displacement base, plus the live-only fidelity guard so
 * a quieter base can't be won by depositing less displacement. */
static void reportRoughness(Scene &scene,
                            const char *tag,
                            const Vector<float3> &centers,
                            float radius,
                            float3 up,
                            float rest)
{
  Vector<int> region;
  collectRegion(scene.mesh, centers, radius, region);
  RoughnessResult live = computeRoughness(scene.mesh, region, RoughnessPoints::Live,
                                          scene.strokeGen, up, rest);
  RoughnessResult base = computeRoughness(scene.mesh, region, RoughnessPoints::Base,
                                          scene.strokeGen, up, rest);
  std::fprintf(stdout,
               "[roughness] %s verts=%d edges=%d | live rms=%.6g p95=%.6g max=%.6g "
               "dih=%.6g | base rms=%.6g p95=%.6g max=%.6g dih=%.6g | maxdisp=%.6g "
               "vol=%.6g\n",
               tag, live.verts, live.edges, live.rms, live.p95, live.maxr,
               live.dihedral, base.rms, base.p95, base.maxr, base.dihedral,
               live.maxDisp, live.volume);
  std::fflush(stdout);
}

/* VDM texel snapshots (save_vdm / assert_vdm): every live tile's texels,
 * keyed by snapshot name — the texel analogue of g_posSnapshots. */
std::map<std::string, std::map<uint64_t, std::vector<float3>>> g_vdmSnapshots;

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
  if (verb == "make_shape") {
    /* make_shape kind=grid|cylinder|torus|sphere [n=..] [m=..] [size=..]
     *            [radius=..] [height=..] [capped=1] [minor=..] */
    const char *kind = getArg(args, "kind");
    if (!kind) {
      err = "make_shape: missing kind= (grid|cylinder|torus|sphere)";
      return false;
    }
    std::string ks = kind;
    for (auto &c : ks) c = (char)std::tolower((unsigned char)c);
    mesh::Mesh *m = nullptr;
    if (ks == "grid" || ks == "plane") {
      m = mesh::makeGrid(getInt(args, "n", 16), getInt(args, "m", 16),
                         getFloat(args, "size", 1.0f));
    } else if (ks == "cylinder") {
      m = mesh::makeCylinder(getInt(args, "n", 24), getInt(args, "m", 8),
                             getFloat(args, "radius", 0.5f),
                             getFloat(args, "height", 2.0f),
                             getBool(args, "capped", true));
    } else if (ks == "torus") {
      m = mesh::makeTorus(getInt(args, "n", 32), getInt(args, "m", 16),
                          getFloat(args, "radius", 1.0f),
                          getFloat(args, "minor", 0.3f));
    } else if (ks == "sphere" || ks == "uvsphere") {
      m = mesh::makeUVSphere(getInt(args, "n", 16), getInt(args, "m", 24),
                             getFloat(args, "radius", 1.0f));
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
    for (auto &c : ms) c = (char)std::tolower((unsigned char)c);
    if (ms == "subdivide") {
      scene.dyntopoParams.mode = dyntopo::DynTopoMode::Subdivide;
    } else if (ms == "collapse") {
      scene.dyntopoParams.mode = dyntopo::DynTopoMode::Collapse;
    } else if (ms == "both") {
      scene.dyntopoParams.mode = dyntopo::DynTopoMode::Both;
    } else {
      err = std::string("dyntopo: unknown mode '") + mode +
            "' (both|subdivide|collapse)";
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
    } else if (ts == "texgrad") {
      scene.currentTool = brush::SculptBrushes::TEXGRAD;
    } else if (ts == "color") {
      scene.currentTool = brush::SculptBrushes::COLOR;
    } else if (ts == "polygroup") {
      scene.currentTool = brush::SculptBrushes::POLYGROUP;
    } else if (ts == "bsmooth") {
      scene.currentTool = brush::SculptBrushes::BSMOOTH;
    } else if (ts == "enhance") {
      scene.currentTool = brush::SculptBrushes::ENHANCE;
    } else if (ts == "grab") {
      scene.currentTool = brush::SculptBrushes::GRAB;
    } else if (ts == "snakehook") {
      scene.currentTool = brush::SculptBrushes::SNAKEHOOK;
    } else if (ts == "layerdraw") {
      scene.currentTool = brush::SculptBrushes::LAYERDRAW;
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
      auto *uv =
          static_cast<mesh::AttrData<litestl::math::float2> *>(uvRef.data);
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
      vdm::exportFaceBounds(*scene.vdm, *scene.mesh,
                            std::span<const int>(pool.data(), pool.size()), bounds);
      Vector<int> bounded;
      for (int i = 0; i < int(pool.size()); i++) {
        if (bounds[i] > 1e-8f) {
          bounded.append(pool[i]);
        }
      }
      pool = std::move(bounded);
    }
    Vector<int> candidates;
    vdm::collectPromotionCandidates(*scene.mesh, *scene.tree, *scene.vdm,
                                    std::span<const int>(pool.data(), pool.size()),
                                    pp, candidates);
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
    vdm::VdmPromoteStats ps = vdm::promoteRegion(
        *scene.mesh, *scene.tree, *scene.vdm,
        std::span<const int>(candidates.data(), candidates.size()), pp, &combined,
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
                   t == brush::SculptBrushes::GRAB ||
                   t == brush::SculptBrushes::POSE ||
                   t == brush::SculptBrushes::COLOR ||
                   t == brush::SculptBrushes::POLYGROUP ||
                   t == brush::SculptBrushes::BSMOOTH;
    if ((scene.currentBackend == BrushBackend::Wgsl ||
         scene.currentBackend == BrushBackend::WgpuNative) &&
        gpuTool) {
      Vector<float3> origins;
      // repeat=N unifies N identical dabs into the one stroke (same as the
      // C++ branch below) — the discriminator for grab-class from-orig
      // semantics: repeated dabs with a fixed grabTo must re-base, not stack.
      int repeat = getInt(args, "repeat", 1);
      if (repeat < 1) repeat = 1;
      for (int i = 0; i < repeat; i++) {
        origins.append(origin);
      }
      if (!runBrushStrokeGPU(scene, origins, normal, err)) {
        return false;
      }
    } else
#endif
    {
      // One stroke verb = one stroke: advance the per-stroke generation every
      // stroke (mirrors the TS app's ++nextStrokeGen). It must be nonzero even in
      // accumulate mode — grab/kelvinlet always stamp the base, and gen 0 collides
      // with the fresh .brush.disp.gen default, which would make every stamped vert
      // read as unstamped. `repeat`ed dabs below share this one stamp.
      uint32_t gen = ++scene.strokeGen;
      int repeat = getInt(args, "repeat", 1);
      if (repeat < 1) repeat = 1;
      // One stroke verb = one meshlog step of `repeat` unified dabs through the
      // executor (same path as the TS app). Previously dyntopo was a separate undo
      // step, so one undo couldn't revert a stroke (tools/repro/repro_single_undo.txt).
      brush::CommandExecutor exec(scene.tree, &scene.brush);
      exec.meshLog = &scene.meshLog;
      exec.ctx.renderMatrix = scene.renderMatrix;
      if (scene.useCsrNeighbors) {
        exec.neighborMode = brush::CommandExecutor::NeighborMode::Csr;
      }
      exec.setNonAccum(scene.nonAccum);
      exec.setStrokeGen(int(gen));
      // layer=<name>: retarget the kernel's first declared attr handle (the
      // layerdraw brush's `slayer`) at the named sculpt layer.
      std::string layerName = getArg(args, "layer", "");
      if (!layerName.empty()) {
        int li = scene.mesh->findSculptLayer(litestl::util::string(layerName.c_str()));
        int attrIdx = li >= 0 ? scene.mesh->sculptLayerAttrIndex(li) : -1;
        if (attrIdx < 0) {
          err = "stroke: unknown sculpt layer '" + layerName + "'";
          return false;
        }
        exec.defaultAttrOverrides.append(brush::BrushAttrLayerOverride{0, attrIdx});
      }
      dyntopo::DynTopoParams *dtp =
          scene.dyntopoEnabled ? &scene.dyntopoParams : nullptr;
      // rough=1: print the §9.1 noise metrics after every dab, inside the one
      // stroke — a separate `roughness` verb per dab is impossible, since each
      // `stroke` verb bumps strokeGen and so resets the base.
      bool roughTrace = getBool(args, "rough", false);
      Vector<float3> roughCenters;
      char roughTag[64];
      exec.beginStep(scene.dyntopoEnabled);
      for (int i = 0; i < repeat; i++) {
        // Each repeat is a new logical dab's primary image (mirrors the TS
        // app's setGrabAccumAdd(false)): grab-class kernels re-base from the
        // stroke-start position per dab instead of adding with an idle
        // dab counter (curDabGen 0 == fresh stamps -> add mode).
        exec.setGrabAccumAdd(false);
        exec.applyDab(scene.currentTool, origin, normal, scene.brush.radius, dtp,
                      scene.dyntopoSeed + uint32_t(i));
        scene.cumSplits += exec.lastDynTopoStats.splits;
        scene.cumCollapses += exec.lastDynTopoStats.collapses;
        scene.cumFlips += exec.lastDynTopoStats.flips;
        if (roughTrace) {
          roughCenters.append(origin);
          std::snprintf(roughTag, sizeof(roughTag), "dab=%d", i);
          reportRoughness(scene, roughTag, roughCenters, scene.brush.radius,
                          float3(0, 0, 1), 0.0f);
        }
      }
      if (scene.dyntopoEnabled) {
        exec.endDynTopoStroke();
      }
      /* Mechanism B: fold an incremental compaction into this still-open stroke
       * step when the layout has fragmented past the threshold. */
      if (scene.autoDefragRatio > 0.0) {
        auto t0 = std::chrono::steady_clock::now();
        bool did = scene.meshLog.compactIfFragmented(scene.tree, scene.autoDefragRatio);
        if (did) {
          std::printf("[auto_defrag] compacted at stroke end in %.2fms\n",
                      std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count());
          std::fflush(stdout);
        }
      }
      exec.endStep();
    }
    // Both backends: the stroke verb leaves the tree current. The GPU path only
    // flags its touched nodes (normals/GPU/bounds) in session.end(), so without
    // this its normals stay stale.
    scene.tree->update(&scene.gpu);
    multiresStrokeEnd(scene);

    scene.lastStroke.valid = true;
    scene.lastStroke.origin = origin;
    scene.lastStroke.normal = normal;
    scene.lastStroke.radius = scene.brush.radius;
    scene.lastStroke.centers.clear();
    scene.lastStroke.centers.append(origin);
    return true;
  }
  if (verb == "stroke_folded") {
    /* Faithful repro of the TS/Electron interactive stroke (SculptPaintOp): ONE
     * meshlog step holding many interleaved per-dab topo chunks + brush-deform
     * simple chunks — unlike `stroke`, which records a single isolated topo step
     * + a separate deform step. This is the structure the redo hang needs.
     *   stroke_folded p1=x,y,z [p2=x,y,z] [normal=x,y,z] [dabs=N]
     * Dabs are linearly spaced p1->p2 (a swipe); p2 defaults to p1 (in place). */
    if (!scene.mesh || !scene.tree) {
      err = "stroke_folded: no mesh/tree";
      return false;
    }
    float3 p1, p2, normal{0, 0, 1};
    if (!parseFloat3(getArg(args, "p1"), p1) &&
        !parseFloat3(getArg(args, "origin"), p1)) {
      err = "stroke_folded: missing p1=x,y,z";
      return false;
    }
    if (!parseFloat3(getArg(args, "p2"), p2)) {
      p2 = p1;
    }
    parseFloat3(getArg(args, "normal"), normal);
    int dabs = getInt(args, "dabs", getInt(args, "repeat", 1));
    if (dabs < 1) dabs = 1;

    // Nonzero every stroke (see the stroke verb): grab orig-stamps in any mode.
    uint32_t gen = ++scene.strokeGen;

    brush::CommandExecutor exec(scene.tree, &scene.brush);
    exec.meshLog = &scene.meshLog;
    exec.ctx.renderMatrix = scene.renderMatrix;
    if (scene.useCsrNeighbors) {
      exec.neighborMode = brush::CommandExecutor::NeighborMode::Csr;
    }
    exec.setNonAccum(scene.nonAccum);
    exec.setStrokeGen(int(gen));
    // One step for the whole stroke; beginStep pushes the first topo chunk when
    // dyntopo is on, matching meshLog.beginStep(hasDyntopo) on the TS side.
    exec.beginStep(scene.dyntopoEnabled);
    for (int i = 0; i < dabs; i++) {
      float t = dabs > 1 ? float(i) / float(dabs - 1) : 0.0f;
      float3 c;
      for (int k = 0; k < 3; k++) {
        c[k] = p1[k] + (p2[k] - p1[k]) * t;
      }
      // One unified dab (dyntopo → deform → per-dab topo-chunk seal), matching
      // SculptPaintOp.applyDab on the TS side.
      dyntopo::DynTopoParams *dtp =
          scene.dyntopoEnabled ? &scene.dyntopoParams : nullptr;
      exec.applyDab(scene.currentTool, c, normal, scene.brush.radius, dtp,
                    scene.dyntopoSeed + uint32_t(i));
    }
    if (scene.dyntopoEnabled) {
      exec.endDynTopoStroke();
    }
    exec.endStep();
    scene.tree->update(&scene.gpu);
    multiresStrokeEnd(scene);

    scene.lastStroke.valid = true;
    scene.lastStroke.origin = p1;
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
                   t == brush::SculptBrushes::POSE ||
                   t == brush::SculptBrushes::COLOR ||
                   t == brush::SculptBrushes::POLYGROUP ||
                   t == brush::SculptBrushes::BSMOOTH;
    if ((scene.currentBackend == BrushBackend::Wgsl ||
         scene.currentBackend == BrushBackend::WgpuNative) &&
        gpuTool) {
      if (!runBrushStrokeGPU(scene, origins, normal, err)) {
        return false;
      }
    } else
#endif
    {
      // A path is one stroke = one meshlog step of unified dabs through the executor.
      // Advance the per-stroke generation once so every dab measures from the same
      // stroke-start snapshot; nonzero every stroke so grab orig-stamps in any mode.
      uint32_t gen = ++scene.strokeGen;
      brush::CommandExecutor exec(scene.tree, &scene.brush);
      exec.meshLog = &scene.meshLog;
      exec.ctx.renderMatrix = scene.renderMatrix;
      exec.setNonAccum(scene.nonAccum);
      exec.setStrokeGen(int(gen));
      dyntopo::DynTopoParams *dtp =
          scene.dyntopoEnabled ? &scene.dyntopoParams : nullptr;
      // rough=1: per-dab noise trace over the swept-so-far region (see `stroke`).
      bool roughTrace = getBool(args, "rough", false);
      Vector<float3> roughCenters;
      char roughTag[64];
      exec.beginStep(scene.dyntopoEnabled);
      for (size_t i = 0; i < origins.size(); i++) {
        exec.applyDab(scene.currentTool, origins[i], normal, scene.brush.radius,
                      dtp, scene.dyntopoSeed + uint32_t(i));
        if (roughTrace) {
          roughCenters.append(origins[i]);
          std::snprintf(roughTag, sizeof(roughTag), "dab=%zu", i);
          reportRoughness(scene, roughTag, roughCenters, scene.brush.radius,
                          float3(0, 0, 1), 0.0f);
        }
      }
      if (scene.dyntopoEnabled) {
        exec.endDynTopoStroke();
      }
      exec.endStep();
    }
    // Both backends leave the tree current (see the stroke verb).
    scene.tree->update(&scene.gpu);
    multiresStrokeEnd(scene);

    scene.lastStroke.valid = true;
    scene.lastStroke.origin = p2;
    scene.lastStroke.normal = normal;
    scene.lastStroke.radius = scene.brush.radius;
    scene.lastStroke.centers.clear();
    for (const float3 &o : origins) {
      scene.lastStroke.centers.append(o);
    }
    return true;
  }
  if (verb == "stroke_screen") {
    //   stroke_screen p1=x,y [p2=x,y] [steps=N] [method=..] [rough=0/1]
    // A stroke as a pointer drag in view pixels, sampled by the same
    // BrushStrokeDriver the app uses — it raycasts, so no world p/normal here.
    if (!scene.mesh || !scene.tree) {
      err = "stroke_screen: no mesh/tree";
      return false;
    }
    litestl::math::float2 p1, p2;
    if (!parseFloat2(getArg(args, "p1"), p1)) {
      err = "stroke_screen: missing p1=x,y";
      return false;
    }
    if (!parseFloat2(getArg(args, "p2"), p2)) {
      p2 = p1;
    }
    int steps = getInt(args, "steps", 8);
    if (steps < 1) {
      steps = 1;
    }

    brush::BrushStrokeDriver driver(scene.tree);
    std::string method = getArg(args, "method", "path");
    if (method == "anchored") {
      driver.strokeMethod = brush::StrokeMethod::Anchored;
    }
    else if (method == "dragdot") {
      driver.strokeMethod = brush::StrokeMethod::DragDot;
    }
    else if (method != "path") {
      err = "stroke_screen: unknown method '" + method + "'";
      return false;
    }
    scene.configureStrokeDriver(driver);

    // Nonzero every stroke (see the stroke verb): grab orig-stamps in any mode.
    uint32_t gen = ++scene.strokeGen;
    brush::CommandExecutor exec(scene.tree, &scene.brush);
    exec.meshLog = &scene.meshLog;
    exec.ctx.renderMatrix = scene.renderMatrix;
    if (scene.useCsrNeighbors) {
      exec.neighborMode = brush::CommandExecutor::NeighborMode::Csr;
    }
    exec.setNonAccum(scene.nonAccum);
    exec.setStrokeGen(int(gen));
    dyntopo::DynTopoParams *dtp = scene.dyntopoEnabled ? &scene.dyntopoParams : nullptr;
    bool roughTrace = getBool(args, "rough", false);
    char roughTag[64];

    Vector<float3> centers;
    float3 lastNormal{0, 0, 1};
    bool stepOpen = false;
    // The driver hands back a variable number of dabs per poll (it walks the
    // spline at brush spacing, not at the pointer's sample rate), so drain it
    // after every event and again after end() flushes the trailing segment.
    auto drain = [&]() {
      int n = driver.poll();
      for (int i = 0; i < n; i++) {
        const brush::DabSample *ps = driver.sampleAt(i);
        if (!ps) {
          continue;
        }
        // Open the step on the first dab, not before: a drag that misses the
        // surface entirely must not leave an empty step for `undo` to eat.
        if (!stepOpen) {
          exec.beginStep(scene.dyntopoEnabled);
          stepOpen = true;
        }
        // No object matrix (see Scene::configureStrokeDriver), so the driver's
        // object-local sample space is world space.
        float3 center(ps->p[0], ps->p[1], ps->p[2]);
        exec.applyDab(scene.currentTool, center, ps->vec, ps->radius, dtp,
                      scene.dyntopoSeed + uint32_t(centers.size()));
        scene.cumSplits += exec.lastDynTopoStats.splits;
        scene.cumCollapses += exec.lastDynTopoStats.collapses;
        scene.cumFlips += exec.lastDynTopoStats.flips;
        centers.append(center);
        lastNormal = ps->vec;
        if (roughTrace) {
          std::snprintf(roughTag, sizeof(roughTag), "dab=%zu", centers.size() - 1);
          reportRoughness(scene, roughTag, centers, scene.brush.radius, float3(0, 0, 1),
                          0.0f);
        }
      }
    };

    for (int i = 0; i < steps; i++) {
      float t = (steps == 1) ? 0.0f : float(i) / float(steps - 1);
      litestl::math::float2 p = p1 * (1.0f - t) + p2 * t;
      driver.push(p[0], p[1], /*pressure=*/1.0f, /*tiltX=*/0.0f, /*tiltY=*/0.0f,
                  /*twist=*/0.0f, scene.brush.invert, /*useAltBrush=*/false,
                  scene.brush.radius, scene.brush.strength, scene.brush.spacing);
      drain();
    }
    driver.end();
    drain();

    if (stepOpen) {
      if (scene.dyntopoEnabled) {
        exec.endDynTopoStroke();
      }
      exec.endStep();
    }

    scene.tree->update(&scene.gpu);
    multiresStrokeEnd(scene);

    if (centers.size() == 0) {
      std::printf("[stroke_screen] no dabs (every pointer event missed the surface)\n");
      std::fflush(stdout);
      return true;
    }
    scene.lastStroke.valid = true;
    scene.lastStroke.origin = centers[centers.size() - 1];
    scene.lastStroke.normal = lastNormal;
    scene.lastStroke.radius = scene.brush.radius;
    scene.lastStroke.centers.clear();
    for (const float3 &c : centers) {
      scene.lastStroke.centers.append(c);
    }
    return true;
  }
  if (verb == "preview_stroke_path") {
    // Exercises the live-mutating preview/rollback primitive (MeshLog::
    // beginPreviewDab/rollbackPreviewDab, step 2a of the Anchored/Drag Dot
    // plan): every point but the last is applied as a rollback-able preview
    // dab that is undone before the next preview, and only the final point
    // is left committed -- one meshlog step, mirroring a single anchored or
    // drag-dot gesture where the pointer moves through many preview
    // positions before release. CPU executor only (no GPU dispatch branch);
    // GPU preview wiring is step 3 of the plan, not yet implemented.
    if (!scene.mesh || !scene.tree) {
      err = "preview_stroke_path: no mesh/tree";
      return false;
    }
    float3 p1, p2, normal{0, 0, 1};
    if (!parseFloat3(getArg(args, "p1"), p1) ||
        !parseFloat3(getArg(args, "p2"), p2)) {
      err = "preview_stroke_path: missing p1/p2";
      return false;
    }
    parseFloat3(getArg(args, "normal"), normal);

    int steps = getInt(args, "steps", 8);
    if (steps < 1) {
      steps = 1;
    }
    Vector<float3> origins;
    for (int i = 0; i < steps; i++) {
      float t = (steps == 1) ? 0.0f : float(i) / float(steps - 1);
      origins.append(p1 * (1.0f - t) + p2 * t);
    }

    uint32_t gen = ++scene.strokeGen;
    brush::CommandExecutor exec(scene.tree, &scene.brush);
    exec.meshLog = &scene.meshLog;
    exec.ctx.renderMatrix = scene.renderMatrix;
    exec.setNonAccum(scene.nonAccum);
    exec.setStrokeGen(int(gen));
    dyntopo::DynTopoParams *dtp = scene.dyntopoEnabled ? &scene.dyntopoParams : nullptr;
    exec.beginStep(scene.dyntopoEnabled);
    for (size_t i = 0; i < origins.size(); i++) {
      bool isLast = (i + 1 == origins.size());
      exec.beginPreviewDab(origins[i], scene.brush.radius);
      exec.applyDab(scene.currentTool, origins[i], normal, scene.brush.radius, dtp,
                    scene.dyntopoSeed + uint32_t(i));
      // Per-dab spatial-query update mirrors a live pointer-move gesture,
      // where the tree must stay current between preview dabs for the next
      // dab's picking/raycast.
      scene.tree->updateQueries();
      if (!isLast) {
        exec.rollbackPreviewDab();
        scene.tree->updateQueries();
      }
    }
    if (scene.dyntopoEnabled) {
      exec.endDynTopoStroke();
    }
    exec.endStep();
    scene.tree->update(&scene.gpu);
    multiresStrokeEnd(scene);

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
  if (verb == "time_gather") {
    /* Time a whole-mesh gather (read every leaf's verts' co+no, leaf-by-leaf) —
     * the layout-sensitive streaming pattern of GPU buffer fill / normal recompute,
     * where the working set exceeds cache (a single brush dab fits cache regardless).
     * Run fragmented vs after `reorder` to measure the DRAM-locality cost. */
    if (!scene.tree || !scene.mesh) {
      err = "time_gather: no mesh/tree";
      return false;
    }
    int passes = getInt(args, "passes", 50);
    mesh::Mesh &m = *scene.mesh;
    util::Vector<spatial::SpatialNode *> leaves = scene.tree->leaves();
    double sink = 0.0;
    auto t0 = std::chrono::steady_clock::now();
    for (int p = 0; p < passes; p++) {
      for (spatial::SpatialNode *leaf : leaves) {
        for (int v : leaf->unique_verts()) {
          float3 co = m.v.co[v];
          float3 no = m.v.no[v];
          sink += double(co[0] + co[1] + co[2] + no[0] + no[1] + no[2]);
        }
      }
    }
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    int64_t total = 0;
    for (spatial::SpatialNode *leaf : leaves) total += leaf->unique_verts().size();
    std::printf("[time_gather] leaves=%d verts/pass=%lld passes=%d total=%.2fms per_pass=%.4fms (sink=%.1f)\n",
                int(leaves.size()), (long long)total, passes, ms, ms / double(passes), sink);
    std::fflush(stdout);
    return true;
  }
  if (verb == "dyntopo_stats") {
    /* Print cumulative dyntopo op counts since the last reset; `reset=1` zeroes. */
    std::printf("[dyntopo_stats] splits=%lld collapses=%lld flips=%lld\n",
                (long long)scene.cumSplits, (long long)scene.cumCollapses,
                (long long)scene.cumFlips);
    std::fflush(stdout);
    if (getBool(args, "reset", false)) {
      scene.cumSplits = scene.cumCollapses = scene.cumFlips = 0;
    }
    return true;
  }
  if (verb == "auto_defrag") {
    /* Enable mechanism-B stroke-end auto-compaction: `auto_defrag ratio=F`
     * (vert page-spread threshold; 0 disables). */
    scene.autoDefragRatio = getFloat(args, "ratio", 3.0f);
    return true;
  }
  if (verb == "reorder") {
    /* Full locality reorder (compaction) via the rebuild path. */
    if (!scene.tree) {
      err = "reorder: no spatial tree";
      return false;
    }
    auto t0 = std::chrono::steady_clock::now();
    scene.tree->reorderForLocality();
    auto t1 = std::chrono::steady_clock::now();
    std::printf("[reorder] full(rebuild) apply=%.2fms\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count());
    std::fflush(stdout);
    return true;
  }
  if (verb == "reorder_inc") {
    /* Incremental locality reorder (mechanism B): same permutation, but remap the
     * tree's cached indices in place instead of rebuilding. Should match
     * `reorder`'s locality gain at a fraction of the apply cost. */
    if (!scene.tree) {
      err = "reorder_inc: no spatial tree";
      return false;
    }
    util::Vector<int> vmap, emap, cmap, lmap, fmap;
    scene.tree->computeLocalityMaps(vmap, emap, cmap, lmap, fmap);
    auto t0 = std::chrono::steady_clock::now();
    scene.tree->applyReorderIncremental(vmap, emap, cmap, lmap, fmap);
    auto t1 = std::chrono::steady_clock::now();
    std::printf("[reorder_inc] incremental apply=%.2fms\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count());
    std::fflush(stdout);
    return true;
  }
  if (verb == "reorder_partial") {
    /* Phase 1b: partial (region-scoped) locality map, but still applied via the
     * full applyReorderIncremental. Selects fragmented leaves (thresh=ratio),
     * builds a mostly-identity closed permutation over just their slots, prints
     * the moved-vs-total counts (the "mostly identity" proof). Bracket with
     * frag_stats to measure whether frag holds vs the full reorder_inc. */
    if (!scene.tree) {
      err = "reorder_partial: no spatial tree";
      return false;
    }
    double thresh = getFloat(args, "thresh", 2.0f);
    util::Vector<sculptcore::spatial::SpatialNode *> dirty;
    scene.tree->selectFragmentedLeaves(thresh, dirty);

    bool scoped = getInt(args, "scoped", 1) != 0;
    util::Vector<int> vmap, emap, cmap, lmap, fmap;
    util::Vector<int> moved[5];
    auto tb0 = std::chrono::steady_clock::now();
    scene.tree->computeLocalityMapsPartial(dirty, vmap, emap, cmap, lmap, fmap, moved);
    auto tb1 = std::chrono::steady_clock::now();

    std::printf("[reorder_partial] thresh=%.2f scoped=%d dirtyLeaves=%d/%d build=%.2fms "
                "moved v=%d e=%d c=%d l=%d f=%d (liveV=%d liveF=%d)\n",
                thresh, int(scoped), int(dirty.size()),
                scene.tree->fragmentationStats().leaves,
                std::chrono::duration<double, std::milli>(tb1 - tb0).count(),
                int(moved[0].size()), int(moved[1].size()), int(moved[2].size()),
                int(moved[3].size()), int(moved[4].size()),
                scene.mesh->v.count, scene.mesh->f.count);

    auto t0 = std::chrono::steady_clock::now();
    if (scoped) {
      scene.tree->applyReorderIncremental(vmap, emap, cmap, lmap, fmap, moved[0],
                                          moved[1], moved[2], moved[3], moved[4]);
    } else {
      scene.tree->applyReorderIncremental(vmap, emap, cmap, lmap, fmap);
    }
    auto t1 = std::chrono::steady_clock::now();
    std::printf("[reorder_partial] apply=%.2fms\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count());
    std::fflush(stdout);
    return true;
  }
  if (verb == "frag_stats") {
    /* Print DRAM-locality fragmentation of the current tree (verts/faces:
     * distinct attribute pages per leaf vs ideal; ratio 1.0 = compact). */
    if (!scene.tree) {
      err = "frag_stats: no spatial tree";
      return false;
    }
    auto s = scene.tree->fragmentationStats();
    std::printf("[frag_stats] leaves=%d\n", s.leaves);
    std::printf("[frag_stats]   verts: count=%lld pagesActual=%lld pagesIdeal=%lld ratio=%.3f\n",
                (long long)s.vertCount, (long long)s.vertPagesActual,
                (long long)s.vertPagesIdeal, s.vertRatio);
    std::printf("[frag_stats]   faces: count=%lld pagesActual=%lld pagesIdeal=%lld ratio=%.3f\n",
                (long long)s.faceCount, (long long)s.facePagesActual,
                (long long)s.facePagesIdeal, s.faceRatio);
    std::fflush(stdout);
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
    scene.mesh->calcAABB(&amn, &amx);
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
  if (verb == "multires_init") {
    /* multires_init levels=N [level=L] [budget=B] [leaf_limit=..]
     * [depth_limit=..] [gpu_tri_target=..]: convert the current mesh into a
     * multires cage and attach level L (default: finest). The old mesh becomes
     * the cage (owned by the scene); mesh/tree become views of the active
     * level's slot. The three tree knobs stand in for build_spatial, which
     * refuses to run while the stack owns the trees; 0 keeps the
     * multiresAutoTune default for that knob. */
    if (!scene.mesh) {
      err = "multires_init: no mesh";
      return false;
    }
    if (scene.multires) {
      err = "multires_init: multires already active";
      return false;
    }
    int levels = getInt(args, "levels", 2);
    int level = getInt(args, "level", levels);
    if (levels < 1 || level < 1 || level > levels) {
      err = "multires_init: bad levels=/level=";
      return false;
    }
    mesh::Mesh *cage = scene.mesh;
    scene.mesh = nullptr;
    if (scene.tree) {
      litestl::alloc::Delete(scene.tree);
      scene.tree = nullptr;
    }
    scene.multiresCage = cage;
    scene.multires = litestl::alloc::New<subdiv::Multires>("debug multires");
    scene.multires->lruBudget = getInt(args, "budget", 3);
    scene.multires->treeLeafLimit = getInt(args, "leaf_limit", 0);
    scene.multires->treeDepthLimit = getInt(args, "depth_limit", 0);
    scene.multires->treeGpuTriTarget = getInt(args, "gpu_tri_target", 0);
    scene.multires->init(*cage, levels);
    scene.multires->setActiveLevel(level);
    scene.attachMultiresLevel();
    std::fprintf(stdout, "[script] multires_init levels=%d level=%d verts=%d faces=%d\n",
                 levels, level, scene.mesh->v.count, scene.mesh->f.count);
    return true;
  }
  if (verb == "multires_level") {
    /* multires_level level=L: write back the active level, switch to L. */
    if (!scene.multires) {
      err = "multires_level: multires not active (run multires_init)";
      return false;
    }
    int level = getInt(args, "level", 0);
    if (level < 1 || level > scene.multires->maxLevel()) {
      err = "multires_level: bad level=";
      return false;
    }
    scene.multires->setActiveLevel(level);
    scene.attachMultiresLevel();
    std::fprintf(stdout, "[script] multires_level level=%d verts=%d\n", level,
                 scene.mesh->v.count);
    return true;
  }
  if (verb == "multires_refit") {
    /* multires_refit [level=active]: least-squares-fit level-1 to level. */
    if (!scene.multires) {
      err = "multires_refit: multires not active (run multires_init)";
      return false;
    }
    int level = getInt(args, "level", scene.multires->activeLevel());
    if (level < 2 || level > scene.multires->maxLevel()) {
      err = "multires_refit: bad level= (need 2..maxLevel)";
      return false;
    }
    int changed = scene.multires->downRefit(level);
    scene.attachMultiresLevel();
    std::fprintf(stdout, "[script] multires_refit level=%d changed=%d\n", level,
                 changed);
    return true;
  }
  if (verb == "grid_stroke") {
    /* grid_stroke origin=x,y,z normal=x,y,z [dabs=1] [step=x,y,z]
     * [level=active]: run the current tool through the grids-native executor
     * (no materialized mesh / meshlog on the hot path), then mirror the
     * touched region into the resident slot mesh. Uses scene.brush props. */
    if (!scene.multires) {
      err = "grid_stroke: multires not active (run multires_init)";
      return false;
    }
    int level = getInt(args, "level", scene.multires->activeLevel());
    if (!ensureGridSession(scene, level, err)) {
      return false;
    }
    if (!brush::GridBrushExecutor::supportsBrush(scene.currentTool)) {
      err = "grid_stroke: tool not grids-capable (see GridBrushExecutor roster)";
      return false;
    }
    float3 origin{0, 0, 0}, normal{0, 0, 1}, step{0, 0, 0};
    if (!parseFloat3(getArg(args, "origin"), origin) ||
        !parseFloat3(getArg(args, "normal"), normal))
    {
      err = "grid_stroke: missing origin=/normal=";
      return false;
    }
    parseFloat3(getArg(args, "step"), step);
    int dabs = getInt(args, "dabs", 1);
    std::string backend = getArg(args, "backend") ? getArg(args, "backend") : "cpp";

    if (backend == "wgpu" || backend == "wgsl" || backend == "gpu") {
      // GPU grids path: the same kernels through a dispatcher — wgpu-native
      // (engine-owned device) or the Vulkan SPIR-V compute path; `gpu` takes
      // whichever is built, wgpu first. Shares the session's undo log with
      // the CPU path, so grid_undo mixes freely.
      const brush::GpuKernelInfo *ki =
          brush::GridGpuStrokeSession::kernelFor(scene.currentTool);
      if (!ki) {
        err = "grid_stroke: tool not grids-GPU-capable";
        return false;
      }
      auto runGpuStroke = [&](brush::IBrushComputeDispatch &d,
                              const char *tag) -> bool {
        brush::GridGpuStrokeSession gs;
        std::string serr;
        if (!gs.begin(scene.multires->gridDomain(level), &scene.brush,
                      scene.currentTool, &d, scene.gridLog, serr))
        {
          err = "grid_stroke: " + serr;
          return false;
        }
        float3 o = origin;
        for (int i = 0; i < dabs; i++, o += step) {
          if (!gs.dab(o, normal, serr)) {
            err = "grid_stroke: " + serr;
            return false;
          }
        }
        if (!gs.end(serr)) {
          err = "grid_stroke: " + serr;
          return false;
        }
        litestl::util::Vector<int> touched;
        for (int v : gs.strokeTouchedVerts()) {
          touched.append(v);
        }
        gridMirrorSync(scene, level, touched);
        std::fprintf(stdout,
                     "[script] grid_stroke(%s) level=%d dabs=%d moved=%d "
                     "undo_bytes=%zu\n",
                     tag, level, dabs, int(touched.size()), scene.gridLog->bytes());
        return true;
      };
#ifdef SBRUSH_WEBGPU_COMPUTE
      if (backend == "wgpu" || backend == "gpu") {
        webgpu::WgpuContext wctx;
        if (!wctx.initNative()) {
          err = "grid_stroke: wgpu-native device init failed";
          return false;
        }
        webgpu::WgpuBrushComputeDispatch disp(&wctx);
        std::string wgsl = std::string(SBRUSH_SCRIPT_STRINGIZE(SBRUSH_WGSL_DIR)) +
                           "/" + ki->kernel + ".wgsl";
        if (!disp.loadKernel(wgsl.c_str())) {
          err = "grid_stroke: failed to load " + wgsl;
          return false;
        }
        return runGpuStroke(disp, "wgpu");
      }
#endif
#ifdef SBRUSH_GPU_DISPATCH
      if (backend == "wgsl" || backend == "gpu") {
        if (!scene.ensureGPU() || !scene.context) {
          err = "grid_stroke: GPU device init failed";
          return false;
        }
        vulkan::BrushComputeDispatch disp(scene.context);
        std::string spv = std::string(SBRUSH_SCRIPT_STRINGIZE(SBRUSH_SPV_DIR)) +
                          "/" + ki->kernel + ".spv";
        if (!disp.loadKernel(spv.c_str())) {
          err = "grid_stroke: failed to load " + spv;
          return false;
        }
        return runGpuStroke(disp, "wgsl");
      }
#endif
      err = "grid_stroke: no GPU compute backend built";
      return false;
    }

    brush::GridBrushExecutor &ex = *scene.gridExec;
    scene.brush.writeProps();
    ex.beginStep();
    int moved = 0;
    float3 o = origin;
    for (int i = 0; i < dabs; i++, o += step) {
      ex.setGrabAccumAdd(false);
      moved += ex.applyDab(scene.currentTool, o, normal);
    }
    ex.endStep();
    litestl::util::Vector<int> touched;
    for (int v : ex.strokeTouchedVerts()) {
      touched.append(v);
    }
    gridMirrorSync(scene, level, touched);
    std::fprintf(stdout,
                 "[script] grid_stroke level=%d dabs=%d moved=%d undo_bytes=%zu\n",
                 level, dabs, moved, scene.gridLog->bytes());
    return true;
  }
  if (verb == "grid_undo" || verb == "grid_redo") {
    /* Seek the grids-native undo log one step and mirror the result. */
    if (!scene.multires) {
      err = "grid_undo/redo: multires not active";
      return false;
    }
    if (!scene.gridExec) {
      err = "grid_undo/redo: no grids session (run grid_stroke first)";
      return false;
    }
    bool ok = verb == "grid_undo" ? scene.gridLog->undo() : scene.gridLog->redo();
    if (ok) {
      // Mirror every vert of the level (seek granularity is leaf blocks; the
      // simple full sync keeps the debug mirror correct).
      subdiv::GridLevelDomain *d = scene.multires->gridDomain(scene.gridLevel);
      litestl::util::Vector<int> all;
      all.resize(d->vertCount());
      for (int v = 0; v < d->vertCount(); v++) {
        all[v] = v;
      }
      gridMirrorSync(scene, scene.gridLevel, all);
    }
    std::fprintf(stdout, "[script] %s ok=%d\n", verb.c_str(), int(ok));
    return true;
  }
  if (verb == "grid_bench") {
    /* grid_bench [subdivs=26] [levels=4] [strokes=19] [dabs=57] [radius=0.1]
     * [strength=0.5] [leaf=512]: self-contained grids-native benchmark — own
     * cube cage + Multires (the scene is untouched), draw strokes marching
     * across the top face, per-phase stats + the G3 perf gates printed. */
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::time_point a, clock::time_point b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };
    int subdivs = getInt(args, "subdivs", 26);
    int levels = getInt(args, "levels", 4);
    int strokes = getInt(args, "strokes", 19);
    int dabs = getInt(args, "dabs", 57);
    float radius = getFloat(args, "radius", 0.1f);
    float strength = getFloat(args, "strength", 0.5f);
    int leaf = getInt(args, "leaf", 512);

    mesh::Mesh *cage = mesh::createCube(subdivs, 1.0f);
    subdiv::Multires *mrb = litestl::alloc::New<subdiv::Multires>("grid bench mr");
    auto t0 = clock::now();
    mrb->init(*cage, levels);
    auto t1 = clock::now();
    subdiv::GridLevelDomain *d = mrb->gridDomain(levels);
    auto t2 = clock::now();
    subdiv::GridTree *tree = d->ensureTree(leaf);
    auto t3 = clock::now();
    std::fprintf(stdout,
                 "[grid_bench] cage %d faces; level %d: %d verts, %d grids, %d "
                 "leaves\n",
                 cage->f.count, levels, d->vertCount(), d->gridCount(),
                 int(tree->leaves.size()));
    std::fprintf(stdout,
                 "[grid_bench] init=%.1fms domain=%.1fms tree=%.1fms\n",
                 ms(t0, t1), ms(t1, t2), ms(t2, t3));

    brush::Brush benchBrush;
    benchBrush.radius = radius;
    benchBrush.strength = strength;
    benchBrush.writeProps();
    subdiv::GridStrokeLog log;

    std::string backend = getArg(args, "backend") ? getArg(args, "backend") : "cpp";
    if (backend == "wgpu" || backend == "wgsl" || backend == "gpu") {
      // GPU half of the "at what size does GPU win" question: same workload
      // through the grids GPU session, dispatch/readback split printed.
      auto runGpuBench = [&](brush::IBrushComputeDispatch &disp,
                             const char *tag) -> bool {
        brush::GridGpuStrokeSession gs;
        auto t4 = clock::now();
        int movedTotal = 0;
        for (int s = 0; s < strokes; s++) {
          std::string serr;
          if (!gs.begin(d, &benchBrush, brush::SculptBrushes::DRAW, &disp, &log,
                        serr))
          {
            std::fprintf(stdout, "[grid_bench] gpu begin failed: %s\n", serr.c_str());
            return false;
          }
          float y = -0.4f + 0.8f * float(s) / float(strokes > 1 ? strokes - 1 : 1);
          for (int i = 0; i < dabs; i++) {
            float x = -0.45f + 0.9f * float(i) / float(dabs > 1 ? dabs - 1 : 1);
            if (!gs.dab(float3(x, y, 0.5f), float3(0, 0, 1), serr)) {
              std::fprintf(stdout, "[grid_bench] gpu dab failed: %s\n", serr.c_str());
              return false;
            }
          }
          if (!gs.end(serr)) {
            std::fprintf(stdout, "[grid_bench] gpu end failed: %s\n", serr.c_str());
            return false;
          }
          movedTotal += int(gs.strokeTouchedVerts().size());
        }
        auto t5 = clock::now();
        const auto &gst = gs.stats;
        std::fprintf(stdout,
                     "[grid_bench] gpu(%s) %d strokes x %d dabs: total=%.1fms "
                     "moved=%d\n",
                     tag, strokes, dabs, ms(t4, t5), movedTotal);
        std::fprintf(stdout,
                     "[grid_bench] gpu per-dab ms: host=%.4f dispatch=%.4f "
                     "readback=%.4f\n",
                     gst.hostMs / gst.dabs, gst.dispatchMs / gst.dabs,
                     gst.readbackMs / gst.dabs);
        return true;
      };
      bool ok = false;
#ifdef SBRUSH_WEBGPU_COMPUTE
      if (!ok && (backend == "wgpu" || backend == "gpu")) {
        const brush::GpuKernelInfo *ki =
            brush::GridGpuStrokeSession::kernelFor(brush::SculptBrushes::DRAW);
        webgpu::WgpuContext wctx;
        if (ki && wctx.initNative()) {
          webgpu::WgpuBrushComputeDispatch disp(&wctx);
          std::string wgsl = std::string(SBRUSH_SCRIPT_STRINGIZE(SBRUSH_WGSL_DIR)) +
                             "/" + ki->kernel + ".wgsl";
          if (disp.loadKernel(wgsl.c_str())) {
            ok = runGpuBench(disp, "wgpu");
          }
        }
      }
#endif
#ifdef SBRUSH_GPU_DISPATCH
      if (!ok && (backend == "wgsl" || backend == "gpu")) {
        const brush::GpuKernelInfo *ki =
            brush::GridGpuStrokeSession::kernelFor(brush::SculptBrushes::DRAW);
        if (ki && scene.ensureGPU() && scene.context) {
          vulkan::BrushComputeDispatch disp(scene.context);
          std::string spv = std::string(SBRUSH_SCRIPT_STRINGIZE(SBRUSH_SPV_DIR)) +
                            "/" + ki->kernel + ".spv";
          if (disp.loadKernel(spv.c_str())) {
            ok = runGpuBench(disp, "wgsl");
          }
        }
      }
#endif
      if (!ok) {
        std::fprintf(stdout, "[grid_bench] gpu backend unavailable\n");
      }
      litestl::alloc::Delete(mrb);
      litestl::alloc::Delete(cage);
      return true;
    }

    brush::GridBrushExecutor ex(d, &benchBrush, &log);
    ex.stats.reset();

    auto t4 = clock::now();
    int moved = 0;
    for (int s = 0; s < strokes; s++) {
      ex.beginStep();
      // March across the top face; alternate rows per stroke so the touched
      // region varies like the interactive bench.
      float y = -0.4f + 0.8f * float(s) / float(strokes > 1 ? strokes - 1 : 1);
      for (int i = 0; i < dabs; i++) {
        float x = -0.45f + 0.9f * float(i) / float(dabs > 1 ? dabs - 1 : 1);
        moved += ex.applyDab(brush::SculptBrushes::DRAW, float3(x, y, 0.5f),
                             float3(0, 0, 1));
      }
      ex.endStep();
    }
    auto t5 = clock::now();

    const auto &st = ex.stats;
    double totalMs = ms(t4, t5);
    std::fprintf(stdout,
                 "[grid_bench] %d strokes x %d dabs: total=%.1fms moved=%d\n",
                 strokes, dabs, totalMs, moved);
    std::fprintf(stdout,
                 "[grid_bench] per-dab ms: query=%.4f capture=%.4f coPrev=%.4f "
                 "stamp=%.4f automask=%.4f kernel=%.4f normals=%.4f bounds=%.4f\n",
                 st.queryMs / st.dabs, st.captureMs / st.dabs, st.coPrevMs / st.dabs,
                 st.stampMs / st.dabs, st.automaskMs / st.dabs, st.kernelMs / st.dabs,
                 st.normalsMs / st.dabs, st.boundsMs / st.dabs);
    double core = st.perDabCoreMs();
    double wb = st.strokes > 0 ? st.writebackMs / st.strokes : 0.0;
    double undoMB = double(log.bytes()) / (1024.0 * 1024.0);
    std::fprintf(stdout,
                 "[grid_bench] gates: per-dab core %.4fms (<=0.35) %s | writeback "
                 "%.2fms/stroke (<=3) %s | undo %.1fMB (<=20) %s\n",
                 core, core <= 0.35 ? "PASS" : "FAIL", wb, wb <= 3.0 ? "PASS" : "FAIL",
                 undoMB, undoMB <= 20.0 ? "PASS" : "FAIL");

    litestl::alloc::Delete(mrb);
    litestl::alloc::Delete(cage);
    return true;
  }
  if (verb == "save_disp") {
    /* save_disp id=NAME level=L: snapshot a level's disp channel. */
    if (!scene.multires) {
      err = "save_disp: multires not active";
      return false;
    }
    int level = getInt(args, "level", scene.multires->activeLevel());
    std::string name = getArg(args, "id", "default");
    gatherDisp(*scene.multires, level, g_dispSnapshots[name]);
    std::fprintf(stdout, "[script] save_disp id=%s level=%d floats=%zu\n", name.c_str(),
                 level, g_dispSnapshots[name].size());
    return true;
  }
  if (verb == "assert_disp") {
    /* assert_disp id=NAME level=L [eps=0] [changed=0]: compare the level's
     * disp channel against a snapshot. eps=0 means bit-exact; changed=1
     * asserts the channel DIFFERS beyond eps instead of matching. */
    if (!scene.multires) {
      err = "assert_disp: multires not active";
      return false;
    }
    int level = getInt(args, "level", scene.multires->activeLevel());
    std::string name = getArg(args, "id", "default");
    auto it = g_dispSnapshots.find(name);
    if (it == g_dispSnapshots.end()) {
      err = "assert_disp: no snapshot '" + name + "'";
      return false;
    }
    std::vector<float> cur;
    gatherDisp(*scene.multires, level, cur);
    if (cur.size() != it->second.size()) {
      err = "assert_disp: size mismatch";
      return false;
    }
    float eps = getFloat(args, "eps", 0.0f);
    bool wantChanged = getBool(args, "changed", false);
    int diffs = 0;
    float worst = 0.0f;
    for (size_t i = 0; i < cur.size(); i++) {
      float d = std::fabs(cur[i] - it->second[i]);
      if (d > eps || (eps == 0.0f && std::memcmp(&cur[i], &it->second[i], 4) != 0)) {
        diffs++;
        worst = d > worst ? d : worst;
      }
    }
    std::fprintf(stdout, "[script] assert_disp id=%s level=%d diffs=%d worst=%g\n",
                 name.c_str(), level, diffs, worst);
    if (wantChanged ? diffs == 0 : diffs > 0) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "assert_disp: %s (diffs=%d worst=%g)",
                    wantChanged ? "expected change, none found" : "unexpected diffs",
                    diffs, worst);
      err = buf;
      return false;
    }
    return true;
  }
  if (verb == "roughness") {
    if (!scene.mesh) {
      err = "roughness: no mesh";
      return false;
    }
    // Region: an explicit center=, else every dab origin of the last stroke.
    Vector<float3> centers;
    float3 c;
    if (parseFloat3(getArg(args, "center"), c)) {
      centers.append(c);
    } else if (scene.lastStroke.valid) {
      for (const float3 &o : scene.lastStroke.centers) {
        centers.append(o);
      }
      if (centers.size() == 0) {
        centers.append(scene.lastStroke.origin);
      }
    } else {
      err = "roughness: no center= and no prior stroke";
      return false;
    }
    float radius = getFloat(
        args, "radius",
        scene.lastStroke.valid ? scene.lastStroke.radius : scene.brush.radius);
    float3 up{0, 0, 1};
    parseFloat3(getArg(args, "up"), up);
    reportRoughness(scene, getArg(args, "tag", "region"), centers, radius, up,
                    getFloat(args, "rest", 0.0f));
    return true;
  }
  if (verb == "save_pos") {
    if (!scene.mesh) {
      err = "save_pos: no mesh";
      return false;
    }
    std::string name = getArg(args, "id", "default");
    auto &snap = g_posSnapshots[name];
    snap.clear();
    for (int v : scene.mesh->v) {
      snap.emplace_back(v, scene.mesh->v.co[v]);
    }
    std::fprintf(stdout, "[script] save_pos id=%s verts=%zu\n", name.c_str(),
                 snap.size());
    return true;
  }
  if (verb == "assert_pos") {
    if (!scene.mesh) {
      err = "assert_pos: no mesh";
      return false;
    }
    std::string name = getArg(args, "id", "default");
    float eps = getFloat(args, "eps", 1e-5f);
    auto it = g_posSnapshots.find(name);
    if (it == g_posSnapshots.end()) {
      err = "assert_pos: no snapshot '" + name + "'";
      return false;
    }
    int dead = 0, moved = 0, worstIdx = -1, firstBad = -1;
    float worst = 0.0f;
    for (auto &pr : it->second) {
      int v = pr.first;
      if (v >= int(scene.mesh->v.capacity()) || scene.mesh->v.freemap[v]) {
        dead++;
        if (firstBad < 0) firstBad = v;
        continue;
      }
      float d = (scene.mesh->v.co[v] - pr.second).length();
      if (d > eps) {
        moved++;
        if (firstBad < 0) firstBad = v;
        if (d > worst) {
          worst = d;
          worstIdx = v;
        }
      }
    }
    std::fprintf(
        stdout,
        "[script] assert_pos id=%s checked=%zu dead=%d moved=%d worst=%g (vert %d)\n",
        name.c_str(), it->second.size(), dead, moved, worst, worstIdx);
    if (dead > 0 || moved > 0) {
      char buf[256];
      std::snprintf(buf, sizeof(buf),
                    "assert_pos: %d dead, %d moved (worst %g at vert %d, first %d)",
                    dead, moved, worst, worstIdx, firstBad);
      err = buf;
      /* soft=1 reports the divergence but lets the script continue. */
      return getBool(args, "soft", false);
    }
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
      const float weight = value ? float(std::atof(value))
                                 : (scene.mesh->v.co[v][2] - zlo) / span;
      mesh::DeformWeight dw{group, weight};
      w.setRun(v, litestl::util::span<const mesh::DeformWeight>(&dw, 1));
      n++;
    }
    std::fprintf(stdout, "[script] set_weights name=%s group=%d verts=%d slots=%zu\n",
                 name, group, n, scene.mesh->deformPool().liveSlotCount());
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
    std::fprintf(stdout, "[script] save_weights id=%s name=%s verts=%zu\n", id.c_str(),
                 name, snap.size());
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
        if (firstBad < 0) firstBad = v;
        continue;
      }
      const int cnt = w.getRun(v, buf, mesh::DEFORM_MAX_INFLUENCES);
      if (cnt != int(pr.second.size())) {
        reshaped++;
        if (firstBad < 0) firstBad = v;
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
          if (firstBad < 0) firstBad = v;
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
                 id.c_str(), it->second.size(), dead, reshaped, changed, worst, worstIdx);
    if (dead > 0 || reshaped > 0 || changed > 0) {
      char buf2[256];
      std::snprintf(buf2, sizeof(buf2),
                    "assert_weights: %d dead, %d reshaped, %d changed (worst %g at "
                    "vert %d, first %d)",
                    dead, reshaped, changed, worst, worstIdx, firstBad);
      err = buf2;
      // soft=1 reports the divergence but lets the script continue.
      return getBool(args, "soft", false);
    }
    return true;
  }
  if (verb == "undo") {
    if (scene.multires) {
      /* Level-aware undo: the step was recorded on the level it was stroked
       * at — auto-switch there first, then re-sync the store from the undone
       * positions (writeback's baseline diff touches only the stroke verts). */
      if (scene.mrUndoLevels.size() == 0) {
        return true; /* nothing multires-recorded to undo */
      }
      int level = scene.mrUndoLevels.pop_back();
      if (level != scene.multires->activeLevel()) {
        /* propagate=false: this switch replays history, and a downward one
         * would push the very detail we are about to undo into the level
         * below. The debt survives for the user's next real switch. */
        scene.multires->setActiveLevel(level, /*propagate=*/false);
        scene.attachMultiresLevel();
      }
      scene.mesh->thawTopo();
      scene.meshLog.undo(scene.mesh, scene.tree);
      scene.multires->writeback(level);
      scene.mrRedoLevels.append(level);
      if (!checkTreeVsRebuild(scene, "undo", err)) {
        return false;
      }
      return true;
    }
    if (scene.mesh && scene.tree) {
      /* The meshlog recorded topology in the thawed state; a brush stroke
       * leaves the mesh frozen (live links freed/CSR-rebuilt), which would
       * mismatch the recorded links during replay. Thaw first. */
      scene.mesh->thawTopo();
      scene.meshLog.undo(scene.mesh, scene.tree);
      /* No masking rebuild: verify the incrementally-maintained tree against
       * the live mesh so it persists and cascades exactly as in Electron. */
      if (!checkTreeVsRebuild(scene, "undo", err)) {
        return false;
      }
    }
    return true;
  }
  if (verb == "redo") {
    if (scene.multires) {
      if (scene.mrRedoLevels.size() == 0) {
        return true;
      }
      int level = scene.mrRedoLevels.pop_back();
      if (level != scene.multires->activeLevel()) {
        /* propagate=false, as in undo: replaying history, not a user switch. */
        scene.multires->setActiveLevel(level, /*propagate=*/false);
        scene.attachMultiresLevel();
      }
      scene.mesh->thawTopo();
      scene.meshLog.redo(scene.mesh, scene.tree);
      scene.multires->writeback(level);
      scene.mrUndoLevels.append(level);
      if (!checkTreeVsRebuild(scene, "redo", err)) {
        return false;
      }
      return true;
    }
    if (scene.mesh && scene.tree) {
      scene.mesh->thawTopo();
      scene.meshLog.redo(scene.mesh, scene.tree);
      if (!checkTreeVsRebuild(scene, "redo", err)) {
        return false;
      }
    }
    return true;
  }
  if (verb == "check_tree") {
    /* Assert incremental tree consistency at an arbitrary script point (no-op
     * when clean). Handy for bisecting a divergence. */
    if (scene.mesh && scene.tree) {
      if (!checkTreeVsRebuild(scene, "check_tree", err)) {
        return false;
      }
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

  if (verb == "bench_dyntopo") {
    /* A/B the per-dab spatial cost: a full tree rebuild (what the pre-M3 path
     * paid every dab) vs one incremental dyntopo dab (ops + incremental node
     * ownership + tree->update of only the dirty leaves). Run on increasingly
     * large meshes to see the rebuild cost grow while the incremental dab stays
     * roughly flat (local to the brush). */
    if (!scene.mesh || !scene.tree) {
      err = "bench_dyntopo: no mesh/tree (make_cube; triangulate; build_spatial)";
      return false;
    }
    int leaf = getInt(args, "leaf_limit", 0);
    int depth = getInt(args, "depth_limit", 16);
    float radius = getFloat(args, "radius", 0.2f);
    float detail = getFloat(args, "detail", 0.02f);
    float3 center{0, 0, 0};
    parseFloat3(getArg(args, "center"), center);

    /* rebuild=0 skips the full tree rebuild + its A/B baseline, reusing the
     * current tree (kept current by the dyntopo callbacks). Lets a script fire
     * several independent dabs on one (expensive) large build to map ops vs
     * split count without paying the O(mesh) rebuild each time. */
    bool doRebuild = getBool(args, "rebuild", true);
    double rebuild_ms = 0.0;
    if (doRebuild) {
      auto t0 = std::chrono::steady_clock::now();
      scene.buildSpatial(leaf, depth, 0);
      rebuild_ms = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0)
                       .count();

      /* Warm-up: build the GPU buffers once so the measured update() below is an
       * INCREMENTAL one (per-frame in the real app), not the cold first build. */
      scene.tree->update(&scene.gpu);
    }

    int fBefore = scene.mesh->f.count;
    scene.dyntopoParams.l_max = detail;
    scene.dyntopoParams.l_min = detail * 0.4f;
    scene.dyntopoParams.grade = getFloat(args, "grade", 0.0f);
    scene.dyntopoParams.do_flips = getBool(args, "flip", true);
    scene.dyntopoParams.max_splits = getInt(args, "max_splits", 0);
    scene.dyntopoParams.do_smooth = getBool(args, "smooth", false);
    scene.dyntopoParams.smooth_lambda = getFloat(args, "smooth_lambda", 0.5f);
    scene.dyntopoParams.mode = dyntopo::DynTopoMode::Subdivide;

    /* Break the incremental dab into its two phases: the remesh + incremental
     * node ownership (local to the brush) vs tree->update() (whose partition +
     * draw-batch phases are currently global). */
    bool useSpatial = getBool(args, "spatial", true);
    /* seed=1 (default): round-0 seed from the tree's in-region leaves (local);
     * seed=0: full-mesh scan. A/B confirms split-count parity + the round-0 win. */
    Vector<int> seedVerts;
    if (getBool(args, "seed", true)) {
      Vector<spatial::SpatialNode *> hit;
      scene.tree->filterNodes(center, radius, hit);
      for (spatial::SpatialNode *n : hit) {
        for (int v : n->unique_verts()) {
          seedVerts.append(v);
        }
      }
    }
    scene.mesh->thawTopo();
    auto t1 = std::chrono::steady_clock::now();
    dyntopo::DynTopoStats st = dyntopo::runDyntopoRemesh(
        *scene.mesh, center, radius, scene.dyntopoParams, 7u,
        useSpatial ? scene.tree->getSpatialCallbacks() : nullptr,
        litestl::util::span<const int>(seedVerts.data(), seedVerts.size()));
    double ops_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t1)
                        .count();
    auto t2 = std::chrono::steady_clock::now();
    scene.tree->update(&scene.gpu);
    double update_ms = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - t2)
                           .count();
    double dab_ms = ops_ms + update_ms;

    /* Convergence check: any in-region edge still longer than its (graded)
     * target means the dab under-refined. Also accumulate edge-length mean/var
     * over the region for a uniformity metric (CV = stddev/mean; lower = more
     * even triangles — the thing tangential smoothing improves). */
    int leftover = 0;
    float grade = scene.dyntopoParams.grade;
    float r2 = radius * radius;
    double lsum = 0.0, lsq = 0.0;
    int lcount = 0;
    for (int e : scene.mesh->e) {
      if (scene.mesh->e.c[e] == ELEM_NONE) continue;
      float3 a = scene.mesh->v.co[scene.mesh->e.vs[e][0]];
      float3 b = scene.mesh->v.co[scene.mesh->e.vs[e][1]];
      float3 mid = (a + b) * 0.5f;
      float d2 = (mid - center).lengthSqr();
      if (d2 > r2) continue;
      float target = detail;
      if (grade > 0.0f && radius > 0.0f) {
        target *= 1.0f + grade * (std::sqrt(d2) / radius);
      }
      float len = (a - b).length();
      if (len > target * 1.001f) {
        leftover++;
      }
      lsum += len;
      lsq += double(len) * len;
      lcount++;
    }
    double lmean = lcount > 0 ? lsum / lcount : 0.0;
    double lvar = lcount > 0 ? lsq / lcount - lmean * lmean : 0.0;
    double lcv = lmean > 0.0 ? std::sqrt(lvar > 0.0 ? lvar : 0.0) / lmean : 0.0;

    /* Max valence over in-region verts (the cascade's high-valence symptom —
     * grading should keep this near the regular-mesh value of 6). */
    int maxVal = 0;
    mesh::Mesh &mm = *scene.mesh;
    for (int v : mm.v) {
      if ((mm.v.co[v] - center).lengthSqr() > r2 || mm.v.e[v] == ELEM_NONE) {
        continue;
      }
      int n = 0, e0 = mm.v.e[v], e = e0;
      do {
        n++;
        int side = mm.e.vs[e][0] == v ? 0 : 1;
        e = mesh::diskEdge(mm.e.disk[e][side * 2 + 1]);
      } while (e != e0 && n < 100000);
      if (n > maxVal) maxVal = n;
    }

    std::printf("[bench_dyntopo] faces %d->%d  splits=%d flips=%d smooths=%d "
                "rounds=%d leftover=%d maxValence=%d cv=%.3f%s%s | "
                "full_rebuild=%.2fms | incremental: ops=%.2fms update=%.2fms "
                "total=%.2fms  speedup=%.1fx\n",
                fBefore, scene.mesh->f.count, st.splits, st.flips, st.smooths,
                st.rounds, leftover, maxVal, lcv, st.budget_hit ? " BUDGET" : "",
                (st.capped && !st.budget_hit) ? " CAPPED" : "", rebuild_ms, ops_ms,
                update_ms, dab_ms, dab_ms > 0.0 ? rebuild_ms / dab_ms : 0.0);
    std::fflush(stdout);
    return true;
  }

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
    std::printf("[save_mesh] %s verts=%d edges=%d faces=%d\n", path.c_str(),
                scene.mesh->v.count, scene.mesh->e.count, scene.mesh->f.count);
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
                path.c_str(), nm->v.count, nm->e.count, nm->f.count, problems);
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
    params.curvature_weight =
        getFloat(args, "curvature_weight", params.curvature_weight);
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
                rep.vert_count, rep.edge_count, rep.face_count, rep.euler,
                rep.tri_count, rep.quad_count, rep.ngon_count, rep.all_quad,
                rep.manifold, rep.consistent_winding, rep.non_manifold_edges,
                rep.boundary_edges, rep.degenerate_faces, rep.inverted_faces,
                rep.irregular_interior_verts);
    if (!rep.manifold) {
      std::printf("[remesh_validate]   topology error: %s\n",
                  rep.manifold_error.c_str());
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
    std::printf("[remesh_curvature] verts=%d mean kmin=%.4f kmax=%.4f\n", n,
                n > 0 ? sKmin / n : 0.0, n > 0 ? sKmax / n : 0.0);
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
    std::printf("[remesh_feature_tag] edges=%d sharp=%d boundary=%d\n", n, sharp,
                boundary);
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
    float3 p(getFloat(args, "x", 0.0f), getFloat(args, "y", 0.0f),
             getFloat(args, "z", 0.0f));
    mesh::ClosestPointResult res = mesh::findClosestPoint(tree, p);
    std::printf("[remesh_closest_point] q=(%.3f,%.3f,%.3f) hit=%d dist=%.5f "
                "face=%d point=(%.4f,%.4f,%.4f)\n",
                p[0], p[1], p[2], res.hit, res.dist, res.face, res.point[0],
                res.point[1], res.point[2]);
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
                st.num_faces, st.num_singularities, st.index_sum, 4 * chi,
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
                st.num_faces, st.num_singularities, st.index_sum, 4 * chi,
                st.curl_before, st.curl_after);
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
                  cs.rounds, cs.attempted_pairs, cs.cancelled_pairs,
                  cs.reverted_rounds, cs.num_singularities, cs.index_sum,
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
    spp.target_edge_length =
        getFloat(args, "target_edge_length", spp.target_edge_length);
    spp.use_density = getBool(args, "use_density", spp.use_density);
    spp.gauge_eps = getFloat(args, "gauge_eps", spp.gauge_eps);
    remesh::SeamlessParamStats st = remesh::computeSeamlessParam(m, spp);
    std::printf("[remesh_seamless] faces=%d corners=%d classes=%d cut_edges=%d "
                "grad_angle_err=%.6f max_seam_translation=%.6e min_jacobian=%.6f "
                "solved=%d\n",
                st.num_faces, st.num_corners, st.num_classes, st.num_cut_edges,
                st.grad_angle_err, st.max_seam_translation, st.min_jacobian,
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
    qp.seam_relax_min_folds = int(
        getFloat(args, "seam_relax_min_folds", float(qp.seam_relax_min_folds)));
    qp.untangle_fold_threshold = getFloat(args, "untangle_threshold",
                                          float(qp.untangle_fold_threshold));
    qp.untangle_field_max_dev =
        getFloat(args, "untangle_max_dev", float(qp.untangle_field_max_dev));
    remesh::QuantizeStats st = remesh::computeQuantization(m, qp);
    std::printf("[remesh_quantize] faces=%d corners=%d classes=%d cut_edges=%d "
                "int_residual=%.6e loop_closure=%.6e min_jacobian=%.6f folds=%d "
                "iters=%d solved=%d feasible=%d\n",
                st.num_faces, st.num_corners, st.num_classes, st.num_cut_edges,
                st.max_integer_residual, st.max_loop_closure, st.min_jacobian,
                st.parametrization_folds, st.iters, st.solved, st.feasible);
    std::printf("[remesh_quantize:profile] total_ms=%.1f setup=%.1f init=%.1f "
                "arap=%.1f rounding=%.1f (assemble=%.1f refactor=%.1f "
                "updown=%.1f backsolve=%.1f) convert=%.1f tier1b=%.1f "
                "stiffen=%.1f tier3=%.1f refactors=%d updowns=%d refreshes=%d "
                "back_solves=%d probes=%d\n",
                st.total_ms, st.setup_ms, st.initial_factor_ms, st.arap_ms,
                st.rounding_ms, st.round_assemble_ms, st.round_refactor_ms,
                st.round_updown_ms, st.round_backsolve_ms, st.convert_ms,
                st.tier1b_ms, st.stiffen_ms, st.tier3_ms, st.full_refactors,
                st.updowns, st.simp_refreshes, st.back_solves, st.tier1b_probes);
    std::printf("[remesh_quantize:gs] rounds=%d converged=%d visits=%d "
                "touched_total=%d touched_max=%d gs_ms=%.1f resort_full=%d "
                "resort_incr=%d resort_keys=%d\n",
                st.gs_rounds, st.gs_converged, st.gs_visits, st.gs_touched_total,
                st.gs_touched_max, st.gs_ms, st.resort_full, st.resort_incr,
                st.resort_keys);
    std::printf("[remesh_quantize:pairs] singularities=%d spurious_pairs=%d "
                "seamless_folds=%d near_pairs=%d\n",
                st.num_singularities, st.spurious_pairs, st.seamless_folds,
                st.seamless_folds_near_pairs);
    std::printf("[remesh_quantize:align] field_dev_mean_deg=%.2f "
                "field_dev_max_deg=%.2f field_dev_frac=%.4f\n",
                st.field_dev_mean_deg, st.field_dev_max_deg, st.field_dev_frac);
    std::fflush(stdout);
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
