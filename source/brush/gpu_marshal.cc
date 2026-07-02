#include "gpu_marshal.h"

#include "brush/brush.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/utils/triangulate.h"
#include "meshlog/attr_saver.h"
#include "meshlog/meshlog.h"
#include "spatial/node.h"
#include "spatial/spatial.h"

#include "litestl/util/index_range.h"

#include <cstddef>
#include <cstring>

namespace sculptcore::brush {

using litestl::math::float3;
using litestl::util::Vector;

/** Tool -> kernel capability map. One table for every dispatcher (debug app +
 * app seam); add a row here to light a brush up on the GPU. */
static const GpuKernelInfo kGpuKernels[] = {
    {.tool = SculptBrushes::DRAW, .kernel = "draw", .accumulable = true},
    {.tool = SculptBrushes::TEXDRAW, .kernel = "texdraw", .accumulable = true},
    // Clay family all runs the `plane` kernel (planeoff/planeSide select the
    // variant), mirroring brush_executor's createPlaneBrush dispatch.
    {.tool = SculptBrushes::CLAY, .kernel = "plane", .accumulable = true},
    {.tool = SculptBrushes::SCRAPE, .kernel = "plane", .accumulable = true},
    {.tool = SculptBrushes::FILL, .kernel = "plane", .accumulable = true},
    {.tool = SculptBrushes::INFLATE, .kernel = "inflate", .accumulable = true},
    {.tool = SculptBrushes::PINCH, .kernel = "pinch", .accumulable = true},
    {.tool = SculptBrushes::SHARP, .kernel = "sharp", .accumulable = true},
    {.tool = SculptBrushes::MASK, .kernel = "mask", .writesMask = true},
    {.tool = SculptBrushes::SMOOTH,
     .kernel = "smooth",
     .needsNeighbors = true,
     .accumulable = true},
    {.tool = SculptBrushes::KELVINLET,
     .kernel = "kelvinlet",
     .isGlobal = true,
     .grabMode = true},
    {.tool = SculptBrushes::POSE, .kernel = "pose", .isGlobal = true},
    {.tool = SculptBrushes::COLOR, .kernel = "color", .writesColor = true},
    {.tool = SculptBrushes::POLYGROUP, .kernel = "polygroup", .faceMode = true},
    {.tool = SculptBrushes::BSMOOTH,
     .kernel = "bsmooth",
     .needsNeighbors = true,
     .accumulable = true,
     .readsVclass = true},
};

const GpuKernelInfo *gpuKernelForTool(SculptBrushes tool)
{
  for (const GpuKernelInfo &k : kGpuKernels) {
    if (k.tool == tool) {
      return &k;
    }
  }
  return nullptr;
}

void applyGpuHostClamps(SculptBrushes tool, Brush &brush)
{
  // Kelvinlet's host stage (clampParams) is C++-only — never lowered to a GPU
  // backend — so replicate it before mu/nu are marshaled (mirrors
  // kelvinlet.sbrush).
  if (tool == SculptBrushes::KELVINLET) {
    if (brush.nu > 0.499f) brush.nu = 0.499f;
    if (brush.nu < 0.0f) brush.nu = 0.0f;
    if (brush.mu < 1e-6f) brush.mu = 1e-6f;
  }
}

void packBrushUniforms(Brush &brush, SculptBrushes tool, bool nonaccum,
                       ComputeBrushUniforms &out)
{
  const GpuKernelInfo *info = gpuKernelForTool(tool);

  out = ComputeBrushUniforms();
  out.strength = brush.strength;
  out.radius = brush.radius;
  out.spacing = brush.spacing;
  out.invert = brush.invert ? 1u : 0u;
  out.falloff_kind = uint32_t(brush.falloff_kind);
  out.falloff_shape = uint32_t(brush.falloff_shape);
  out.falloff_dir[0] = brush.falloff_dir[0];
  out.falloff_dir[1] = brush.falloff_dir[1];
  out.falloff_dir[2] = brush.falloff_dir[2];
  out.falloff_extent[0] = brush.falloff_extent[0];
  out.falloff_extent[1] = brush.falloff_extent[1];
  out.falloff_extent[2] = brush.falloff_extent[2];
  out.coord_space = uint32_t(brush.coord_space);
  out.tex_repeat = brush.tex_repeat;
  out.stroke_path_count = uint32_t(brush.strokePathCount);
  out.nonaccum = (nonaccum && info && info->accumulable) ? 1u : 0u;

  // POLYGROUP's `activeGroup` is the first appended DSL uniform, at offset 72 —
  // the slot the host struct gives kelvinlet's `mu`. Mutually exclusive
  // brushes, so the i32 bits are written into the f32 `mu` as a
  // bit-reinterpret (WGSL reads `activeGroup` as i32). FRAGILE: only works
  // while `mu` is the first post-fixed field — the static_assert pins that. If
  // another DSL-uniform brush is added, give it its own named slot instead.
  static_assert(offsetof(ComputeBrushUniforms, mu) == 72,
                "polygroup activeGroup aliases the first appended DSL uniform "
                "slot (offset 72); update this if the layout changes");
  switch (tool) {
  case SculptBrushes::POLYGROUP: {
    int ag = brush.activeGroup;
    std::memcpy(&out.mu, &ag, sizeof(int));
    break;
  }
  case SculptBrushes::KELVINLET:
    applyGpuHostClamps(tool, brush);
    out.mu = brush.mu;
    out.nu = brush.nu;
    break;
  case SculptBrushes::PINCH:
  case SculptBrushes::SHARP:
    // The `@static` `pinch` uniform is the first appended DSL slot (offset 72,
    // aliasing mu). Without this the kernel reads mu's 1.0 default.
    out.pinch = brush.pinch;
    break;
  case SculptBrushes::CLAY:
  case SculptBrushes::SCRAPE:
  case SculptBrushes::FILL:
    out.planeoff = brush.planeoff;
    out.planeSide = brush.planeSide;
    break;
  case SculptBrushes::COLOR:
    for (int i = 0; i < 4; i++) {
      out.brushColor[i] = brush.brushColor[i];
    }
    break;
  default:
    break;
  }
}

void packCtxUniforms(const Brush &brush, SculptBrushes tool, const float3 &origin,
                     const float3 &normal, const float *renderMatrixRowMajor,
                     ComputeCtxUniforms &out)
{
  out = ComputeCtxUniforms();
  out.surfacePos[0] = origin[0];
  out.surfacePos[1] = origin[1];
  out.surfacePos[2] = origin[2];
  out.surfaceNo[0] = normal[0];
  out.surfaceNo[1] = normal[1];
  out.surfaceNo[2] = normal[2];

  // litestl::math::Matrix's backing buffer is row-major, but WGSL mat4x4 /
  // std140 storage is column-major — transpose on the way out or the two
  // backends read different matrices (VIEWPLANE/VIEWREPEAT texture space).
  if (renderMatrixRowMajor) {
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 4; c++) {
        out.render_matrix[c * 4 + r] = renderMatrixRowMajor[r * 4 + c];
      }
    }
  }

  // Global-brush ctx tail (offset 96): kelvinlet grab vectors or pose cage.
  if (tool == SculptBrushes::KELVINLET) {
    for (int i = 0; i < 3; i++) {
      out.global.kelvinlet.grabFrom[i] = brush.grabFrom[i];
      out.global.kelvinlet.grabTo[i] = brush.grabTo[i];
    }
  } else if (tool == SculptBrushes::POSE) {
    for (int a = 0; a < 4; a++) {
      for (int i = 0; i < 3; i++) {
        out.global.pose.poseCageRest[a][i] = brush.poseCageRest[a][i];
        out.global.pose.poseCageNow[a][i] = brush.poseCageNow[a][i];
      }
    }
  }
}

