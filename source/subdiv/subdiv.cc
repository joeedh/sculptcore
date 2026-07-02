#include "subdiv.h"

#include "mesh/attribute_bool.h"
#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"

#include "litestl/util/alloc.h"
#include "litestl/util/assert.h"

using namespace litestl;
using litestl::math::float3;
using litestl::util::Assert;
using litestl::util::Vector;

namespace sculptcore::subdiv {

void StencilTable::eval(const Vector<float3> &src, Vector<float3> &dst) const
{
  dst.resize(fineCount);
  for (int i = 0; i < fineCount; i++) {
    float3 p;
    for (int k = offsets[i]; k < offsets[i + 1]; k++) {
      p += src[indices[k]] * weights[k];
    }
    dst[i] = p;
  }
}

void gatherVertCo(mesh::Mesh &m, Vector<float3> &out)
{
  out.resize(m.v.capacity());
  for (int i = 0; i < int(m.v.capacity()); i++) {
    out[i] = m.v.freemap[i] ? float3() : m.v.co[i];
  }
}

namespace {

/* One stencil row under construction: entries kept ascending by coarse vert
 * id, weights accumulated in double so the emitted float weight is independent
 * of add() call order per id. */
struct RowBuilder {
  Vector<int> idx;
  Vector<double> w;

  void clear()
  {
    idx.clear();
    w.clear();
  }

  void add(int vert, double weight)
  {
    int n = int(idx.size());
    int at = n;
    for (int i = 0; i < n; i++) {
      if (idx[i] == vert) {
        w[i] += weight;
        return;
      }
      if (idx[i] > vert) {
        at = i;
        break;
      }
    }
    idx.append(0);
    w.append(0.0);
    for (int i = n; i > at; i--) {
      idx[i] = idx[i - 1];
      w[i] = w[i - 1];
    }
    idx[at] = vert;
    w[at] = weight;
  }

