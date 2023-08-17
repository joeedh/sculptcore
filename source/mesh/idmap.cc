#include "idmap.h"

namespace sculptcore::mesh {
IDMap::IDMap(Mesh *mesh_, ElemType mask_) : m(mesh_), mask(mask_)
{
  for (int i = 0; i < 32; i++) {
    id_attrs[i] = nullptr;
  }

  auto make_on_swap = [this]<ElemType elem_type>() {
    return [this](Mesh *m, int v1, int v2) {
      AttrData<int> &attr = id_attr<elem_type>();

      int id1 = attr[v1], id2 = attr[v2];
      std::swap(idmap[id1], idmap[id2]);
    };
  };

  if (mask & VERTEX) {
    vert_id.ensure(mesh_->v.attrs);
    id_attrs[VERTEX] = vert_id.get_data<int>();

    IDMap *mythis = this;

    mesh_->v.on_swap.add(make_on_swap.operator()<VERTEX>(), this);
  }

  if (mask & EDGE) {
    edge_id.ensure(mesh_->e.attrs);
    id_attrs[EDGE] = edge_id.get_data<int>();

    mesh_->e.on_swap.add(make_on_swap.operator()<EDGE>(), this);
  }

  if (mask & CORNER) {
    corner_id.ensure(mesh_->c.attrs);
    id_attrs[CORNER] = corner_id.get_data<int>();

    mesh_->c.on_swap.add(make_on_swap.operator()<CORNER>(), this);
  }

  if (mask & FACE) {
    face_id.ensure(mesh_->f.attrs);
    id_attrs[FACE] = face_id.get_data<int>();

    mesh_->f.on_swap.add(make_on_swap.operator()<FACE>(), this);
  }
}

void IDMap::unlink_callbacks()
{
  if (mask & VERTEX) {
    m->v.on_swap.remove(this);
  }
  if (mask & EDGE) {
    m->e.on_swap.remove(this);
  }
  if (mask & CORNER) {
    m->c.on_swap.remove(this);
  }
  if (mask & FACE) {
    m->f.on_swap.remove(this);
  }
}

void IDMap::regen_idmap(ElemType mask)
{
  max_id = 0;

  auto dispatch = [&](auto cb) {
    if (mask & VERTEX) {
      cb.template operator()<VERTEX>(m->v);
    }
    if (mask & EDGE) {
      cb.template operator()<EDGE>(m->e);
    }
    if (mask & CORNER) {
      cb.template operator()<CORNER>(m->c);
    }
    if (mask & FACE) {
      cb.template operator()<FACE>(m->f);
    }
  };

  dispatch([&]<ElemType elem_type>(auto &list) {
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

  dispatch([&]<ElemType elem_type>(auto &list) {
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
} // namespace sculptcore::mesh
