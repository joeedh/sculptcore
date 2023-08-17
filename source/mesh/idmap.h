#include "attribute_builtin.h"
#include "mesh.h"

#include "math/vector.h"

#include "util/boolvector.h"
#include "util/map.h"
#include "util/vector.h"

#include "cmath"

namespace sculptcore::mesh {
#define ID_NONE 0

struct IDMap {
  BuiltinAttr<int, ".id.vert"> vert_id;
  BuiltinAttr<int, ".id.edge"> edge_id;
  BuiltinAttr<int, ".id.corner"> corner_id;
  BuiltinAttr<int, ".id.face"> face_id;

  AttrData<int> *id_attrs[32];

  util::Vector<int> freelist;
  util::Vector<int> idmap;

  ElemType mask;

  int max_id = 0;
  Mesh *m;

  IDMap(Mesh *mesh_, ElemType mask_ = VERTEX | EDGE | FACE);

  /*
  We do not remove callbacks (e.g. m->v.on_swap)
  inside the destructor since we cannot assume the Mesh
  still exists there.
  */
  void unlink_callbacks();

  template <ElemType elem_type> AttrData<int> &id_attr()
  {
    return *id_attrs[int(elem_type)];
  }

  template <ElemType elem_type> int alloc_id(int elem)
  {
    int id;

    if (freelist.size()) {
      id = freelist.pop_back();
    } else {
      id = max_id++;

      if (id >= idmap.size()) {
        idmap.resize(id + 1);
      }
    }

    idmap[id] = elem;
    id_attr<elem_type>()[elem] = id;

    return id;
  }

  template <ElemType elem_type> void free_id(int elem)
  {
    int id = id_attr<elem_type>[elem];

    id_attr<elem_type>[elem] = ID_NONE;
    idmap[id] = -1;
  }

  void regen_idmap(ElemType mask);
};
} // namespace sculptcore::mesh