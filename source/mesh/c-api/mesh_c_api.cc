#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "mesh/attribute.h"
#include "mesh/mesh.h"
#include "mesh/mesh_serialize.h"

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
