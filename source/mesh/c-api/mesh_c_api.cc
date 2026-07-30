#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "mesh/attr_weights.h"
#include "mesh/attribute.h"
#include "mesh/attribute_bool.h"
#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "mesh/mesh_serialize.h"
#include "mesh/utils/triangulate.h"
#include "mesh/uvgen.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

using namespace sculptcore::mesh;
using namespace litestl::util;
using namespace sculptcore;
using namespace litestl;

using math::float3;

#ifdef WASM
#include <emscripten/bind.h>
using namespace emscripten;
#endif

/** Visit every corner in Mesh_toArrays' export order: face live-iteration
 * order, walking each face's (outer) corner cycle. Corner element-index order
 * only coincides with this on a freshly built mesh — topology edits fragment
 * corner ids — so every corner-domain marshal in this file must use THIS
 * order, or a post-dyntopo flush writes each attribute onto the wrong Blender
 * loop. */
template <typename Fn> static void mesh_foreach_corner_export_order(Mesh *m, Fn fn)
{
  for (int fi : m->f) {
    const int li = m->f.l[fi];
    if (li == ELEM_NONE) {
      continue;
    }
    const int c0 = m->l.c[li];
    int cc = c0;
    do {
      fn(cc);
      cc = m->c.next[cc];
    } while (cc != c0);
  }
}

