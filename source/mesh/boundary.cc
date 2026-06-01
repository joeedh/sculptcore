#include "boundary.h"

#include "attribute.h"
#include "attribute_bool.h"
#include "mesh.h"
#include "mesh_iter.h"

namespace sculptcore::mesh::boundary {

namespace {

BoolAttrView *ensureBoolEdge(MeshBase *m, const char *name, bool temp)
{
  AttrRef &ref = m->e.attrs.ensure(AttrType::BOOL, name);
  if (temp) ref.flag = AttrFlag::TEMP;
  return static_cast<BoolAttrView *>(ref.data);
}
BoolAttrView *ensureBoolVert(MeshBase *m, const char *name, bool temp)
{
  AttrRef &ref = m->v.attrs.ensure(AttrType::BOOL, name);
  if (temp) ref.flag = AttrFlag::TEMP;
  return static_cast<BoolAttrView *>(ref.data);
}
BoolAttrView *findBoolEdge(MeshBase *m, const char *name)
{
  if (!m->e.attrs.has(AttrType::BOOL, name)) return nullptr;
  AttrRef ref = m->e.attrs.find_attribute(AttrType::BOOL, name);
  return static_cast<BoolAttrView *>(ref.data);
}
AttrData<int> *findIntFace(MeshBase *m, const char *name)
{
  if (!m->f.attrs.has(AttrType::INT, name)) return nullptr;
  AttrRef ref = m->f.attrs.find_attribute(AttrType::INT, name);
  return static_cast<AttrData<int> *>(ref.data);
}
AttrData<int> *ensureIntVert(MeshBase *m, const char *name, bool temp)
{
  AttrRef &ref = m->v.attrs.ensure(AttrType::INT, name, /*materialize=*/true);
  if (temp) ref.flag = AttrFlag::TEMP;
  return static_cast<AttrData<int> *>(ref.data);
}

// True if the faces adjacent to edge `e` carry differing poly-group ids.
// Walks the radial cycle explicitly: CornerOfEdgeIter's operator++ doesn't
// terminate at the cycle start, so a range-for over it would spin forever.
bool computePolygroupBoundary(MeshBase *m, int e, AttrData<int> *faceGroup)
{
  if (!faceGroup) return false;
  int c0 = m->e.c[e];
  if (c0 == ELEM_NONE) return false; // wire edge — no adjacent faces
  bool haveFirst = false;
  int g0 = 0;
  int c = c0;
  do {
    int face = m->l.f[m->c.l[c]];
    int g = (*faceGroup)[face];
    if (!haveFirst) {
      g0 = g;
      haveFirst = true;
    } else if (g != g0) {
      return true;
    }
    c = m->c.radial_next[c];
  } while (c != c0 && c != ELEM_NONE);
  return false;
}

} // namespace

void markEdgeDirty(MeshBase *m, int e)
{
  ensureBoolEdge(m, EDGE_DIRTY, true)->set(e, true);
  m->boundaryDirty = true;
}

void markVertDirty(MeshBase *m, int v)
{
  ensureBoolVert(m, VERT_DIRTY, true)->set(v, true);
  m->boundaryDirty = true;
}

void setEdgeFlag(MeshBase *m, const char *flagName, int e, bool state)
{
  ensureBoolEdge(m, flagName, /*temp=*/false)->set(e, state);
  markEdgeDirty(m, e);
  markVertDirty(m, m->e.vs[e][0]);
  markVertDirty(m, m->e.vs[e][1]);
}

bool edgeFlag(MeshBase *m, const char *flagName, int e)
{
  BoolAttrView *v = findBoolEdge(m, flagName);
  return v ? v->get(e) : false;
}

BoolAttrView *findBoolEdgeView(MeshBase *m, const char *flagName)
{
  return findBoolEdge(m, flagName);
}

void markFaceDirty(MeshBase *m, int f)
{
  // Walk the face's boundary/hole lists and their corner cycles, marking each
  // corner's edge + vertex dirty (mirrors BasicFaceIter::computeCentroid).
  int l = m->f.l[f];
  while (l != ELEM_NONE) {
    int start = m->l.c[l];
    int c = start;
    if (c != ELEM_NONE) {
      do {
        markEdgeDirty(m, m->c.e[c]);
        markVertDirty(m, m->c.v[c]);
        c = m->c.next[c];
      } while (c != start && c != ELEM_NONE);
    }
    l = m->l.next[l];
  }
}

void markAllDirty(MeshBase *m)
{
  BoolAttrView *eDirty = ensureBoolEdge(m, EDGE_DIRTY, true);
  for (int e = 0; e < m->e.count; e++) eDirty->set(e, true);
  BoolAttrView *vDirty = ensureBoolVert(m, VERT_DIRTY, true);
  for (int v = 0; v < m->v.count; v++) vDirty->set(v, true);
  m->boundaryDirty = true;
}

void recomputeDirty(MeshBase *m)
{
  BoolAttrView *eDirty = ensureBoolEdge(m, EDGE_DIRTY, true);
  BoolAttrView *vDirty = ensureBoolVert(m, VERT_DIRTY, true);

  BoolAttrView *eProj = findBoolEdge(m, EDGE_PROJECTED);
  BoolAttrView *eSharp = findBoolEdge(m, EDGE_SHARP);
  BoolAttrView *eSeam = findBoolEdge(m, EDGE_SEAM);
  BoolAttrView *eUv = findBoolEdge(m, EDGE_UVCHART);
  BoolAttrView *ePoly = ensureBoolEdge(m, EDGE_POLYGROUP, true);
  AttrData<int> *faceGroup = findIntFace(m, FACE_GROUP);
  AttrData<int> *vClass = ensureIntVert(m, VERT_CLASS, true);

  // Pass 1: recompute derived flags for dirty edges, and dirty their endpoints
  // so the affected vertex classifications refresh too.
  for (int e = 0; e < m->e.count; e++) {
    if (!eDirty->get(e)) continue;
    ePoly->set(e, computePolygroupBoundary(m, e, faceGroup));
    // (UV-chart derived flag is a follow-up: compare per-corner UVs across e.)
    eDirty->set(e, false);
    vDirty->set(m->e.vs[e][0], true);
    vDirty->set(m->e.vs[e][1], true);
  }

  // Pass 2: recompute the classification bitmask of every dirty vertex from its
  // incident edges' boundary flags.
  for (int v = 0; v < m->v.count; v++) {
    if (!vDirty->get(v)) continue;
    int cls = BC_NONE;
    for (int e : EdgeOfVertIter(m, v, m->v.e[v])) {
      if (eProj && eProj->get(e)) cls |= BC_PROJECTED;
      if (eSharp && eSharp->get(e)) cls |= BC_SHARP;
      if (eSeam && eSeam->get(e)) cls |= BC_SEAM;
      if (ePoly->get(e)) cls |= BC_POLYGROUP;
      if (eUv && eUv->get(e)) cls |= BC_UVCHART;
    }
    (*vClass)[v] = cls;
    vDirty->set(v, false);
  }

  m->boundaryDirty = false; // classification is now current
}

int vertClass(MeshBase *m, int v)
{
  if (!m->v.attrs.has(AttrType::INT, VERT_CLASS)) return 0;
  AttrRef ref = m->v.attrs.find_attribute(AttrType::INT, VERT_CLASS);
  return (*static_cast<AttrData<int> *>(ref.data))[v];
}

} // namespace sculptcore::mesh::boundary