void packStrokePath(const Brush &brush, Vector<ComputeStrokeSample> &out)
{
  out.clear();
  for (int i = 0; i < brush.strokePathCount; i++) {
    const auto &s = brush.strokePath[i];
    ComputeStrokeSample o;
    o.pos[0] = s.pos[0];
    o.pos[1] = s.pos[1];
    o.pos[2] = s.pos[2];
    o.normal[0] = s.normal[0];
    o.normal[1] = s.normal[1];
    o.normal[2] = s.normal[2];
    o.arclen = s.arclen;
    out.append(o);
  }
}

int packGeometry(mesh::Mesh &m, spatial::SpatialTree *tree, bool faceMode,
                 Vector<float> &co, Vector<float> &no, Vector<float> &mask)
{
  if (faceMode) {
    int faceCount = m.f.count;
    co.resize(size_t(faceCount) * 3);
    no.resize(size_t(faceCount) * 3);
    mask.resize(size_t(faceCount));
    for (int fi = 0; fi < faceCount; fi++) {
      mesh::FaceProxy fp(&m, fi);
      float3 ctr = fp.calc_center();
      float3 fn = m.f.no[fi];
      co[fi * 3 + 0] = ctr[0];
      co[fi * 3 + 1] = ctr[1];
      co[fi * 3 + 2] = ctr[2];
      no[fi * 3 + 0] = fn[0];
      no[fi * 3 + 1] = fn[1];
      no[fi * 3 + 2] = fn[2];
      mask[fi] = 0.0f;
    }
    return faceCount;
  }

  int vcount = m.v.count;
  co.resize(size_t(vcount) * 3);
  no.resize(size_t(vcount) * 3);
  mask.resize(size_t(vcount));
  for (int i = 0; i < vcount; i++) {
    float3 c = m.v.co[i], n = m.v.no[i];
    co[i * 3 + 0] = c[0];
    co[i * 3 + 1] = c[1];
    co[i * 3 + 2] = c[2];
    no[i * 3 + 0] = n[0];
    no[i * 3 + 1] = n[1];
    no[i * 3 + 2] = n[2];
    mask[i] = tree ? tree->treeMesh.v.mask[i] : 0.0f;
  }
  return vcount;
}

