#include "gpu_stroke.h"

#ifdef SBRUSH_GPU_DISPATCH

#include "scene.h"

#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/utils/triangulate.h"
#include "spatial/node.h"
#include "spatial/spatial.h"
#include "spatial/spatial_enums.h"
#include "vulkan/vk_backend.h"
#include "vulkan/vk_compute.h"
#include "vulkan/vk_context.h"
#include "vulkan/vk_normals.h"

#include "litestl/math/geom.h"
#include "litestl/util/index_range.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace sculptcore::debug_app {

using litestl::math::float3;
using litestl::util::Vector;

namespace {

// --- GPU fixture capture (--gpu-capture) ----------------------------------
// Serializes the exact per-binding buffer bytes a wgsl stroke uploads, plus
// the final readback, into a JSON fixture the Dawn/WebGPU replay harness feeds
// verbatim (see webgpu-verify). co/no are pre-expanded to the std430 stride-16
// layout vk_compute.cc binds, so the harness binds them byte-for-byte.
std::string b64encode(const void *src, size_t len)
{
  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const unsigned char *p = static_cast<const unsigned char *>(src);
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  for (; i + 3 <= len; i += 3) {
    uint32_t n = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) | p[i + 2];
    out.push_back(tbl[(n >> 18) & 63]);
    out.push_back(tbl[(n >> 12) & 63]);
    out.push_back(tbl[(n >> 6) & 63]);
    out.push_back(tbl[n & 63]);
  }
  if (len - i == 1) {
    uint32_t n = uint32_t(p[i]) << 16;
    out.push_back(tbl[(n >> 18) & 63]);
    out.push_back(tbl[(n >> 12) & 63]);
    out.push_back('=');
    out.push_back('=');
  } else if (len - i == 2) {
    uint32_t n = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8);
    out.push_back(tbl[(n >> 18) & 63]);
    out.push_back(tbl[(n >> 12) & 63]);
    out.push_back(tbl[(n >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

// Expand n packed xyz triples to the stride-16 (xyz + 0 pad) std430 layout,
// then base64. Mirrors BrushComputeDispatch::beginStroke's expansion so the
// harness binds the same bytes the native co/no storage buffers hold.
std::string b64Stride16(const float *packed, int n)
{
  std::vector<float> buf(size_t(n) * 4, 0.0f);
  for (int i = 0; i < n; i++) {
    buf[size_t(i) * 4 + 0] = packed[i * 3 + 0];
    buf[size_t(i) * 4 + 1] = packed[i * 3 + 1];
    buf[size_t(i) * 4 + 2] = packed[i * 3 + 2];
  }
  return b64encode(buf.data(), buf.size() * sizeof(float));
}

} // namespace

GpuStrokeSession::~GpuStrokeSession()
{
  delete disp_;
  disp_ = nullptr;
  delete normalPass_;
  normalPass_ = nullptr;
}

// Marshals the full mesh co/no/mask once, loads the SPIR-V kernel, uploads
// neighbor topology / brush texture when the kernel needs them, and opens the
// meshlog step. Per-dab dispatch reads the previous dab's result (like the C++
// executor); geometry must match the C++ path bit-modulo-fp, which
// `make.mjs sbrush-verify` asserts via the <brush>_ab.txt A/B scripts.
bool GpuStrokeSession::begin(Scene &scene, std::string &err)
{
  scene.profiler.beginStroke();
  auto ptBegin = StrokeProfiler::now();

  if (!scene.ensureGPU() || !scene.context) {
    err = "stroke(wgsl): GPU device init failed";
    return false;
  }

  switch (scene.currentTool) {
  case brush::SculptBrushes::DRAW: kernel_ = "draw"; break;
  case brush::SculptBrushes::TEXDRAW: kernel_ = "texdraw"; break;
  case brush::SculptBrushes::CLAY: kernel_ = "clay"; break;
  case brush::SculptBrushes::INFLATE: kernel_ = "inflate"; break;
  case brush::SculptBrushes::PINCH: kernel_ = "pinch"; break;
  case brush::SculptBrushes::SHARP: kernel_ = "sharp"; break;
  case brush::SculptBrushes::MASK: kernel_ = "mask"; writesMask_ = true; break;
  case brush::SculptBrushes::SMOOTH: kernel_ = "smooth"; needsNeighbors_ = true; break;
  case brush::SculptBrushes::KELVINLET: kernel_ = "kelvinlet"; break;
  case brush::SculptBrushes::POSE: kernel_ = "pose"; break;
  default:
    err = "stroke(wgsl): tool has no GPU kernel";
    return false;
  }

  mesh::Mesh *m = scene.mesh;
  vcount_ = m->v.count;

  cap_ = !capturePrefix_.empty();

  Vector<float> co, no, mask;
  co.resize(size_t(vcount_) * 3);
  no.resize(size_t(vcount_) * 3);
  mask.resize(size_t(vcount_));
  for (int i = 0; i < vcount_; i++) {
    float3 c = m->v.co[i], n = m->v.no[i];
    co[i * 3 + 0] = c[0]; co[i * 3 + 1] = c[1]; co[i * 3 + 2] = c[2];
    no[i * 3 + 0] = n[0]; no[i * 3 + 1] = n[1]; no[i * 3 + 2] = n[2];
    mask[i] = scene.tree->treeMesh.v.mask[i];
  }
  if (cap_) {
    capCo_ = b64Stride16(co.data(), vcount_);
    capNo_ = b64Stride16(no.data(), vcount_);
    capMask_ = b64encode(mask.data(), size_t(vcount_) * sizeof(float));
  }

  disp_ = new vulkan::BrushComputeDispatch(scene.context);
  std::string spv = std::string(SBRUSH_SPV_DIR) + "/" + kernel_ + ".spv";
  if (!disp_->loadSpirv(spv.c_str())) {
    err = "stroke(wgsl): failed to load " + spv;
    return false;
  }
  if (!disp_->beginStroke(co.data(), no.data(), mask.data(), vcount_)) {
    err = "stroke(wgsl): vertex upload failed";
    return false;
  }

  // CSR neighbor topology for for_neighbor kernels. Build it in the same
  // EdgeOfVertIter order the C++ kernel walks so the per-vertex `avg += nb.co`
  // accumulates identically — keeping the GPU result bit-modulo-fp identical.
  if (needsNeighbors_) {
    Vector<vulkan::ComputeVertNbr> meta;
    Vector<uint32_t> flat;
    meta.resize(vcount_);
    for (int v = 0; v < vcount_; v++) {
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
    if (!disp_->setNeighbors(meta.data(), vcount_, flat.data(), int(flat.size()))) {
      err = "stroke(wgsl): neighbor upload failed";
      return false;
    }
    if (cap_) {
      capNbrMeta_ = b64encode(meta.data(), meta.size() * sizeof(vulkan::ComputeVertNbr));
      capNbrVerts_ = b64encode(flat.data(), flat.size() * sizeof(uint32_t));
    }
  }

  // Brush texture (binding 8). The kernel multiplies strength by
  // sampleBrushTex; with no texture bound the dummy 1x1 white returns 1.0, so
  // only upload when one is set. coord_space/tex_repeat ride in on the per-dab
  // uniforms below.
  if (scene.brush.tex_width > 0 && scene.brush.tex_height > 0 &&
      scene.brush.tex_pixels.size() > 0) {
    if (!disp_->setBrushTexture(scene.brush.tex_pixels.data(),
                                scene.brush.tex_width, scene.brush.tex_height)) {
      err = "stroke(wgsl): brush texture upload failed";
      return false;
    }
    if (cap_) {
      capTexture_ = "{\"width\":" + std::to_string(scene.brush.tex_width) +
                    ",\"height\":" + std::to_string(scene.brush.tex_height) +
                    ",\"pixels\":\"" +
                    b64encode(scene.brush.tex_pixels.data(),
                              size_t(scene.brush.tex_width) * scene.brush.tex_height *
                                  sizeof(float)) +
                    "\"}";
    }
  }

  scene.meshLog.beginStep();
  scene.brush.resetStrokePath();

  // GPU-resident live-render bring-up (interactive only). Build the global
  // normal topology + per-node slot->vertex maps once, flip the GPU nodes'
  // render VBOs to gpu_owned, then do an initial normal-recompute + scatter so
  // every GPU node (even ones no dab will touch) holds valid GPU-fed data for
  // the first frame. The batch/verify path leaves liveBackend_ null and skips
  // all of this, running the original full-readback-at-end code unchanged.
  if (liveBackend_) {
    normalPass_ = new vulkan::GpuNormalPass(scene.context);
    if (!normalPass_->init()) {
      err = "stroke(wgsl): GPU normal pass init failed";
      return false;
    }
    buildNormalTopology(scene);
    for (spatial::SpatialNode *gn : scene.tree->gpu_nodes()) {
      scene.tree->buildGpuNodeSlotVertex(gn, &scene.gpu);
    }
    scene.tree->gpuStrokeActive = true;
    normalPass_->computeNormals(disp_->coBuffer(), disp_->noBuffer());
    // One-time initial scatter so every GPU node (even ones no dab touches)
    // shows GPU-fed data on the first frame. Once-per-stroke, so the simple
    // submit-per-node scatter() is fine here; per-dab scatter is batched.
    for (spatial::SpatialNode *gn : scene.tree->gpu_nodes()) {
      if (!gn->gpu_data) {
        continue;
      }
      spatial::GpuData &gd = *gn->gpu_data;
      if (!gd.pos || !gd.nor || !gd.slotVertex || gd.total_verts <= 0) {
        continue;
      }
      VkBuffer pos = liveBackend_->ensureStorageVkBuffer(gd.pos);
      VkBuffer nor = liveBackend_->ensureStorageVkBuffer(gd.nor);
      VkBuffer slotV = liveBackend_->ensureStorageVkBuffer(gd.slotVertex);
      if (!pos || !nor || !slotV) {
        continue;
      }
      normalPass_->scatter(disp_->coBuffer(), disp_->noBuffer(), slotV, pos, nor,
                           gd.total_verts);
    }
  }
  scene.profiler.addBegin(StrokeProfiler::ms(ptBegin, StrokeProfiler::now()));
  return true;
}

// Build the global triangle topology (3 vertex indices per fan-triangulated
// face) plus a vertex->incident-triangle CSR, and upload it to normalPass_.
// The mesh is static during a stroke, so this runs once at begin(). The
// triangulation matches mesh::triangulate (the render path's), not the spatial
// per-node fan — GPU normals are render/pick-only and intentionally not
// bit-identical to the CPU per-node normals.
void GpuStrokeSession::buildNormalTopology(Scene &scene)
{
  mesh::Mesh *m = scene.mesh;

  litestl::util::Vector<mesh::Tri> tris;
  mesh::triangulate(*m, litestl::util::IndexRange(0, m->f.count), tris);
  int triCount = int(tris.size());
  topoTriCount_ = triCount;

  topoTriVerts_.resize(size_t(triCount) * 3);

  // Pass 1: flatten tri vertex indices + count incident tris per vertex.
  Vector<uint32_t> counts;
  counts.resize(vcount_);
  for (int v = 0; v < vcount_; v++) {
    counts[v] = 0;
  }
  for (int t = 0; t < triCount; t++) {
    for (int j = 0; j < 3; j++) {
      int v = tris[t].v[j];
      topoTriVerts_[size_t(t) * 3 + j] = uint32_t(v);
      counts[v]++;
    }
  }

  // Pass 2: prefix-sum into a (offset,count) CSR meta, then scatter tri indices.
  topoMeta_.resize(size_t(vcount_) * 2);  // uvec2: [2v]=offset, [2v+1]=count
  uint32_t off = 0;
  for (int v = 0; v < vcount_; v++) {
    topoMeta_[size_t(v) * 2 + 0] = off;
    topoMeta_[size_t(v) * 2 + 1] = counts[v];
    off += counts[v];
  }
  topoList_.resize(off);
  Vector<uint32_t> cursor;
  cursor.resize(vcount_);
  for (int v = 0; v < vcount_; v++) {
    cursor[v] = topoMeta_[size_t(v) * 2 + 0];
  }
  for (int t = 0; t < triCount; t++) {
    for (int j = 0; j < 3; j++) {
      int v = tris[t].v[j];
      topoList_[cursor[v]++] = uint32_t(t);
    }
  }

  // Dedup stamp arrays for buildDabWork (0 = unstamped; stampGen_ starts at 1).
  triStamp_.resize(triCount);
  vertStamp_.resize(vcount_);
  for (int t = 0; t < triCount; t++) triStamp_[t] = 0;
  for (int v = 0; v < vcount_; v++) vertStamp_[v] = 0;
  stampGen_ = 0;

  normalPass_->setTopology(topoTriVerts_.data(), triCount, topoMeta_.data(),
                           vcount_, topoList_.data(), int(topoList_.size()));
}

void GpuStrokeSession::buildDabWork(const litestl::util::Vector<uint32_t> &uverts)
{
  stampGen_++;
  uint32_t gen = stampGen_;
  workTris_.clear();
  workVerts_.clear();

  // Every incident triangle of a moved vert recomputes its face normal.
  for (uint32_t v : uverts) {
    uint32_t off = topoMeta_[size_t(v) * 2 + 0];
    uint32_t cnt = topoMeta_[size_t(v) * 2 + 1];
    for (uint32_t k = 0; k < cnt; k++) {
      uint32_t t = topoList_[off + k];
      if (triStamp_[t] != gen) {
        triStamp_[t] = gen;
        workTris_.append(t);
      }
    }
  }
  // Each touched triangle's three verts then re-sum their vertex normal.
  for (uint32_t t : workTris_) {
    for (int j = 0; j < 3; j++) {
      uint32_t v = topoTriVerts_[size_t(t) * 3 + j];
      if (vertStamp_[v] != gen) {
        vertStamp_[v] = gen;
        workVerts_.append(v);
      }
    }
  }
  // The vert pass re-sums each work vert's normal over its *full* incident-face
  // ring (the CSR), but the face pass above only refreshes triNo for faces that
  // touch a moved vert. A work vert on the boundary of that region has incident
  // faces outside workTris_ whose triNo would be stale (or uninitialized garbage
  // on the first dab) — corrupting the summed vertex normal. Expand workTris_ to
  // cover every incident face of every work vert so all triNo a work vert reads
  // are freshly computed. workVerts_ is left unchanged: only verts adjacent to
  // motion need their normal recomputed; the extra faces exist solely to give
  // those verts a complete, fresh 1-ring. (Iterate by index — workVerts_ is not
  // grown here, but appending to workTris_ must not alias the loop range.)
  int boundaryStart = int(workVerts_.size());
  for (int i = 0; i < boundaryStart; i++) {
    uint32_t v = workVerts_[i];
    uint32_t off = topoMeta_[size_t(v) * 2 + 0];
    uint32_t cnt = topoMeta_[size_t(v) * 2 + 1];
    for (uint32_t k = 0; k < cnt; k++) {
      uint32_t t = topoList_[off + k];
      if (triStamp_[t] != gen) {
        triStamp_[t] = gen;
        workTris_.append(t);
      }
    }
  }
}

// Capture a node's pre-dab co/no/f.no into the meshlog, once per node per
// stroke (live path). Mirrors the snapshot block the batch path runs in end(),
// but here it must fire before the per-dab partial readback overwrites
// m->v.co — so we snapshot on a node's first touch, while its verts still hold
// the pre-stroke state.
void GpuStrokeSession::snapshotNode(Scene &scene, spatial::SpatialNode *node)
{
  if (scene.meshLog.hasSimpleChunk(node->id)) {
    return;
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

bool GpuStrokeSession::dab(Scene &scene, float3 origin, float3 normal,
                           std::string &err)
{
  // Phase timestamps for --profile (now() is cheap; addDab() no-ops when off).
  // cpu = pt0..ptCpu (marshal/work-list/target resolve), gpu = ptCpu..ptGpu
  // (the runOneShot submit + queue-wait), read = ptGpu..ptRead (live readback).
  auto pt0 = StrokeProfiler::now();
  StrokeProfiler::Clock::time_point ptCpu = pt0, ptGpu = pt0, ptRead = pt0;

  Vector<spatial::SpatialNode *> nodes;
  scene.tree->filterNodes(origin, scene.brush.radius, nodes);
  if (nodes.size() == 0) {
    return true;
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
    // Live path: capture this node's pre-dab state for undo the first time it
    // is touched, before the partial readback below overwrites m->v.co.
    if (liveBackend_) {
      snapshotNode(scene, node);
    }
    touched_.append(node);
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

  // Kelvinlet host stage (clampParams) is C++-only — never lowered to WGSL —
  // so replicate it here before marshaling mu/nu (mirrors kelvinlet.sbrush).
  if (scene.currentTool == brush::SculptBrushes::KELVINLET) {
    if (scene.brush.nu > 0.499f) scene.brush.nu = 0.499f;
    if (scene.brush.nu < 0.0f) scene.brush.nu = 0.0f;
    if (scene.brush.mu < 1e-6f) scene.brush.mu = 1e-6f;
    bu.mu = scene.brush.mu;
    bu.nu = scene.brush.nu;
  }

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

  // Global-brush ctx tail (offset 96): kelvinlet grab vectors or pose cage.
  if (scene.currentTool == brush::SculptBrushes::KELVINLET) {
    for (int i = 0; i < 3; i++) {
      cu.global.kelvinlet.grabFrom[i] = scene.brush.grabFrom[i];
      cu.global.kelvinlet.grabTo[i] = scene.brush.grabTo[i];
    }
  } else if (scene.currentTool == brush::SculptBrushes::POSE) {
    for (int a = 0; a < 4; a++) {
      for (int i = 0; i < 3; i++) {
        cu.global.pose.poseCageRest[a][i] = scene.brush.poseCageRest[a][i];
        cu.global.pose.poseCageNow[a][i] = scene.brush.poseCageNow[a][i];
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

  // GPU-resident live path: batch the brush dab and the localized normal
  // recompute into a single submit (dab -> barrier -> face -> barrier -> vert),
  // so a dab costs one queue-wait instead of three-plus. The batch/verify path
  // keeps the simple one-submit-per-dab dab() call so sbrush-verify is
  // unaffected.
  if (liveBackend_) {
    if (!disp_->prepareDab(bu, cu, uverts.data(), int(uverts.size()),
                           chunks.data(), int(chunks.size()),
                           scene.brush.falloff_curve.data(), sp.data(),
                           int(sp.size()))) {
      err = "stroke(wgsl): compute dispatch failed";
      return false;
    }
    buildDabWork(uverts);
    normalPass_->prepareNormals(disp_->coBuffer(), disp_->noBuffer(),
                                workTris_.data(), int(workTris_.size()),
                                workVerts_.data(), int(workVerts_.size()));

    // Resolve each touched GPU owner's render VBOs up front (host-side; may
    // create/upload buffers) so the scatter dispatches can ride this dab's
    // single submit instead of each costing its own queue-wait.
    struct ScatterTarget {
      VkBuffer pos, nor, slotV;
      int count;
    };
    Vector<ScatterTarget> targets;
    Vector<spatial::SpatialNode *> owners;
    for (auto *node : nodes) {
      spatial::SpatialNode *owner = scene.tree->find_gpu_owner(node);
      if (!owner || owners.contains(owner)) {
        continue;
      }
      owners.append(owner);
      if (!owner->gpu_data) {
        continue;
      }
      spatial::GpuData &gd = *owner->gpu_data;
      if (!gd.pos || !gd.nor || !gd.slotVertex || gd.total_verts <= 0) {
        continue;
      }
      VkBuffer pos = liveBackend_->ensureStorageVkBuffer(gd.pos);
      VkBuffer nor = liveBackend_->ensureStorageVkBuffer(gd.nor);
      VkBuffer slotV = liveBackend_->ensureStorageVkBuffer(gd.slotVertex);
      if (!pos || !nor || !slotV) {
        continue;
      }
      targets.append({pos, nor, slotV, gd.total_verts});
    }

    // dab -> (barrier) -> face/vert normals -> (barrier) -> per-owner scatter,
    // all in one command buffer / one queue-wait.
    normalPass_->beginScatterBatch();
    ptCpu = StrokeProfiler::now();
    scene.context->runOneShot([&](VkCommandBuffer cb) {
      disp_->recordDab(cb);
      vulkan::GpuNormalPass::computeBarrier(cb);
      normalPass_->recordNormals(cb);
      vulkan::GpuNormalPass::computeBarrier(cb);
      for (auto &t : targets) {
        normalPass_->recordScatter(cb, disp_->coBuffer(), disp_->noBuffer(),
                                   t.slotV, t.pos, t.nor, t.count);
      }
    });
    ptGpu = StrokeProfiler::now();
  } else {
    ptCpu = StrokeProfiler::now();
    if (!disp_->dab(bu, cu, uverts.data(), int(uverts.size()), chunks.data(),
                    int(chunks.size()), scene.brush.falloff_curve.data(),
                    sp.data(), int(sp.size()))) {
      err = "stroke(wgsl): compute dispatch failed";
      return false;
    }
    ptGpu = StrokeProfiler::now();
  }
  // Default the readback phase to empty; the live block below extends it.
  ptRead = ptGpu;

  // Live path: the dab/normals/scatter above already deformed the render VBOs
  // on the GPU this frame. Read back only this dab's moved verts into the CPU
  // mesh so ray-pick + node bounds stay correct for the next dab. Touched leaves
  // get RegenBounds only — never UpdateGPU/UpdateNormals: the scatter owns the
  // VBOs and the normals until stroke end.
  if (liveBackend_) {
    int n = int(uverts.size());
    Vector<float> coBack, noBack;
    coBack.resize(size_t(n) * 3);
    noBack.resize(size_t(n) * 3);
    if (disp_->readbackVerts(uverts.data(), n, coBack.data(), noBack.data())) {
      mesh::Mesh *m = scene.mesh;
      for (int i = 0; i < n; i++) {
        int v = int(uverts[i]);
        m->v.co[v] = float3(coBack[i * 3 + 0], coBack[i * 3 + 1], coBack[i * 3 + 2]);
        m->v.no[v] = float3(noBack[i * 3 + 0], noBack[i * 3 + 1], noBack[i * 3 + 2]);
      }
    }
    for (auto *node : nodes) {
      node->update(spatial::Spatial_RegenBounds);
    }
    ptRead = StrokeProfiler::now();
  }

  scene.profiler.addDab(StrokeProfiler::ms(pt0, ptCpu),
                        StrokeProfiler::ms(ptCpu, ptGpu),
                        StrokeProfiler::ms(ptGpu, ptRead));

  if (cap_) {
    // falloff_curve is the 256-entry LUT the dab() contract expects.
    std::string d = "{\"nodeCount\":" + std::to_string(chunks.size()) +
                    ",\"unique\":\"" +
                    b64encode(uverts.data(), uverts.size() * sizeof(uint32_t)) +
                    "\",\"nodes\":\"" +
                    b64encode(chunks.data(),
                              chunks.size() * sizeof(vulkan::ComputeNodeMeta)) +
                    "\",\"brushU\":\"" + b64encode(&bu, sizeof(bu)) +
                    "\",\"ctxU\":\"" + b64encode(&cu, sizeof(cu)) +
                    "\",\"falloff\":\"" +
                    b64encode(scene.brush.falloff_curve.data(), 256 * sizeof(float)) +
                    "\",\"stroke\":\"" +
                    b64encode(sp.data(), sp.size() * sizeof(vulkan::ComputeStrokeSample)) +
                    "\"}";
    capDabs_.push_back(std::move(d));
  }
  return true;
}

void GpuStrokeSession::end(Scene &scene)
{
  auto ptEnd0 = StrokeProfiler::now();
  mesh::Mesh *m = scene.mesh;

  Vector<float> coOut, maskOut;
  coOut.resize(size_t(vcount_) * 3);
  if (writesMask_) {
    maskOut.resize(size_t(vcount_));
  }
  disp_->endStroke(coOut.data(), nullptr, writesMask_ ? maskOut.data() : nullptr);

  if (cap_) {
    // One fixture per wgsl stroke; a script with several strokes suffixes
    // .<n>.json after the first so none clobber the others.
    static int captureSeq = 0;
    std::string path = capturePrefix_;
    if (captureSeq > 0) {
      path += "." + std::to_string(captureSeq);
    }
    path += ".json";
    captureSeq++;

    std::string j = "{\n";
    j += "  \"kernel\": \"" + std::string(kernel_) + "\",\n";
    j += "  \"vertCount\": " + std::to_string(vcount_) + ",\n";
    j += "  \"hasNeighbors\": " + std::string(needsNeighbors_ ? "true" : "false") + ",\n";
    j += "  \"writesMask\": " + std::string(writesMask_ ? "true" : "false") + ",\n";
    j += "  \"co\": \"" + capCo_ + "\",\n";
    j += "  \"no\": \"" + capNo_ + "\",\n";
    j += "  \"mask\": \"" + capMask_ + "\",\n";
    j += "  \"nbrMeta\": " + (capNbrMeta_.empty() ? "null" : "\"" + capNbrMeta_ + "\"") + ",\n";
    j += "  \"nbrVerts\": " + (capNbrVerts_.empty() ? "null" : "\"" + capNbrVerts_ + "\"") + ",\n";
    j += "  \"texture\": " + (capTexture_.empty() ? "null" : capTexture_) + ",\n";
    j += "  \"dabs\": [";
    for (size_t i = 0; i < capDabs_.size(); i++) {
      j += (i ? ",\n    " : "\n    ") + capDabs_[i];
    }
    j += capDabs_.empty() ? "]" : "\n  ]";
    j += ",\n";
    j += "  \"expectCo\": \"" +
         b64encode(coOut.data(), size_t(vcount_) * 3 * sizeof(float)) + "\",\n";
    j += "  \"expectMask\": " +
         (writesMask_ ? "\"" + b64encode(maskOut.data(), size_t(vcount_) * sizeof(float)) + "\""
                      : std::string("null")) +
         "\n";
    j += "}\n";

    std::FILE *fp = std::fopen(path.c_str(), "wb");
    if (fp) {
      std::fwrite(j.data(), 1, j.size(), fp);
      std::fclose(fp);
    }
  }

  if (liveBackend_) {
    finishLive(scene, coOut, maskOut);
    scene.meshLog.endStep();
    delete disp_;
    disp_ = nullptr;
    delete normalPass_;
    normalPass_ = nullptr;
    scene.profiler.addEnd(StrokeProfiler::ms(ptEnd0, StrokeProfiler::now()));
    scene.profiler.endStroke();
    return;
  }

  // Snapshot pre-stroke node state for undo (mesh.v.co is still pre-stroke
  // here), mirroring the emitted `*Pre` stage, then write the GPU result.
  for (auto *node : touched_) {
    snapshotNode(scene, node);
  }

  for (int i = 0; i < vcount_; i++) {
    m->v.co[i] = float3(coOut[i * 3 + 0], coOut[i * 3 + 1], coOut[i * 3 + 2]);
  }
  // Mask paints the .spatial.v.mask attribute, not geometry; write it back to
  // the tree mesh. (Matches the C++ Mask brush, whose *Pre snapshots co/no but
  // not mask, so neither path restores mask on undo.)
  if (writesMask_) {
    for (int i = 0; i < vcount_; i++) {
      scene.tree->treeMesh.v.mask[i] = maskOut[i];
    }
  }
  for (auto *node : touched_) {
    node->update(spatial::Spatial_UpdateNormals | spatial::Spatial_UpdateGPU |
                 spatial::Spatial_RegenBounds);
  }

  scene.meshLog.endStep();

  delete disp_;
  disp_ = nullptr;
  scene.profiler.addEnd(StrokeProfiler::ms(ptEnd0, StrokeProfiler::now()));
  scene.profiler.endStroke();
}

// Finalize the GPU-resident live stroke: sync the full CPU mesh from the final
// GPU readback, resolve normals per the end-mode toggle, hand the render VBOs
// back to the CPU draw path, and queue the right dirty bits. Per-dab readback
// already kept touched verts in sync for picking; this guarantees everything
// (untouched-by-readback verts, mask, normals) is consistent at release.
void GpuStrokeSession::finishLive(Scene &scene, litestl::util::Vector<float> &coOut,
                                  litestl::util::Vector<float> &maskOut)
{
  mesh::Mesh *m = scene.mesh;

  for (int i = 0; i < vcount_; i++) {
    m->v.co[i] = float3(coOut[i * 3 + 0], coOut[i * 3 + 1], coOut[i * 3 + 2]);
  }
  if (writesMask_) {
    for (int i = 0; i < vcount_; i++) {
      scene.tree->treeMesh.v.mask[i] = maskOut[i];
    }
  }

  const bool gpuNormals = scene.strokeEndNormals == StrokeEndNormals::Gpu;
  if (gpuNormals) {
    // Cheaper release: take the GPU's global-sum vertex normals straight back
    // (skips the CPU 1-ring gather) and recompute only face normals on the CPU
    // — flat-shaded render needs m->f.no, and a face normal is one cross per
    // tri, no gather. Not bit-identical to the CPU path; that is the trade.
    Vector<uint32_t> allv;
    allv.resize(vcount_);
    for (int i = 0; i < vcount_; i++) {
      allv[i] = uint32_t(i);
    }
    Vector<float> noOut;
    noOut.resize(size_t(vcount_) * 3);
    if (disp_->readbackVerts(allv.data(), vcount_, nullptr, noOut.data())) {
      for (int i = 0; i < vcount_; i++) {
        m->v.no[i] = float3(noOut[i * 3 + 0], noOut[i * 3 + 1], noOut[i * 3 + 2]);
      }
    }
    // node->data->tris only ever holds the node's own unique_faces' tris, so
    // summing within a node and normalizing its unique_faces needs no
    // ownership gating (mirrors update_node_normals' face half).
    for (auto *node : touched_) {
      for (int f : node->unique_faces()) {
        m->f.no[f].zero();
      }
      for (const auto &tri : node->data->tris) {
        int v1 = m->c.v[tri.c[0]];
        int v2 = m->c.v[tri.c[1]];
        int v3 = m->c.v[tri.c[2]];
        m->f.no[tri.f] += litestl::math::triNormal(m->v.co[v1], m->v.co[v2],
                                                   m->v.co[v3]);
      }
      for (int f : node->unique_faces()) {
        m->f.no[f].normalize();
      }
    }
  }

  // Hand every GPU node's render VBOs back to the CPU draw path: drop
  // gpu_owned, mark update_buffer so the host data re-uploads, and dispose the
  // slot maps. Untouched nodes' host data is still the pre-stroke flat soup
  // (correct, since they never moved); touched nodes are refreshed below via
  // UpdateGPU. Done for all GPU nodes because the begin() initial scatter
  // flipped them all to gpu_owned.
  scene.tree->gpuStrokeActive = false;
  for (spatial::SpatialNode *gn : scene.tree->gpu_nodes()) {
    if (!gn->gpu_data) {
      continue;
    }
    spatial::GpuData &gd = *gn->gpu_data;
    if (gd.pos) {
      gd.pos->gpu_owned = false;
      gd.pos->update_buffer = true;
    }
    if (gd.nor) {
      gd.nor->gpu_owned = false;
      gd.nor->update_buffer = true;
    }
    if (gd.slotVertex) {
      scene.gpu.destroyBuffer(gd.slotVertex);
      gd.slotVertex = nullptr;
    }
  }

  // Cpu (default): the CPU normal recompute makes m->v.no/m->f.no byte-identical
  // to the C++ backend. Gpu: normals already resolved above, so skip
  // UpdateNormals and only refresh the render soup from the synced CPU mesh.
  for (auto *node : touched_) {
    if (gpuNormals) {
      node->update(spatial::Spatial_UpdateGPU | spatial::Spatial_RegenBounds);
    } else {
      node->update(spatial::Spatial_UpdateNormals | spatial::Spatial_UpdateGPU |
                   spatial::Spatial_RegenBounds);
    }
  }
}

} // namespace sculptcore::debug_app

#endif // SBRUSH_GPU_DISPATCH
