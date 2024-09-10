#include "mesh_c_api.h"

#include "litestl/math/vector.h"
#include "mesh/attribute.h"
#include "mesh/mesh.h"
#include "litestl/util/alloc.h"
#include "litestl/util/string.h"

#include <cstdio>

using namespace sculptcore::mesh;
using namespace litestl::util;
using namespace sculptcore;
using namespace litestl;

using math::float3;

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
}