void packNeighborCSR(mesh::Mesh &m, int vcount, Vector<ComputeVertNbr> &meta,
                     const uint32_t **flatVerts, int *flatCount)
{
  // Sourced from the shared frozen-topology MeshTopoCache (same EdgeOfVertIter
  // walk as the C++ kernel), so per-vertex accumulation order matches and the
  // build amortizes across strokes.
  m.topo_cache.ensureRing1(m);
  mesh::VertNbrCSR &csr = m.topo_cache.ring1;
  meta.resize(vcount);
  for (int v = 0; v < vcount; v++) {
    uint32_t off = csr.offsets[v];
    meta[v].offset = off;
    meta[v].count = csr.offsets[v + 1] - off;
  }
  *flatVerts = reinterpret_cast<const uint32_t *>(csr.nbr_verts.data());
  *flatCount = int(csr.nbr_verts.size());
}

void snapshotNodeForUndo(meshlog::MeshLog &log, spatial::SpatialNode *node)
{
  auto *mm = node->data->m;
  const int sid = log.curStrokeId();

  // Vertex co/no: element-keyed AttrSaver gate (survives dyntopo
  // restructuring), appending touched verts into the per-step element store.
  // Mirrors the generated CPU *Pre legacy-default save set.
  {
    meshlog::AttrSaver<mesh::ElemType::VERTEX> saver;
    saver.ensure(*mm);
    mesh::AttrRef refs[2] = {mm->v.co, mm->v.no};
    int mask = saver.add(refs[0], meshlog::CO) | saver.add(refs[1], meshlog::NO);
    auto *store = log.elemStore(mesh::ElemType::VERTEX);
    litestl::util::span<const mesh::AttrRef> span(refs, 2);
    for (int e : node->unique_verts()) {
      if (saver.needsData(e, sid, mask)) {
        store->data.appendFrom(mm->v.attrs, e, span);
        saver.updateSaved(e, sid, mask);
      }
    }
  }
  // Face normals.
  {
    meshlog::AttrSaver<mesh::ElemType::FACE> saver;
    saver.ensure(*mm);
    mesh::AttrRef refs[1] = {mm->f.no};
    int mask = saver.add(refs[0], meshlog::NO);
    auto *store = log.elemStore(mesh::ElemType::FACE);
    litestl::util::span<const mesh::AttrRef> span(refs, 1);
    for (int e : node->unique_faces()) {
      if (saver.needsData(e, sid, mask)) {
        store->data.appendFrom(mm->f.attrs, e, span);
        saver.updateSaved(e, sid, mask);
      }
    }
  }
}