extern "C" {

/* String API */
const char *getStrData(const string *str)
{
  return str->c_str();
}

/* Mesh API */
Mesh *createMesh()
{
  return alloc::New<Mesh>("Mesh");
}

void freeMesh(Mesh *mesh)
{
  alloc::Delete<Mesh>(mesh);
}

/* Fan-triangulate every n-gon of @p mesh in place (via the public Euler ops, so
 * n_ngon_faces stays current). Triangle-only consumers (dyntopo) then skip their
 * per-dab triangulate prepass. Does NOT thread MeshCallbacks — callers rebuild
 * the spatial tree afterwards. No-op-safe on null / already-triangle meshes. */
void Mesh_triangulate(Mesh *mesh)
{
  if (mesh) {
    (void)triangulateMesh(*mesh);
  }
}

/* Live n-gon (>3-sided face) count; 0 == all-triangles. Exact — the counter is
 * maintained at make_face/kill_face and resynced by recountNgons() on bulk load.
 * Lets JS gate the triangulate button / dyntopo tip with no face scan. */
int Mesh_ngonFaceCount(Mesh *mesh)
{
  return mesh ? mesh->ngonFaceCount() : 0;
}

int makeVertex(Mesh *mesh, float x, float y, float z)
{
  float3 co(x, y, z);

  return mesh->make_vertex(co);
}

/* Serialize @p mesh into a freshly-allocated buffer; *out_size receives the
 * byte count. Free the result with freeMeshBuffer. Returns nullptr on failure. */
uint8_t *serializeMesh(Mesh *mesh, int *out_size)
{
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  if (!serial::writeMesh(*mesh, ss)) {
    *out_size = 0;
    return nullptr;
  }

  std::string s = ss.str();
  uint8_t *buf = static_cast<uint8_t *>(alloc::alloc("mesh serialize buffer", s.size()));
  std::memcpy(buf, s.data(), s.size());
  *out_size = int(s.size());
  return buf;
}

/* Serialize @p mesh into a freshly-allocated buffer holding only the
 * uncompressed column payload (serial::writeMeshRaw) — no lz4 step, no BinFile
 * header. The autosave worker compresses + frames this off-thread via the JS
 * lz4 codec. *out_size receives the byte count; free with freeMeshBuffer.
 * Returns nullptr on failure. */
uint8_t *serializeMeshRaw(Mesh *mesh, int *out_size)
{
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  if (!serial::writeMeshRaw(*mesh, ss)) {
    *out_size = 0;
    return nullptr;
  }

  std::string s = ss.str();
  uint8_t *buf = static_cast<uint8_t *>(alloc::alloc("mesh serialize raw buffer", s.size()));
  std::memcpy(buf, s.data(), s.size());
  *out_size = int(s.size());
  return buf;
}

/* Deserialize a buffer produced by serializeMesh into a fresh Mesh. Returns
 * nullptr (and frees the partial mesh) on failure. */
Mesh *deserializeMesh(const uint8_t *data, int size)
{
  std::string s(reinterpret_cast<const char *>(data), size_t(size));
  std::stringstream ss(s, std::ios::in | std::ios::out | std::ios::binary);

  Mesh *mesh = alloc::New<Mesh>("Mesh");
  if (!serial::readMesh(*mesh, ss)) {
    alloc::Delete<Mesh>(mesh);
    return nullptr;
  }
  return mesh;
}

void freeMeshBuffer(uint8_t *buf)
{
  if (buf) {
    alloc::release(static_cast<void *>(buf));
  }
}

AttrRef *copyAttrRef(const AttrRef &ref)
{
  return alloc::New<AttrRef>("AttrRef", ref);
}

void freeAttrRef(AttrRef *ref)
{
  alloc::Delete<AttrRef>(ref);
}

AttrRef *getAttr(Mesh *mesh, AttrType type, string name, ElemType domain)
{
  switch (domain) {
  case VERTEX:
    return copyAttrRef(mesh->v.attrs.find_attribute(type, name));
  case EDGE:
    return copyAttrRef(mesh->e.attrs.find_attribute(type, name));
  case CORNER:
    return copyAttrRef(mesh->c.attrs.find_attribute(type, name));
  case LIST:
    return copyAttrRef(mesh->l.attrs.find_attribute(type, name));
  case FACE:
    return copyAttrRef(mesh->f.attrs.find_attribute(type, name));
  }

  return nullptr;
}

string *getAttrName(AttrRef *ref)
{
  return &ref->name;
}

/* Does not allocate memory. */
AttrRef *getAttrs(Mesh *mesh, ElemType domain, int *count_out)
{
  AttrGroup *attrs = nullptr;

  switch (domain) {
  case VERTEX:
    attrs = &mesh->v.attrs;
    break;
  case EDGE:
    attrs = &mesh->e.attrs;
    break;
  case CORNER:
    attrs = &mesh->c.attrs;
    break;
  case LIST:
    attrs = &mesh->l.attrs;
    break;
  case FACE:
    attrs = &mesh->f.attrs;
    break;
  }

  if (!attrs) {
    fprintf(stderr, "Unknown domain %d\n", domain);
    fflush(stderr);
    return nullptr;
  }

  *count_out = int(attrs->attrs.size());
  return attrs->attrs.data();
}

/** Read a named FLOAT vertex attribute into `out` (Mesh_arraySizes' verts_num
 * live-order values, matching Mesh_toArrays). Returns 1 when the attribute
 * exists (out filled), 0 otherwise (out untouched). Used to pull mask
 * (`.spatial.v.mask`) back to the Blender mesh. */
int Mesh_readVertFloatAttr(Mesh *m, const char *name, float *out)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }
  AttrRef ref = m->v.attrs.find_attribute(AttrType::FLOAT, name);
  if (!ref.exists()) {
    return 0;
  }
  auto *data = static_cast<AttrData<float> *>(ref.data);
  int i = 0;
  for (int vi : m->v) {
    out[i++] = data->safe_get(vi);
  }
  return 1;
}

/** Write `in` (verts_num live-order values) into a named FLOAT vertex
 * attribute, creating it if missing. Used to seed mask (`.spatial.v.mask`)
 * from the Blender mesh on enter. Returns 1. */
int Mesh_writeVertFloatAttr(Mesh *m, const char *name, const float *in)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }
  AttrRef &ref = m->v.attrs.ensure(AttrType::FLOAT, name, /*materialize=*/true);
  auto *data = static_cast<AttrData<float> *>(ref.data);
  int i = 0;
  for (int vi : m->v) {
    data->materialize(vi);
    (*data)[vi] = in[i++];
  }
  return 1;
}

/** Read a named FLOAT4 vertex attribute into `out` (verts_num * 4 floats,
 * live-vert order). Returns 1 when the attribute exists, 0 otherwise. Used to
 * pull vertex colors (the `color` attr) back to the Blender mesh. */
