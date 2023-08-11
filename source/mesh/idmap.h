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

  IDMap(Mesh *mesh_, ElemType mask_ = VERTEX | EDGE | FACE) : m(mesh_), mask(mask_)
  {
    for (int i = 0; i < 32; i++) {
      id_attrs[i] = nullptr;
    }

    if (mask & VERTEX) {
      vert_id.ensure(mesh_->v.attrs);
      id_attrs[VERTEX] = vert_id.get_data<int>();
    }

    if (mask & EDGE) {
      edge_id.ensure(mesh_->e.attrs);
      id_attrs[EDGE] = edge_id.get_data<int>();
    }

    if (mask & CORNER) {
      corner_id.ensure(mesh_->c.attrs);
      id_attrs[CORNER] = corner_id.get_data<int>();
    }

    if (mask & FACE) {
      face_id.ensure(mesh_->f.attrs);
      id_attrs[FACE] = face_id.get_data<int>();
    }
  }

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
    id_attr<elem_type>[elem] = id;

    return id;
  }

  template <ElemType elem_type> void free_id(int elem)
  {
    int id = id_attr<elem_type>[elem];

    id_attr<elem_type>[elem] = ID_NONE;
    idmap[id] = -1;
  }

  void regen_idmap(ElemType mask)
  {
    max_id = 0;

    auto dispatch = [&](auto cb) {
      if (mask & VERTEX) {
        cb(VERTEX, m->v);
      }
      if (mask & EDGE) {
        cb(EDGE, m->e);
      }
      if (mask & CORNER) {
        cb(CORNER, m->c);
      }
      if (mask & FACE) {
        cb(FACE, m->f);
      }
    };

    dispatch([&](ElemType elem_type, auto &list) {
      AttrData<int> &idattr = id_attr<elem_type>();

      for (int elem : list) {
        int id = idattr[elem];

        max_id = std::max(max_id, id);
      }
    });

    max_id++;
    idmap.resize(max_id);
    for (int i = 0; i < max_id; i++) {
      idmap[i] = -1;
    }

    dispatch([&](ElemType elem_type, auto &list) {
      AttrData<int> &idattr = this->id_attr<elem_type>();

      for (int elem : list) {
        int id = idattr[elem];

        if (id == ID_NONE) {
          alloc_id<elem_type>(elem);
        } else if (idmap[id] != -1) {
          printf("duplicate id detected\n");
          idattr[elem] = ID_NONE;

          alloc_id<elem_type>(elem);
        } else {
          idmap[id] = elem;
        }
      }
    });
  }
};
} // namespace sculptcore::mesh