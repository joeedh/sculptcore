#include "boundary.h"

#include "attribute.h"
#include "attribute_bool.h"
#include "attribute_enums.h"
#include "mesh.h"
#include "mesh_iter.h"

#include "litestl/math/vector.h"

namespace sculptcore::mesh::boundary {

namespace {

using litestl::math::float2;

// The mesh's active UV corner layer (first FLOAT2 corner attr tagged AttrUse::UV),
// or null if the mesh carries no UVs.
AttrData<float2> *findUvCorner(MeshBase *m)
{
  for (AttrRef &attr : m->c.attrs.attrs) {
    if (attr.type == AttrType::FLOAT2 && attr.data &&
        (static_cast<int>(attr.use) & static_cast<int>(AttrUse::UV)) != 0) {
      return static_cast<AttrData<float2> *>(attr.data);
    }
  }
  return nullptr;
}

// Corner of face `f` sitting at vertex `v` (ELEM_NONE if none).
int cornerOfFaceVert(MeshBase *m, int f, int v)
{
  int l = m->f.l[f];
  if (l == ELEM_NONE) return ELEM_NONE;
  int c0 = m->l.c[l], c = c0;
  do {
    if (m->c.v[c] == v) return c;
    c = m->c.next[c];
  } while (c != c0 && c != ELEM_NONE);
  return ELEM_NONE;
}

// True if the UVs are discontinuous across interior edge `e` (its two faces
// assign different UVs to a shared endpoint) — i.e. e is a UV-chart boundary.
bool computeUvChartBoundary(MeshBase *m, int e, AttrData<float2> *uv)
{
  if (!uv) return false;
  int c0 = m->e.c[e];
  if (c0 == ELEM_NONE) return false; // wire edge
  // Collect the (up to) two incident faces.
  int fA = ELEM_NONE, fB = ELEM_NONE;
  int c = c0;
  do {
    int f = m->l.f[m->c.l[c]];
    if (f != fA && f != fB) {
      if (fA == ELEM_NONE) fA = f;
      else if (fB == ELEM_NONE) fB = f;
    }
    c = m->c.radial_next[c];
  } while (c != c0 && c != ELEM_NONE);
  if (fA == ELEM_NONE || fB == ELEM_NONE) {
    return false; // mesh-boundary edge (1 face): handled topologically elsewhere
  }
  const float eps2 = 1e-10f;
  int verts[2] = {m->e.vs[e][0], m->e.vs[e][1]};
  for (int vi = 0; vi < 2; vi++) {
    int v = verts[vi];
    int ca = cornerOfFaceVert(m, fA, v);
    int cb = cornerOfFaceVert(m, fB, v);
    if (ca == ELEM_NONE || cb == ELEM_NONE) continue;
    if (((*uv)[ca] - (*uv)[cb]).lengthSqr() > eps2) {
      return true;
    }
  }
  return false;
}

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
  m->e.boundaryDirty.ensure(m->e.attrs);
  m->e.boundaryDirty.set(e, true);
  m->boundaryDirty = true;
}

void markVertDirty(MeshBase *m, int v)
{
  m->v.boundaryDirty.ensure(m->v.attrs);
  m->v.boundaryDirty.set(v, true);
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
  // Iterate the full index range and skip freed slots (count is the live count,
  // not a valid index bound on a non-compact mesh — see recomputeDirty).
  BoolAttrView *eDirty = ensureBoolEdge(m, EDGE_DIRTY, true);
  const int ecap = int(m->e.capacity());
  for (int e = 0; e < ecap; e++)
    if (!m->e.freemap[e]) eDirty->set(e, true);
  BoolAttrView *vDirty = ensureBoolVert(m, VERT_DIRTY, true);
  const int vcap = int(m->v.capacity());
  for (int v = 0; v < vcap; v++)
    if (!m->v.freemap[v]) vDirty->set(v, true);
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
  AttrData<float2> *uvCorner = findUvCorner(m);
  // The UV-chart layer is only needed (and only materialized) when the mesh has
  // UVs to derive it from.
  BoolAttrView *eUvDerive = uvCorner ? ensureBoolEdge(m, EDGE_UVCHART, true) : nullptr;

  // Pass 1: recompute derived flags for dirty edges, and dirty their endpoints
  // so the affected vertex classifications refresh too. Iterate the full index
  // range and skip freed slots — after a topology edit (collapse frees, split
  // grows capacity) the mesh is non-compact, so live elements sit at indices
  // >= count and `count` is NOT a valid loop bound.
  const int ecap = int(m->e.capacity());
  for (int e = 0; e < ecap; e++) {
    if (m->e.freemap[e] || !eDirty->get(e)) continue;
    ePoly->set(e, computePolygroupBoundary(m, e, faceGroup));
    if (eUvDerive) {
      eUvDerive->set(e, computeUvChartBoundary(m, e, uvCorner));
    }
    eDirty->set(e, false);
    vDirty->set(m->e.vs[e][0], true);
    vDirty->set(m->e.vs[e][1], true);
  }

  // Pass 2: recompute the classification bitmask of every dirty vertex from its
  // incident edges' boundary flags.
  const int vcap = int(m->v.capacity());
  for (int v = 0; v < vcap; v++) {
    if (m->v.freemap[v] || !vDirty->get(v)) continue;
    int cls = BC_NONE;
    for (int e : EdgeOfVertIter(m, v, m->v.e[v])) {
      if (eProj && eProj->get(e)) cls |= BC_PROJECTED;
      if (eSharp && eSharp->get(e)) cls |= BC_SHARP;
      if (eSeam && eSeam->get(e)) cls |= BC_SEAM;
      if (ePoly->get(e)) cls |= BC_POLYGROUP;
      BoolAttrView *uvView = eUvDerive ? eUvDerive : eUv;
      if (uvView && uvView->get(e)) cls |= BC_UVCHART;
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

void graphStats(MeshBase *m, litestl::util::Vector<int> &out)
{
  out.clear();
  BoolAttrView *views[] = {
      findBoolEdge(m, EDGE_PROJECTED),
      findBoolEdge(m, EDGE_SHARP),
      findBoolEdge(m, EDGE_SEAM),
      findBoolEdge(m, EDGE_POLYGROUP),
      findBoolEdge(m, EDGE_UVCHART),
  };

  const int vcap = int(m->v.capacity());
  litestl::util::Vector<int> valence, parent;
  valence.resize(vcap);
  parent.resize(vcap);
  for (int v = 0; v < vcap; v++) {
    valence[v] = 0;
    parent[v] = v;
  }
  // Union-find with path halving; only flagged-edge endpoints ever get unioned.
  auto find = [&parent](int x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };

  int flagged = 0;
  const int ecap = int(m->e.capacity());
  for (int e = 0; e < ecap; e++) {
    if (m->e.freemap[e]) continue;
    bool on = false;
    for (BoolAttrView *view : views) {
      if (view && view->get(e)) {
        on = true;
        break;
      }
    }
    if (!on) continue;
    flagged++;
    int v0 = m->e.vs[e][0], v1 = m->e.vs[e][1];
    valence[v0]++;
    valence[v1]++;
    int r0 = find(v0), r1 = find(v1);
    if (r0 != r1) {
      parent[r0] = r1;
    }
  }

  int graphVerts = 0, non2 = 0, components = 0;
  for (int v = 0; v < vcap; v++) {
    if (valence[v] == 0) continue;
    graphVerts++;
    if (valence[v] != 2) non2++;
    if (find(v) == v) components++;
  }

  out.append(flagged);
  out.append(graphVerts);
  out.append(non2);
  out.append(components);
}

} // namespace sculptcore::mesh::boundary