int Mesh_readVertFloat4Attr(Mesh *m, const char *name, float *out)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }
  AttrRef ref = m->v.attrs.find_attribute(AttrType::FLOAT4, name);
  if (!ref.exists()) {
    return 0;
  }
  auto *data = static_cast<AttrData<math::float4> *>(ref.data);
  int i = 0;
  for (int vi : m->v) {
    math::float4 c = data->safe_get(vi);
    out[i * 4] = c[0];
    out[i * 4 + 1] = c[1];
    out[i * 4 + 2] = c[2];
    out[i * 4 + 3] = c[3];
    i++;
  }
  return 1;
}

/** Write `in` (verts_num * 4 floats) into a named FLOAT4 vertex attribute,
 * creating it if missing. Used to seed vertex colors (`color`) from the
 * Blender mesh on enter. Returns 1. */
int Mesh_writeVertFloat4Attr(Mesh *m, const char *name, const float *in)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }
  AttrRef &ref = m->v.attrs.ensure(AttrType::FLOAT4, name, /*materialize=*/true);
  auto *data = static_cast<AttrData<math::float4> *>(ref.data);
  int i = 0;
  for (int vi : m->v) {
    data->materialize(vi);
    (*data)[vi] = math::float4(in[i * 4], in[i * 4 + 1], in[i * 4 + 2], in[i * 4 + 3]);
    i++;
  }
  return 1;
}

/** Write a named FLOAT2 corner (loop) attribute from `in` (per-corner, in the
 * mesh's corner/loop order — matching the corner_verts passed to
 * Mesh_fromArrays). Used to seed a UV map for the external draw provider. */
int Mesh_writeCornerFloat2Attr(Mesh *m, const char *name, const float *in)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }
  AttrRef &ref = m->c.attrs.ensure(AttrType::FLOAT2, name, /*materialize=*/true);
  // Tag as UV so every UV consumer (chart derivation, slide-reprojection, the
  // draw provider) sees the seeded layer.
  ref.use = AttrUse(int(ref.use) | int(AttrUse::UV));
  auto *data = static_cast<AttrData<math::float2> *>(ref.data);
  int i = 0;
  mesh_foreach_corner_export_order(m, [&](int ci) {
    data->materialize(ci);
    (*data)[ci] = math::float2(in[i * 2], in[i * 2 + 1]);
    i++;
  });
  // Rewriting a UV layer stales every derived `.boundary.edge.uvchart` flag;
  // mark the whole mesh boundary-dirty so the next recompute re-derives them.
  boundary::markAllDirty(m);
  return 1;
}

/** Read a named INT face attribute into `out` (Mesh_arraySizes' faces_num
 * live-order values, matching Mesh_toArrays' face order). Returns 1 when the
 * attribute exists, 0 otherwise. Used to pull face sets (the `group` attr)
 * back to the Blender mesh. */
int Mesh_readFaceIntAttr(Mesh *m, const char *name, int *out)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }
  AttrRef ref = m->f.attrs.find_attribute(AttrType::INT, name);
  if (!ref.exists()) {
    return 0;
  }
  auto *data = static_cast<AttrData<int> *>(ref.data);
  int i = 0;
  for (int fi : m->f) {
    out[i++] = data->safe_get(fi);
  }
  return 1;
}

/** Write `in` (faces_num live-order values) into a named INT face attribute,
 * creating it if missing. Used to seed face sets (`group`) from the Blender
 * mesh on enter. Returns 1. */
int Mesh_writeFaceIntAttr(Mesh *m, const char *name, const int *in)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }
  AttrRef &ref = m->f.attrs.ensure(AttrType::INT, name, /*materialize=*/true);
  auto *data = static_cast<AttrData<int> *>(ref.data);
  int i = 0;
  for (int fi : m->f) {
    data->materialize(fi);
    (*data)[fi] = in[i++];
  }
  return 1;
}

/** Resolve an ElemType domain flag (VERTEX/EDGE/CORNER/LIST/FACE) to its
 * ElemData. Returns null for an unknown flag. Shared by the generic-attribute
 * bridge below (the round-trip of arbitrary user layers). */
static ElemData *mesh_elem_domain(Mesh *m, int domain)
{
  switch (ElemType(domain)) {
  case VERTEX:
    return &m->v;
  case EDGE:
    return &m->e;
  case CORNER:
    return &m->c;
  case LIST:
    return &m->l;
  case FACE:
    return &m->f;
  }
  return nullptr;
}

