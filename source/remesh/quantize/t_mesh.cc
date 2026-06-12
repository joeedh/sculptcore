#include "remesh/quantize/t_mesh.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"

#include "litestl/math/vector.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cmath>

namespace sculptcore::remesh {

using litestl::math::float2;
using litestl::util::Vector;
using sculptcore::mesh::AttrFlag;
using sculptcore::mesh::BuiltinAttr;
using sculptcore::mesh::Mesh;

namespace {

inline bool interiorEdge(Mesh &m, int e, int &fa, int &fb)
{
  int c1 = m.e.c[e];
  if (c1 == ELEM_NONE) {
    return false;
  }
  int c2 = m.c.radial_next[c1];
  if (c2 == c1 || m.c.radial_next[c2] != c1) {
    return false;
  }
  fa = m.l.f[m.c.l[c1]];
  fb = m.l.f[m.c.l[c2]];
  return fa != fb;
}

// CCW rotation of an integer-grid 2D vector by k*90 degrees.
inline float2 rot90(int k, float2 p)
{
  k = ((k % 4) + 4) % 4;
  switch (k) {
  case 0:
    return float2(p[0], p[1]);
  case 1:
    return float2(-p[1], p[0]);
  case 2:
    return float2(-p[0], -p[1]);
  default:
    return float2(p[1], -p[0]);
  }
}

} // namespace

QuantGraph buildQuantGraph(Mesh &m, const Vector<int> &cornerClass,
                           const Vector<int> &gauge, const Vector<int> &periodEC)
{
  QuantGraph g;
  const int ecap = int(m.e.capacity());
  g.sideOfEdge.resize(ecap);
  for (int i = 0; i < ecap; i++) {
    g.sideOfEdge[i] = -1;
  }

  BuiltinAttr<bool, ".remesh.e.is_cut", AttrFlag::TEMP> is_cut;
  is_cut.ensure(m.e.attrs);

  for (int e : m.e) {
    int fa, fb;
    if (!interiorEdge(m, e, fa, fb) || !is_cut[e]) {
      continue;
    }
    int c1 = m.e.c[e];            // corner in fa at endpoint 1 = v(c1)
    int c2 = m.c.radial_next[c1]; // corner in fb at endpoint 2
    int cb = m.c.next[c2];        // corner in fb at endpoint 1
    int side = g.num_sides();
    g.sideOfEdge[e] = side;
    g.edge.append(e);
    g.cla.append(cornerClass[c1]);            // fa at endpoint 1
    g.clb.append(cornerClass[cb]);            // fb at endpoint 1
    g.cla2.append(cornerClass[m.c.next[c1]]); // fa at endpoint 2
    g.clb2.append(cornerClass[c2]);           // fb at endpoint 2
    g.period.append(periodEC[e]);
    g.ga.append(gauge[fa]);
    g.gb.append(gauge[fb]);
    g.t_real.append(float2(0.0f, 0.0f));
    g.t_int.append(float2(0.0f, 0.0f));
  }
  return g;
}

double loopClosureResidual(Mesh &m, const QuantGraph &g, const Vector<int> &periodEC)
{
  // One corner per vertex, so we can start each one-ring walk somewhere valid.
  const int vcap = int(m.v.capacity());
  Vector<int> vcorner;
  vcorner.resize(vcap);
  for (int i = 0; i < vcap; i++) {
    vcorner[i] = -1;
  }
  for (int f : m.f) {
    int c0 = m.l.c[m.f.l[f]], cc = c0;
    do {
      vcorner[m.c.v[cc]] = cc;
      cc = m.c.next[cc];
    } while (cc != c0);
  }

  double maxres = 0.0;
  for (int v : m.v) {
    int cstart = vcorner[v];
    if (cstart < 0) {
      continue;
    }
    int Racc = 0;
    float2 Tacc(0.0f, 0.0f);
    int c = cstart;
    bool ok = true;
    int guard = 0;
    do {
      int e = m.c.e[c];
      int fa, fb;
      if (!interiorEdge(m, e, fa, fb)) {
        ok = false; // boundary vertex: no closed one-ring
        break;
      }
      int p = periodEC[e];
      int side = g.sideOfEdge[e];
      float2 t = (side >= 0) ? g.t_int[side] : float2(0.0f, 0.0f);
      int fc = m.l.f[m.c.l[c]];
      if (fc == fa) { // forward fa->fb: X_fb = R(p)X_fa + t
        Tacc = rot90(p, Tacc);
        Tacc = float2(Tacc[0] + t[0], Tacc[1] + t[1]);
        Racc += p;
      } else { // inverse fb->fa: X_fa = R(-p)(X_fb - t)
        Tacc = float2(Tacc[0] - t[0], Tacc[1] - t[1]);
        Tacc = rot90(-p, Tacc);
        Racc -= p;
      }
      c = m.c.next[m.c.radial_next[c]]; // next corner around v
      if (++guard > 4096) {
        ok = false;
        break;
      }
    } while (c != cstart);

    if (!ok || (((Racc % 4) + 4) % 4) != 0) {
      continue; // boundary, degenerate, or singular vertex
    }
    double res = std::sqrt(double(Tacc[0]) * double(Tacc[0]) +
                           double(Tacc[1]) * double(Tacc[1]));
    maxres = std::fmax(maxres, res);
  }
  return maxres;
}

} // namespace sculptcore::remesh
