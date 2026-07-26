#include "roughness.h"

#include "mesh/attribute.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"

#include <algorithm>
#include <cmath>

namespace sculptcore::debug_app {

using litestl::math::float3;
using litestl::util::Vector;

namespace {

/** Point-set accessor: live positions, or the brush displacement base. Reads
 * whichever base representation the engine carries — `co - disp` on the disp
 * path, `.brush.orig.co` on the legacy path — so one metric scores both sides
 * of the A/B. Unstamped verts fall back to live, which is what both paths mean
 * by "not displaced this stroke". */
struct PointSet {
  mesh::Mesh *m = nullptr;
  mesh::AttrData<float3> *dispVec = nullptr;
  mesh::AttrData<int> *dispGen = nullptr;
  mesh::AttrData<float3> *origCo = nullptr;
  mesh::AttrData<int> *origGen = nullptr;
  uint32_t gen = 0;
  bool useBase = false;

  float3 operator()(int v)
  {
    if (useBase) {
      if (dispVec && dispGen && dispGen->safe_get(v) == int(gen)) {
        return m->v.co[v] - dispVec->safe_get(v);
      }
      if (origCo && origGen && origGen->safe_get(v) == int(gen)) {
        return origCo->safe_get(v);
      }
    }
    return m->v.co[v];
  }
};

// Faces on an edge's radial cycle: 1 = boundary, 2 = interior manifold.
int edgeFaceCount(mesh::Mesh *m, int e)
{
  int c0 = m->e.c[e];
  if (c0 == ELEM_NONE) {
    return 0;
  }
  int n = 0, cc = c0;
  do {
    n++;
    cc = m->c.radial_next[cc];
  } while (cc != c0 && n < 64);
  return n;
}

// Unnormalized Newell normal of face `f` under `P` (length ~ 2*area).
float3 faceNormal(mesh::Mesh *m, int f, PointSet &P)
{
  float3 n(0.0f, 0.0f, 0.0f);
  int li = m->f.l[f];
  if (li == ELEM_NONE) {
    return n;
  }
  int c0 = m->l.c[li], c = c0, guard = 0;
  do {
    int cn = m->c.next[c];
    float3 a = P(m->c.v[c]), b = P(m->c.v[cn]);
    n[0] += (a[1] - b[1]) * (a[2] + b[2]);
    n[1] += (a[2] - b[2]) * (a[0] + b[0]);
    n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    c = cn;
  } while (c != c0 && ++guard < 64);
  return n;
}

/** One-ring of `v`: the disk's other endpoints plus the incident faces. Order
 * is irrelevant to every quantity below (centroid, mean edge length,
 * area-weighted normal), so the disk cycle is walked directly rather than
 * reconstructing fan order. Returns false when any incident edge is not
 * interior-manifold, i.e. `v` sits on a boundary the umbrella can't span. */
bool gatherRing(mesh::Mesh *m, int v, Vector<int> &ring, Vector<int> &faces)
{
  ring.clear();
  faces.clear();
  for (int e : mesh::EdgeOfVertIter(m, v, m->v.e[v])) {
    if (edgeFaceCount(m, e) != 2) {
      return false;
    }
    ring.append(m->e.vs[e][0] == v ? m->e.vs[e][1] : m->e.vs[e][0]);
    // Each incident face owns exactly one corner whose vert is v, and that
    // corner sits in exactly one of v's disk edges' radial cycles — so this
    // filter enumerates the incident faces without a dedupe pass.
    int c0 = m->e.c[e], cc = c0, guard = 0;
    do {
      if (m->c.v[cc] == v) {
        faces.append(m->l.f[m->c.l[cc]]);
      }
      cc = m->c.radial_next[cc];
    } while (cc != c0 && ++guard < 64);
  }
  return ring.size() >= 3 && faces.size() >= 3;
}

// Area-weighted vertex normal + barycentric vertex area from incident faces.
void vertNormalArea(
    mesh::Mesh *m, const Vector<int> &faces, PointSet &P, float3 &nrm, float &area)
{
  float3 acc(0.0f, 0.0f, 0.0f);
  float a2 = 0.0f;
  for (int f : faces) {
    float3 fn = faceNormal(m, f, P);
    acc += fn;
    a2 += fn.length();
  }
  float len = acc.length();
  nrm = len > 0.0f ? acc / len : float3(0.0f, 0.0f, 0.0f);
  // |Newell| is 2*area; the vertex gets a 1/3 barycentric share of each face.
  area = a2 / 6.0f;
}

} // namespace

void collectRegion(mesh::Mesh *m,
                   const Vector<float3> &centers,
                   float radius,
                   Vector<int> &out)
{
  out.clear();
  float r2 = radius * radius;
  for (int v : m->v) {
    float3 co = m->v.co[v];
    for (const float3 &c : centers) {
      if ((co - c).dot(co - c) <= r2) {
        out.append(v);
        break;
      }
    }
  }
}

RoughnessResult computeRoughness(mesh::Mesh *m,
                                 const Vector<int> &region,
                                 RoughnessPoints pts,
                                 uint32_t strokeGen,
                                 float3 up,
                                 float rest)
{
  RoughnessResult res;
  if (!m || region.size() == 0) {
    return res;
  }
  // The disk/radial walks below need live links; a frozen mesh has dropped them.
  if (m->topo_frozen) {
    m->thawTopo();
  }

  PointSet P;
  P.m = m;
  P.gen = strokeGen;
  P.useBase = pts == RoughnessPoints::Base;
  if (m->v.attrs.has(mesh::AttrType::FLOAT3, ".brush.disp.vec") &&
      m->v.attrs.has(mesh::AttrType::INT, ".brush.disp.gen"))
  {
    P.dispVec = m->v.attrs.find_attribute(mesh::AttrType::FLOAT3, ".brush.disp.vec")
                    .get_data<float3>();
    P.dispGen =
        m->v.attrs.find_attribute(mesh::AttrType::INT, ".brush.disp.gen").get_data<int>();
  }
  if (m->v.attrs.has(mesh::AttrType::FLOAT3, ".brush.orig.co") &&
      m->v.attrs.has(mesh::AttrType::INT, ".brush.orig.gen"))
  {
    P.origCo = m->v.attrs.find_attribute(mesh::AttrType::FLOAT3, ".brush.orig.co")
                   .get_data<float3>();
    P.origGen =
        m->v.attrs.find_attribute(mesh::AttrType::INT, ".brush.orig.gen").get_data<int>();
  }
  // The fidelity guard is always measured on the live surface (plan §9.1 —
  // measuring it through `co - disp` would be circular).
  PointSet L;
  L.m = m;

  Vector<uint8_t> inRegion, isInterior;
  inRegion.resize(m->v.capacity());
  isInterior.resize(m->v.capacity());
  for (int v : region) {
    if (v >= 0 && v < int(m->v.capacity()) && !m->v.freemap[v]) {
      inRegion[v] = 1;
    }
  }

  Vector<float> rs;
  Vector<int> ring, faces;
  for (int v : region) {
    if (!inRegion[v] || !gatherRing(m, v, ring, faces)) {
      continue;
    }
    bool ringInside = true;
    for (int nb : ring) {
      ringInside &= inRegion[nb] != 0;
    }
    if (!ringInside) {
      continue;
    }
    isInterior[v] = 1;

    float3 p = P(v), centroid(0.0f, 0.0f, 0.0f);
    float h = 0.0f;
    for (int nb : ring) {
      float3 q = P(nb);
      centroid += q;
      h += (q - p).length();
    }
    centroid /= float(ring.size());
    h /= float(ring.size());
    float3 nrm;
    float area = 0.0f;
    vertNormalArea(m, faces, P, nrm, area);
    if (h > 0.0f) {
      rs.append((p - centroid).dot(nrm) / h);
    }

    float3 lnrm;
    float larea = 0.0f;
    vertNormalArea(m, faces, L, lnrm, larea);
    float height = m->v.co[v].dot(up) - rest;
    res.maxDisp = std::fmax(res.maxDisp, std::fabs(height));
    res.volume += height * larea;
  }
  res.verts = int(rs.size());

  if (rs.size() > 0) {
    double sum2 = 0.0;
    Vector<float> mags;
    for (float r : rs) {
      sum2 += double(r) * double(r);
      mags.append(std::fabs(r));
    }
    res.rms = float(std::sqrt(sum2 / double(rs.size())));
    std::sort(mags.begin(), mags.end());
    res.maxr = mags[mags.size() - 1];
    int pi = int(0.95f * float(mags.size() - 1) + 0.5f);
    res.p95 = mags[pi];
  }

  // Mean |dihedral| over edges whose endpoints are both interior region verts:
  // a face-based check that catches the spike/sliver modes the vertex umbrella
  // averages away. Same point set as the roughness above.
  Vector<uint8_t> seenEdge;
  seenEdge.resize(m->e.capacity());
  double dsum = 0.0;
  int dn = 0;
  for (int v : region) {
    if (!isInterior[v]) {
      continue;
    }
    for (int e : mesh::EdgeOfVertIter(m, v, m->v.e[v])) {
      int other = m->e.vs[e][0] == v ? m->e.vs[e][1] : m->e.vs[e][0];
      if (seenEdge[e] || !isInterior[other]) {
        continue;
      }
      seenEdge[e] = 1;
      int c0 = m->e.c[e];
      int c1 = m->c.radial_next[c0];
      int f0 = m->l.f[m->c.l[c0]], f1 = m->l.f[m->c.l[c1]];
      if (f0 == f1) {
        continue;
      }
      float3 n0 = faceNormal(m, f0, P), n1 = faceNormal(m, f1, P);
      if (n0.normalize() == 0.0f || n1.normalize() == 0.0f) {
        continue;
      }
      dsum += std::acos(double(std::clamp(n0.dot(n1), -1.0f, 1.0f)));
      dn++;
    }
  }
  res.edges = dn;
  res.dihedral = dn > 0 ? float(dsum / double(dn)) : 0.0f;
  return res;
}

} // namespace sculptcore::debug_app