/** Read a named attribute of arbitrary `type` on `domain` into `out`
 * (one element per live element of the domain, in the same live-iteration order
 * as Mesh_toArrays / Mesh_arraySizes' per-domain counts). `out` must hold
 * `count * sizeof(element)` bytes for the type. Returns 1 when
 * the attribute exists (out filled), 0 otherwise (out untouched). The generic
 * counterpart of the typed Mesh_read*Attr functions, used to round-trip every
 * user layer back to the Blender mesh after a topology rebuild.
 *
 * AttrType::WEIGHTS is refused: its elements are DeformPool indices, which mean
 * nothing outside this mesh. Use the sc_mesh_weights_* entry points. */
int Mesh_readAttr(Mesh *m, int domain, const char *name, int type, void *out)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }
  ElemData *ed = mesh_elem_domain(m, domain);
  if (!ed) {
    return 0;
  }
  AttrType attr_type = AttrType(type);
  if (attr_type == AttrType::WEIGHTS) {
    return 0;
  }
  AttrRef ref = ed->attrs.find_attribute(attr_type, name);
  if (!ref.exists()) {
    return 0;
  }
  sculptcore::mesh::detail::type_dispatch(attr_type, [&]<typename T>() {
    int i = 0;
    auto readOne = [&](int elem) {
      if constexpr (std::is_same_v<T, bool>) {
        static_cast<uint8_t *>(out)[i++] =
            static_cast<BoolAttrView *>(ref.data)->get(elem) ? 1 : 0;
      } else {
        static_cast<T *>(out)[i++] = static_cast<AttrData<T> *>(ref.data)->safe_get(elem);
      }
    };
    if (ElemType(domain) == CORNER) {
      mesh_foreach_corner_export_order(m, readOne);
    } else {
      for (int elem : *ed) {
        readOne(elem);
      }
    }
  });
  return 1;
}

/** Write `in` (one element per live element of the domain, live-iteration
 * order) into a named attribute of arbitrary `type` on `domain`, creating it
 * with `use`
 * (an AttrUse — UV/COLOR/... — so a re-imported UV map stays a UV map) if it is
 * missing. The generic counterpart of the typed Mesh_write*Attr functions, used
 * to seed every user layer into the engine on enter. Returns 1.
 *
 * AttrType::WEIGHTS is refused: a raw write would store caller-supplied
 * DeformPool indices with no reference taken, so the pool would free runs the
 * column still names. Use the sc_mesh_weights_* entry points. */
int Mesh_writeAttr(Mesh *m, int domain, const char *name, int type, int use, const void *in)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }
  ElemData *ed = mesh_elem_domain(m, domain);
  if (!ed) {
    return 0;
  }
  AttrType attr_type = AttrType(type);
  if (attr_type == AttrType::WEIGHTS) {
    return 0;
  }
  AttrRef &ref = ed->attrs.ensure(attr_type, name, /*materialize=*/true);
  ref.use = AttrUse(use);
  sculptcore::mesh::detail::type_dispatch(attr_type, [&]<typename T>() {
    int i = 0;
    auto writeOne = [&](int elem) {
      if constexpr (std::is_same_v<T, bool>) {
        static_cast<BoolAttrView *>(ref.data)->set(
            elem, static_cast<const uint8_t *>(in)[i++] != 0);
      } else {
        auto *data = static_cast<AttrData<T> *>(ref.data);
        data->materialize(elem);
        (*data)[elem] = static_cast<const T *>(in)[i++];
      }
    };
    if (ElemType(domain) == CORNER) {
      mesh_foreach_corner_export_order(m, writeOne);
    } else {
      for (int elem : *ed) {
        writeOne(elem);
      }
    }
  });
  return 1;
}

/** Total influences across every live vertex of the `VERT_WEIGHTS` layer: the
 * value sc_mesh_weights_get writes to `offsets[vert_count]`, and the length the
 * `group_ids` / `weights` arrays need. 0 when the mesh carries no such layer. */
int sc_mesh_weights_element_count(Mesh *m)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }
  WeightsRef w = findVertWeights(*m, VERT_WEIGHTS);
  if (!w.exists()) {
    return 0;
  }
  int total = 0;
  for (int vi : m->v) {
    total += w.runSize(vi);
  }
  return total;
}

