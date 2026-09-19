#include "meshlog_base.h"

namespace sculptcore::meshlog {

  void MeshLog::selectionBeginStep()
  {
    beginStep(false);
  }


  void MeshLog::selectionEndStep()
  {
    endStep();
  }


  LogElemKind MeshLog::selectDomainKind(int domain)
  {
    switch (domain) {
    case 0:
      return LogElemKind::Vert;
    case 1:
      return LogElemKind::Edge;
    default:
      return LogElemKind::Face;
    }
  }


  void MeshLog::selectOne(mesh::Mesh *m, int domain, int idx, bool state)
  {
    if (!m || idx < 0) {
      return;
    }
    getTopoChunk()->onChange(selectDomainKind(domain), m, idx);
    switch (domain) {
    case 0:
      m->v.select.set(idx, state);
      break;
    case 1:
      m->e.select.set(idx, state);
      break;
    case 2:
      m->f.select.set(idx, state);
      break;
    }
  }


  void MeshLog::selectIndices(mesh::Mesh *m, int domain, util::Vector<int> &indices, int state)
  {
    if (!m) {
      return;
    }
    if (m->topo_frozen) {
      m->thawTopo();
    }
    bool s = state != 0;
    for (int idx : indices) {
      selectOne(m, domain, idx, s);
    }
  }


  void MeshLog::selectAllElems(mesh::Mesh *m, int domain, int state)
  {
    if (!m) {
      return;
    }
    if (m->topo_frozen) {
      m->thawTopo();
    }
    bool s = state != 0;
    switch (domain) {
    case 0:
      for (int i : m->v) {
        selectOne(m, 0, i, s);
      }
      break;
    case 1:
      for (int i : m->e) {
        selectOne(m, 1, i, s);
      }
      break;
    case 2:
      for (int i : m->f) {
        selectOne(m, 2, i, s);
      }
      break;
    }
  }


  int MeshLog::selectShortestPath(mesh::Mesh *m, int vEnd, int state)
  {
    if (!m || active_vert_ < 0 || vEnd < 0) {
      if (m && vEnd >= 0) {
        active_vert_ = vEnd;
      }
      return 0;
    }
    util::Vector<int> path;
    if (!mesh::shortestEdgePath(m, active_vert_, vEnd, path) || path.size() < 2) {
      active_vert_ = vEnd;
      return 0;
    }
    if (m->topo_frozen) {
      m->thawTopo();
    }
    bool s = state != 0;
    for (int v : path) {
      selectOne(m, 0, v, s);
    }
    for (int i = 0; i + 1 < int(path.size()); i++) {
      int e = m->find_edge(path[i], path[i + 1]);
      if (e != ELEM_NONE) {
        selectOne(m, 1, e, s);
      }
    }
    active_vert_ = vEnd;
    return int(path.size());
  }


  int MeshLog::selectLoop(mesh::Mesh *m, int seedEdge, int kind, int state)
  {
    if (!m || seedEdge < 0) {
      return 0;
    }
    if (m->topo_frozen) {
      m->thawTopo();
    }
    util::Vector<int> elems;
    int domain = kind == 2 ? 2 : 1;
    if (kind == 2) {
      mesh::walkFaceLoop(*m, seedEdge, elems);
    } else if (kind == 0) {
      mesh::walkEdgeLoop(*m, seedEdge, elems);
    } else {
      mesh::walkEdgeRing(*m, seedEdge, elems);
    }
    bool s = state != 0;
    if (s && elems.size() > 0) {
      bool all = true;
      for (int el : elems) {
        if (!m->elemSelected(domain, el)) {
          all = false;
          break;
        }
      }
      if (all) {
        s = false;
      }
    }
    for (int el : elems) {
      selectOne(m, domain, el, s);
    }
    return s ? int(elems.size()) : -int(elems.size());
  }


  void MeshLog::selectFromSets(mesh::Mesh *m,
                      int domain,
                      util::Vector<int> &faces,
                      util::Vector<int> &verts,
                      bool state)
  {
    if (!m) {
      return;
    }
    switch (domain) {
    case 0:
      for (int v : verts) {
        selectOne(m, 0, v, state);
      }
      break;
    case 2:
      for (int f : faces) {
        selectOne(m, 2, f, state);
      }
      break;
    case 1: {
      util::Set<int> vset;
      for (int v : verts) {
        vset.add(v);
      }
      for (int v : verts) {
        for (int e : m->e_of_v(v)) {
          int other = m->e.vs[e][0] == v ? m->e.vs[e][1] : m->e.vs[e][0];
          if (vset.contains(other)) {
            selectOne(m, 1, e, state);
          }
        }
      }
      break;
    }
    }
  }


  void MeshLog::selectScreenCircle(mesh::Mesh *m,
                          spatial::SpatialTree *tree,
                          const math::float3 &co,
                          const math::float3 &ray,
                          float r1,
                          float r2,
                          int domain,
                          int state)
  {
    if (!m || !tree) {
      return;
    }
    if (m->topo_frozen) {
      m->thawTopo();
    }
    util::Vector<int> faces, verts;
    tree->castScreenCircle(co, ray, r1, r2, faces, verts);
    selectFromSets(m, domain, faces, verts, state != 0);
  }


  void MeshLog::selectScreenRect(mesh::Mesh *m,
                        spatial::SpatialTree *tree,
                        const math::float3 &near0,
                        const math::float3 &near1,
                        const math::float3 &near2,
                        const math::float3 &near3,
                        const math::float3 &far0,
                        const math::float3 &far1,
                        const math::float3 &far2,
                        const math::float3 &far3,
                        int domain,
                        int state)
  {
    if (!m || !tree) {
      return;
    }
    if (m->topo_frozen) {
      m->thawTopo();
    }
    util::Vector<int> faces, verts;
    tree->castScreenRect(
        near0, near1, near2, near3, far0, far1, far2, far3, faces, verts);
    selectFromSets(m, domain, faces, verts, state != 0);
  }


  void MeshLog::setActiveElem(int domain, int idx)
  {
    switch (domain) {
    case 0:
      active_vert_ = idx;
      break;
    case 1:
      active_edge_ = idx;
      break;
    case 2:
      active_face_ = idx;
      break;
    }
  }


  int MeshLog::activeVert() const
  {
    return active_vert_;
  }


  int MeshLog::activeEdge() const
  {
    return active_edge_;
  }


  int MeshLog::activeFace() const
  {
    return active_face_;
  }


  MeshLog::LogEntry &MeshLog::curEntry()
  {
    return entries[curStep_];
  }


  const MeshLog::LogEntry &MeshLog::curEntry() const
  {
    return entries[curStep_];
  }

} // namespace sculptcore::meshlog
