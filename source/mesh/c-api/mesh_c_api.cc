#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "mesh/attribute.h"
#include "mesh/mesh.h"

#include <cstdio>
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

void *memAlloc(const char *tag, size_t size)
{
  return alloc::alloc(tag, size);
}

void memRelease(void *mem)
{
  alloc::release(mem);
}
void printMemBlocks()
{
  alloc::print_blocks();
}
}

void memAlloc2(const std::string &tag, int size)
{
  std::string *cpy = new std::string(tag);
  alloc::alloc(cpy->c_str(), size);
}

#if 0 //def WASM
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