/** Read the weights layer as CSR — `offsets` takes `vert_count + 1` entries in
 * live-vertex order (Mesh_toArrays' order), `group_ids` and `weights` take
 * sc_mesh_weights_element_count entries. CSR rather than a call per vertex
 * because crossing the ctypes boundary is the expensive part.
 *
 * Returns 1, or 0 without writing anything when there is no weights layer. */
int sc_mesh_weights_get(Mesh *m, int *offsets, int *group_ids, float *weights)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }
  WeightsRef w = findVertWeights(*m, VERT_WEIGHTS);
  if (!w.exists()) {
    return 0;
  }

  Vector<DeformWeight> run;
  int i = 0, out = 0;
  for (int vi : m->v) {
    offsets[i++] = out;
    const int n = w.runSize(vi);
    if (n > 0) {
      run.resize(n);
      w.getRun(vi, run.data(), n);
      for (int k = 0; k < n; k++) {
        group_ids[out] = run[k].group;
        weights[out] = run[k].weight;
        out++;
      }
    }
  }
  offsets[i] = out;
  return 1;
}

/** Write the weights layer from CSR arrays in the layout and order
 * sc_mesh_weights_get produces, creating the layer (and the mesh's DeformPool)
 * on first use. Every live vertex is written, so a vertex whose run is empty is
 * cleared rather than left alone. Runs need not be sorted or deduplicated —
 * interning canonicalizes them. Returns the number of vertices written. */
int sc_mesh_weights_set(Mesh *m,
                        const int *offsets,
                        const int *group_ids,
                        const float *weights)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }
  WeightsRef w = ensureVertWeights(*m, VERT_WEIGHTS);

  Vector<DeformWeight> run;
  int i = 0;
  for (int vi : m->v) {
    const int start = offsets[i], end = offsets[i + 1];
    i++;
    run.clear();
    for (int k = start; k < end; k++) {
      run.append(DeformWeight{group_ids[k], weights[k]});
    }
    w.setRun(vi, span<const DeformWeight>(run.data(), size_t(run.size())));
  }
  return i;
}

/** How many vertex-group names the mesh carries, i.e. how many NUL-terminated
 * strings sc_mesh_weight_groups_get writes. 0 when the mesh has no pool. */
int sc_mesh_weight_group_count(Mesh *m)
{
  DeformPool *pool = m->deformPoolOrNull();
  return pool ? int(pool->group_names.size()) : 0;
}

/** Copy the group names into `buf` as NUL-terminated strings packed back to
 * back, and return the byte count that takes. Call it once with `buf` null to
 * size the buffer; a `buf_size` too small writes nothing and returns the same
 * count. */
int sc_mesh_weight_groups_get(Mesh *m, char *buf, int buf_size)
{
  DeformPool *pool = m->deformPoolOrNull();
  if (!pool) {
    return 0;
  }

  int need = 0;
  for (const string &name : pool->group_names) {
    need += int(name.size()) + 1;
  }
  if (!buf || buf_size < need) {
    return need;
  }

  int at = 0;
  for (const string &name : pool->group_names) {
    std::memcpy(buf + at, name.c_str(), name.size() + 1);
    at += int(name.size()) + 1;
  }
  return need;
}

/** Replace the name table with `count` NUL-terminated strings packed back to
 * back in `buf`. A DeformWeight::group is an index into this list, so its order
 * is the whole contract between host and engine — write it before the weights,
 * and reconcile by name, never by position, across a mode change. */
void sc_mesh_weight_groups_set(Mesh *m, const char *buf, int count)
{
  DeformPool &pool = m->deformPool();
  pool.group_names.clear();

  const char *p = buf;
  for (int i = 0; i < count; i++) {
    pool.group_names.append(string(p));
    p += std::strlen(p) + 1;
  }
}

/** Live edge count (sizes Mesh_readEdgeFlags buffers). */
int Mesh_edgeCount(Mesh *m)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }
  return m->e.count;
}

/** Set the named boundary bool edge flag (e.g. `.boundary.edge.seam`) from
 * per-edge vertex pairs: `edge_verts` holds `edges_num * 2` engine vertex
 * indices, `values` one byte per edge. Edges are resolved by endpoint lookup —
 * the caller's edge order need not match the engine's. A false value is only
 * written when the layer already exists (an all-false write on a fresh mesh
 * creates nothing). Marks the affected elements boundary-dirty; call
 * Mesh_recomputeBoundary (or rely on the executors' lazy recompute) afterwards.
 * Returns the number of edges applied. */