void chunkNodes(const Vector<spatial::SpatialNode *> &nodes, bool faceMode,
                Vector<uint32_t> &uverts, Vector<ComputeNodeMeta> &chunks)
{
  uverts.clear();
  chunks.clear();
  for (spatial::SpatialNode *node : nodes) {
    // Both element kinds are OrderedSet<int> of global indices, flattened into
    // uverts + 64-wide NodeMeta chunks (the field is named vert_* but is just
    // offset/count — faces in face mode).
    auto &uv = faceMode ? node->unique_faces() : node->unique_verts();
    Vector<int> idx;
    for (int gi : uv) {
      idx.append(gi);
    }
    int n = int(idx.size());
    for (int written = 0; written < n; written += 64) {
      int cnt = (n - written < 64) ? (n - written) : 64;
      ComputeNodeMeta meta;
      meta.vert_offset = uint32_t(uverts.size());
      meta.vert_count = uint32_t(cnt);
      for (int k = 0; k < cnt; k++) {
        uverts.append(uint32_t(idx[written + k]));
      }
      chunks.append(meta);
    }
  }
}

void GpuNormalTopology::build(mesh::Mesh &m)
{
  vcount = m.v.count;

  litestl::util::Vector<mesh::Tri> tris;
  mesh::triangulate(m, litestl::util::IndexRange(0, m.f.count), tris);
  triCount = int(tris.size());

  triVerts.resize(size_t(triCount) * 3);

  // Pass 1: flatten tri vertex indices + count incident tris per vertex.
  Vector<uint32_t> counts;
  counts.resize(vcount);
  for (int v = 0; v < vcount; v++) {
    counts[v] = 0;
  }
  for (int t = 0; t < triCount; t++) {
    for (int j = 0; j < 3; j++) {
      int v = tris[t].v[j];
      triVerts[size_t(t) * 3 + j] = uint32_t(v);
      counts[v]++;
    }
  }

  // Pass 2: prefix-sum into (offset,count) CSR meta, then scatter tri ids.
  meta.resize(size_t(vcount) * 2);
  uint32_t off = 0;
  for (int v = 0; v < vcount; v++) {
    meta[size_t(v) * 2 + 0] = off;
    meta[size_t(v) * 2 + 1] = counts[v];
    off += counts[v];
  }
  list.resize(off);
  Vector<uint32_t> cursor;
  cursor.resize(vcount);
  for (int v = 0; v < vcount; v++) {
    cursor[v] = meta[size_t(v) * 2 + 0];
  }
  for (int t = 0; t < triCount; t++) {
    for (int j = 0; j < 3; j++) {
      int v = tris[t].v[j];
      list[cursor[v]++] = uint32_t(t);
    }
  }

  // Dedup stamp arrays for dabWork (0 = unstamped; stampGen_ starts at 1).
  triStamp_.resize(triCount);
  vertStamp_.resize(vcount);
  for (int t = 0; t < triCount; t++) triStamp_[t] = 0;
  for (int v = 0; v < vcount; v++) vertStamp_[v] = 0;
  stampGen_ = 0;
}

void GpuNormalTopology::dabWork(const Vector<uint32_t> &uverts,
                                Vector<uint32_t> &workTris,
                                Vector<uint32_t> &workVerts)
{
  stampGen_++;
  uint32_t gen = stampGen_;
  workTris.clear();
  workVerts.clear();

  // Every incident triangle of a moved vert recomputes its face normal.
  for (uint32_t v : uverts) {
    uint32_t off = meta[size_t(v) * 2 + 0];
    uint32_t cnt = meta[size_t(v) * 2 + 1];
    for (uint32_t k = 0; k < cnt; k++) {
      uint32_t t = list[off + k];
      if (triStamp_[t] != gen) {
        triStamp_[t] = gen;
        workTris.append(t);
      }
    }
  }
  // Each touched triangle's three verts then re-sum their vertex normal.
  for (uint32_t t : workTris) {
    for (int j = 0; j < 3; j++) {
      uint32_t v = triVerts[size_t(t) * 3 + j];
      if (vertStamp_[v] != gen) {
        vertStamp_[v] = gen;
        workVerts.append(v);
      }
    }
  }
  // Boundary expansion (see the doc comment): give every work vert a complete,
  // fresh 1-ring of face normals. Iterate by index — appending to workTris
  // must not alias the loop range.
  int boundaryStart = int(workVerts.size());
  for (int i = 0; i < boundaryStart; i++) {
    uint32_t v = workVerts[i];
    uint32_t off = meta[size_t(v) * 2 + 0];
    uint32_t cnt = meta[size_t(v) * 2 + 1];
    for (uint32_t k = 0; k < cnt; k++) {
      uint32_t t = list[off + k];
      if (triStamp_[t] != gen) {
        triStamp_[t] = gen;
        workTris.append(t);
      }
    }
  }
}

} // namespace sculptcore::brush