  void commit(StencilTable &st)
  {
    for (int i = 0; i < int(idx.size()); i++) {
      st.indices.append(idx[i]);
      st.weights.append(float(w[i]));
    }
    st.fineCount++;
    st.offsets.append(int(st.indices.size()));
  }
};

int countEdgeFaces(mesh::Mesh *m, int e)
{
  int c0 = m->e.c[e];
  if (c0 == ELEM_NONE) {
    return 0;
  }
  int n = 0, cc = c0;
  do {
    n++;
    cc = m->c.radial_next[cc];
  } while (cc != c0);
  return n;
}

int countFaceVerts(mesh::Mesh *m, int f)
{
  int c0 = m->l.c[m->f.l[f]], cc = c0, n = 0;
  do {
    n++;
    cc = m->c.next[cc];
  } while (cc != c0);
  return n;
}

void addFaceVerts(mesh::Mesh *m, int f, double weightEach, RowBuilder &row)
{
  int c0 = m->l.c[m->f.l[f]], cc = c0;
  do {
    row.add(m->c.v[cc], weightEach);
    cc = m->c.next[cc];
  } while (cc != c0);
}

/* Corner of face `f` whose vert is `v1` (ELEM_NONE if absent). */
int faceCornerOfVert(mesh::Mesh *m, int f, int v1)
{
  int c0 = m->l.c[m->f.l[f]], cc = c0;
  do {
    if (m->c.v[cc] == v1) {
      return cc;
    }
    cc = m->c.next[cc];
  } while (cc != c0);
  return ELEM_NONE;
}

void fillNone(Vector<int> &v, int n)
{
  v.resize(n);
  for (int i = 0; i < n; i++) {
    v[i] = ELEM_NONE;
  }
}

} // namespace

Refiner::~Refiner()
{
  clear();
}

void Refiner::clear()
{
  for (SubdivLevel &lvl : levels) {
    if (lvl.mesh) {
      alloc::Delete(lvl.mesh);
      lvl.mesh = nullptr;
    }
  }
  levels.clear();
  gridCount_ = 0;
}

/* One uniform CC step m0 -> lvl.mesh. `prevLvl` is null for the first step
 * (m0 == the cage), where grids are seeded one-per-cage-corner. */
static void refineStep(mesh::Mesh *m0,
                       const SubdivLevel *prevLvl,
                       SubdivLevel &lvl,
                       int gridCount)
{
  using namespace sculptcore::mesh;

  m0->thawTopo();

  BoolAttrView *sharp = boundary::findBoolEdgeView(m0, boundary::EDGE_SHARP);
  auto isCrease = [&](int e) {
    return countEdgeFaces(m0, e) != 2 || (sharp && (*sharp)[e]);
  };

  Mesh *m1 = alloc::New<Mesh>("subdiv level");
  lvl.mesh = m1;

  StencilTable &st = lvl.stencil;
  st.coarseCount = int(m0->v.capacity());
  st.offsets.append(0);

  fillNone(lvl.facePointOf, int(m0->f.capacity()));
  fillNone(lvl.edgePointOf, int(m0->e.capacity()));
  fillNone(lvl.vertPointOf, int(m0->v.capacity()));

  RowBuilder row;
  auto appendVert = [&](RowBuilder &r) {
    r.commit(st);
    int nv = m1->make_vertex(float3());
    Assert(nv == st.fineCount - 1, "subdiv level verts allocate densely");
    return nv;
  };

  /* Face points: centroid of the face's verts. */
  for (int fi : m0->f) {
    row.clear();
    addFaceVerts(m0, fi, 1.0 / countFaceVerts(m0, fi), row);
    lvl.facePointOf[fi] = appendVert(row);
  }

  /* Edge points: midpoint on creases/boundary, else the (v0+v1+fp1+fp2)/4 rule
   * expanded onto the coarse verts (face points are fine verts). */
  for (int ei : m0->e) {
    row.clear();
    int v0 = m0->e.vs[ei][0], v1 = m0->e.vs[ei][1];
    if (isCrease(ei)) {
      row.add(v0, 0.5);
      row.add(v1, 0.5);
    } else {
      row.add(v0, 0.25);
      row.add(v1, 0.25);
      int c0 = m0->e.c[ei], cc = c0;
      do {
        int f = m0->l.f[m0->c.l[cc]];
        addFaceVerts(m0, f, 0.25 / countFaceVerts(m0, f), row);
        cc = m0->c.radial_next[cc];
      } while (cc != c0);
    }
    lvl.edgePointOf[ei] = appendVert(row);
  }

  /* Vertex points: corner (>=3 creases) holds, 2 creases -> the 1/8·6/8·1/8
   * crease rule, else the smooth (Q + 2R + (n-3)S)/n rule expanded. */
  for (int vi : m0->v) {
    row.clear();

    int valence = 0, nCrease = 0;
    int creaseOpp[2] = {ELEM_NONE, ELEM_NONE};
    for (int e1 : m0->e_of_v(vi)) {
      valence++;
      if (isCrease(e1)) {
        int opp = m0->e.vs[e1][0] == vi ? m0->e.vs[e1][1] : m0->e.vs[e1][0];
        if (nCrease < 2) {
          creaseOpp[nCrease] = opp;
        }
        nCrease++;
      }
    }

    if (valence == 0 || nCrease >= 3) {
      row.add(vi, 1.0);
    } else if (nCrease == 2) {
      row.add(vi, 0.75);
      row.add(creaseOpp[0], 0.125);
      row.add(creaseOpp[1], 0.125);
    } else {
      double n = double(valence);
      row.add(vi, (n - 3.0) / n);
      Vector<int> vfaces;
      for (int e1 : m0->e_of_v(vi)) {
        int opp = m0->e.vs[e1][0] == vi ? m0->e.vs[e1][1] : m0->e.vs[e1][0];
        row.add(vi, 1.0 / (n * n));
        row.add(opp, 1.0 / (n * n));
        int c0 = m0->e.c[e1];
        if (c0 == ELEM_NONE) {
          continue;
        }
        int cc = c0;
        do {
          if (m0->c.v[cc] == vi) {
            int f = m0->l.f[m0->c.l[cc]];
            if (!vfaces.contains(f)) {
              vfaces.append(f);
            }
          }
          cc = m0->c.radial_next[cc];
        } while (cc != c0);
      }
      for (int f : vfaces) {
        addFaceVerts(m0, f, 1.0 / (n * n * double(countFaceVerts(m0, f))), row);
      }
    }
    lvl.vertPointOf[vi] = appendVert(row);
  }

  /* Child faces: one quad per coarse corner. */
  Vector<int> childFaceOfCorner;
  fillNone(childFaceOfCorner, int(m0->c.capacity()));
  for (int fi : m0->f) {
    int c0 = m0->l.c[m0->f.l[fi]], cc = c0;
    do {
      int cp = m0->c.prev[cc];
      int quad[4] = {lvl.vertPointOf[m0->c.v[cc]],
                     lvl.edgePointOf[m0->c.e[cc]],
                     lvl.facePointOf[fi],
                     lvl.edgePointOf[m0->c.e[cp]]};
      childFaceOfCorner[cc] = m1->make_face(std::span<int>(quad, 4));
      cc = m0->c.next[cc];
    } while (cc != c0);
  }

  /* Propagate EDGE_SHARP onto both child edges of each sharp coarse edge. */
  if (sharp) {
    for (int ei : m0->e) {
      if (!(*sharp)[ei]) {
        continue;
      }
      int ep = lvl.edgePointOf[ei];
      for (int side = 0; side < 2; side++) {
        int ce = m1->find_edge(lvl.vertPointOf[m0->e.vs[ei][side]], ep);
        if (ce != ELEM_NONE) {
          boundary::setEdgeFlag(m1, boundary::EDGE_SHARP, ce, true);
        }
      }
    }
  }

  /* Positions: the level's geometry IS the stencil evaluation (the bit-exact
   * contract with evalFromCage). */
  Vector<float3> srcCo, dstCo;
  gatherVertCo(*m0, srcCo);
  st.eval(srcCo, dstCo);
  for (int i = 0; i < st.fineCount; i++) {
    m1->v.co[i] = dstCo[i];
  }
  lvl.vertCount = st.fineCount;
  m1->recalc_normals();

  /* Grids. First step: seed one 1-cell grid per cage corner. Later steps:
   * split each cell into 4, mapping through the point-of tables. */
  if (!prevLvl) {
    lvl.gridSide = 1;
    lvl.gridVerts.resize(gridCount * 4);
    lvl.gridFaces.resize(gridCount);
    int g = 0;
    for (int fi : m0->f) {
      int c0 = m0->l.c[m0->f.l[fi]], cc = c0;
      do {
        int cp = m0->c.prev[cc];
        int *gv = &lvl.gridVerts[g * 4];
        gv[0] = lvl.vertPointOf[m0->c.v[cc]]; /* (0,0) */
        gv[1] = lvl.edgePointOf[m0->c.e[cc]]; /* (1,0) */
        gv[2] = lvl.edgePointOf[m0->c.e[cp]]; /* (0,1) */
        gv[3] = lvl.facePointOf[fi];          /* (1,1) */
        lvl.gridFaces[g] = childFaceOfCorner[cc];
        g++;
        cc = m0->c.next[cc];
      } while (cc != c0);
    }
    Assert(g == gridCount, "grid count == total cage corners");
  } else {
    int S0 = prevLvl->gridSide, S1 = S0 * 2;
    int W0 = S0 + 1, W1 = S1 + 1;
    lvl.gridSide = S1;
    lvl.gridVerts.resize(gridCount * W1 * W1);
    lvl.gridFaces.resize(gridCount * S1 * S1);

    for (int g = 0; g < gridCount; g++) {
      const int *og = &prevLvl->gridVerts[g * W0 * W0];
      int *ng = &lvl.gridVerts[g * W1 * W1];

      for (int j = 0; j <= S0; j++) {
        for (int i = 0; i <= S0; i++) {
          ng[(2 * j) * W1 + 2 * i] = lvl.vertPointOf[og[j * W0 + i]];
        }
      }
      for (int j = 0; j <= S0; j++) {
        for (int i = 0; i < S0; i++) {
          int e1 = m0->find_edge(og[j * W0 + i], og[j * W0 + i + 1]);
          ng[(2 * j) * W1 + 2 * i + 1] = lvl.edgePointOf[e1];
        }
      }
      for (int j = 0; j < S0; j++) {
        for (int i = 0; i <= S0; i++) {
          int e1 = m0->find_edge(og[j * W0 + i], og[(j + 1) * W0 + i]);
          ng[(2 * j + 1) * W1 + 2 * i] = lvl.edgePointOf[e1];
        }
      }
      for (int j = 0; j < S0; j++) {
        for (int i = 0; i < S0; i++) {
          int F = prevLvl->gridFaces[g * S0 * S0 + j * S0 + i];
          ng[(2 * j + 1) * W1 + 2 * i + 1] = lvl.facePointOf[F];
          for (int dj = 0; dj < 2; dj++) {
            for (int di = 0; di < 2; di++) {
              int corner = faceCornerOfVert(m0, F, og[(j + dj) * W0 + i + di]);
              Assert(corner != ELEM_NONE, "grid cell corner vert on its face");
              lvl.gridFaces[g * S1 * S1 + (2 * j + dj) * S1 + 2 * i + di] =
                  childFaceOfCorner[corner];
            }
          }
        }
      }
    }
  }
}

void Refiner::refine(mesh::Mesh &cage, int levelCount)
{
  clear();

  cage.thawTopo();
  gridCount_ = 0;
  for (int fi : cage.f) {
    gridCount_ += countFaceVerts(&cage, fi);
  }

  mesh::Mesh *prev = &cage;
  for (int i = 0; i < levelCount; i++) {
    SubdivLevel lvl;
    refineStep(prev, i == 0 ? nullptr : &levels[i - 1], lvl, gridCount_);
    levels.append(std::move(lvl));
    prev = levels.last().mesh;
  }
}

void Refiner::evalFromCage(const Vector<float3> &cageCo,
                           int level,
                           Vector<float3> &out) const
{
  Assert(level >= 1 && level <= int(levels.size()), "level in refined range");

  Vector<float3> tmp[2];
  const Vector<float3> *src = &cageCo;
  for (int i = 0; i < level; i++) {
    Vector<float3> &dst = (i == level - 1) ? out : tmp[i & 1];
    levels[i].stencil.eval(*src, dst);
    src = &dst;
  }
}

} // namespace sculptcore::subdiv