int Mesh_writeEdgeFlagsByVerts(
    Mesh *m, const char *name, const int *edge_verts, const uint8_t *values, int edges_num)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }
  bool haveLayer = boundary::findBoolEdgeView(m, name) != nullptr;
  int applied = 0;
  for (int i = 0; i < edges_num; i++) {
    const bool state = values[i] != 0;
    if (!state && !haveLayer) {
      continue;
    }
    const int e = m->find_edge(edge_verts[i * 2], edge_verts[i * 2 + 1]);
    if (e == ELEM_NONE) {
      continue;
    }
    boundary::setEdgeFlag(m, name, e, state);
    haveLayer = true;
    applied++;
  }
  return applied;
}

/** Read the named boundary bool edge flag: fills `r_edge_verts` with the
 * engine vertex pair of every flagged live edge (up to `max_edges` pairs;
 * size with Mesh_edgeCount). Returns the flagged-edge count, 0 when the layer
 * doesn't exist. Engine vertex indices — map them through Mesh_toArrays'
 * vert_map after a topology change. */
int Mesh_readEdgeFlags(Mesh *m, const char *name, int *r_edge_verts, int max_edges)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }
  BoolAttrView *view = boundary::findBoolEdgeView(m, name);
  if (!view) {
    return 0;
  }
  int found = 0;
  for (int e : m->e) {
    if (!view->get(e)) {
      continue;
    }
    if (found < max_edges) {
      r_edge_verts[found * 2] = m->e.vs[e][0];
      r_edge_verts[found * 2 + 1] = m->e.vs[e][1];
    }
    found++;
  }
  return found;
}

/** Fold pending boundary edits into the derived flags + per-vertex
 * classification (boundary::recomputeDirty; needs live topology). */
void Mesh_recomputeBoundary(Mesh *m)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }
  boundary::recomputeDirty(m);
}

/** generateUVFromSeams into a caller-named corner layer (created or
 * overwritten in place, tagged AttrUse::UV) — unlike the reflected
 * Mesh::generateUVFromSeams, which always allocates a fresh unique name.
 * `margin_milli` is the pre-pack chart padding in 1/1000 UV units. Returns the
 * chart count (0 = no faces, no layer created). The derived UV-chart boundary
 * flags are rebuilt afterwards (a wholesale UV rewrite dirties nothing on its
 * own), so the constraint system sees the new charts immediately. */
int Mesh_generateUVFromSeams(Mesh *m, const char *name, int margin_milli)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }
  const int charts = generateUVFromSeams(m, name, float(margin_milli) / 1000.0f);
  if (charts > 0) {
    boundary::markAllDirty(m);
    boundary::recomputeDirty(m);
  }
  return charts;
}

/** Monotonic topology-edit stamp (bumped by every make_/kill_/reorder_ op).
 * Snapshot it after building/importing; an unchanged stamp at flush/exit
 * means original indices are still valid — the positions-only fast path. */
uint64_t Mesh_topoStamp(Mesh *mesh)
{
  return mesh->topo_stamp;
}

/* Bulk array conversion (Blender layout) --------------------------------- */

/** Build a mesh from flat arrays in Blender's native layout: `positions` is
 * `verts_num * 3` floats, `corner_verts` holds every face's vertex indices
 * back to back, and `face_offsets` gives `faces_num + 1` offsets into it.
 * Edges derive from the face corners (make_face); vertices referenced by no
 * face stay as loose vertices. Invalid/degenerate faces are skipped (count
 * reported on stderr). Returns null when the offsets don't match
 * `corners_num`. */
