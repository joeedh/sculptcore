#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "mesh/attribute.h"
#include "mesh/mesh.h"
#include "mesh/mesh_serialize.h"
#include "mesh/utils/triangulate.h"

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