Mesh *Mesh_fromArrays(const float *positions,
                      int verts_num,
                      const int *corner_verts,
                      int corners_num,
                      const int *face_offsets,
                      int faces_num)
{
  if (faces_num > 0 && face_offsets[faces_num] != corners_num) {
    fprintf(stderr,
            "Mesh_fromArrays: face_offsets[%d]=%d != corners_num=%d\n",
            faces_num,
            face_offsets[faces_num],
            corners_num);
    return nullptr;
  }

  Mesh *m = alloc::New<Mesh>("Mesh from arrays");

  Vector<int> vmap;
  vmap.resize(verts_num);
  for (int i = 0; i < verts_num; i++) {
    vmap[i] = m->make_vertex(
        float3(positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]));
  }

  int skipped = 0;
  Vector<int> vs;
  for (int fi = 0; fi < faces_num; fi++) {
    const int start = face_offsets[fi];
    const int end = face_offsets[fi + 1];

    bool valid = end - start >= 3;
    vs.clear();
    for (int k = start; k < end && valid; k++) {
      const int vi = corner_verts[k];
      valid = vi >= 0 && vi < verts_num;
      if (valid) {
        vs.append(vmap[vi]);
      }
    }

    if (!valid || m->make_face(std::span<int>(vs.data(), size_t(vs.size()))) == ELEM_NONE) {
      skipped++;
    }
  }
  if (skipped) {
    fprintf(stderr, "Mesh_fromArrays: skipped %d invalid/degenerate face(s)\n", skipped);
  }
  return m;
}

/** Element counts for sizing Mesh_toArrays buffers. `r_verts_domain_size`
 * (optional) receives the vert index-space size (freelist gaps included) for
 * sizing the Mesh_toArrays remap table. */
void Mesh_arraySizes(
    Mesh *m, int *r_verts_num, int *r_corners_num, int *r_faces_num, int *r_verts_domain_size)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }
  *r_verts_num = m->v.count;
  *r_faces_num = m->f.count;
  int corners = 0;
  for (int fi : m->f) {
    const int li = m->f.l[fi];
    if (li != ELEM_NONE) {
      corners += m->l.size[li];
    }
  }
  *r_corners_num = corners;
  if (r_verts_domain_size) {
    *r_verts_domain_size = int(m->v.capacity());
  }
}

/** Compact-aware export into caller-allocated arrays (sizes from
 * Mesh_arraySizes; same layout as Mesh_fromArrays, `face_offsets` gets
 * `faces_num + 1` entries). Vertices export in live-iteration order.
 * `r_vert_map` (optional, `r_verts_domain_size` entries) receives engine
 * vert index -> exported index, ELEM_NONE for dead slots — every attribute
 * copy and undo coupling must flow through it. Returns 1 when freelist gaps
 * forced a real remap, 0 when the map is identity. */
int Mesh_toArrays(
    Mesh *m, float *positions, int *corner_verts, int *face_offsets, int *r_vert_map)
{
  if (m->topo_frozen) {
    m->thawTopo();
  }

  const int domain_size = int(m->v.capacity());
  Vector<int> local_map;
  int *vmap = r_vert_map;
  if (vmap == nullptr) {
    local_map.resize(domain_size);
    vmap = local_map.data();
  }
  for (int i = 0; i < domain_size; i++) {
    vmap[i] = ELEM_NONE;
  }

  int out_v = 0;
  bool gaps = false;
  for (int vi : m->v) {
    if (vi != out_v) {
      gaps = true;
    }
    const float3 co = m->v.co[vi];
    positions[out_v * 3] = co[0];
    positions[out_v * 3 + 1] = co[1];
    positions[out_v * 3 + 2] = co[2];
    vmap[vi] = out_v++;
  }

  int out_c = 0;
  int out_f = 0;
  for (int fi : m->f) {
    face_offsets[out_f++] = out_c;
    const int li = m->f.l[fi];
    if (li == ELEM_NONE) {
      continue;
    }
    const int c0 = m->l.c[li];
    int cc = c0;
    do {
      corner_verts[out_c++] = vmap[m->c.v[cc]];
      cc = m->c.next[cc];
    } while (cc != c0);
  }
  face_offsets[out_f] = out_c;

  return gaps ? 1 : 0;
}
}

#if 0 // def WASM
#include "../mesh.h"
namespace sculptcore::mesh {

EMSCRIPTEN_BINDINGS(mesh)
{
  class_<Mesh>("Mesh")
    .constructor<>()
    .function("makeVertex", &Mesh::make_vertex, allow_raw_pointers())
    .function("getAttr", &getAttr, allow_raw_pointers())
    .function("getAttrs", &getAttrs, allow_raw_pointers());
}
}
#endif
