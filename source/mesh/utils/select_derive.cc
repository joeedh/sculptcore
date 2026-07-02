#include "select_derive.h"

#include "../mesh.h"

namespace sculptcore::mesh {

using litestl::util::Set;
using litestl::util::Vector;

litestl::util::Set<int> deriveFaceSelection(Mesh &m, DeriveRule rule)
{
  auto *vsel = m.v.select.get_data();
  auto *esel = m.e.select.get_data();

  Set<int> out;
  for (int f : m.f) {
    bool allVerts = true, allEdges = true, any = false, haveCorner = false;
    for (int l = m.f.l[f]; l != ELEM_NONE; l = m.l.next[l]) {
      int c0 = m.l.c[l], c = c0;
      do {
        haveCorner = true;
        bool vs = vsel->get(m.c.v[c]);
        bool es = esel->get(m.c.e[c]);
        allVerts &= vs;
        allEdges &= es;
        any |= vs || es;
        c = m.c.next[c];
      } while (c != c0);
    }
    if (!haveCorner) {
      continue;
    }
    bool take = rule == DeriveRule::All ? (allVerts || allEdges) : any;
    if (take) {
      out.add(f);
    }
  }
  return out;
}

litestl::util::Set<int> deriveEdgeSelection(Mesh &m, DeriveRule rule)
{
  auto *vsel = m.v.select.get_data();
  auto *fsel = m.f.select.get_data();

  Set<int> out;
  for (int e : m.e) {
    bool v0 = vsel->get(m.e.vs[e][0]);
    bool v1 = vsel->get(m.e.vs[e][1]);
    if (rule == DeriveRule::All) {
      if (v0 && v1) {
        out.add(e);
      }
      continue;
    }
    bool take = v0 || v1;
    if (!take) {
      int c0 = m.e.c[e];
      if (c0 != ELEM_NONE) {
        int c = c0;
        do {
          if (fsel->get(m.l.f[m.c.l[c]])) {
            take = true;
            break;
          }
          c = m.c.radial_next[c];
        } while (c != c0);
      }
    }
    if (take) {
      out.add(e);
    }
  }
  return out;
}

litestl::util::Set<int> deriveVertSelection(Mesh &m)
{
  auto *vsel = m.v.select.get_data();
  auto *esel = m.e.select.get_data();
  auto *fsel = m.f.select.get_data();

  Set<int> out;
  for (int vi : m.v) {
    if (vsel->get(vi)) {
      out.add(vi);
    }
  }
  for (int ei : m.e) {
    if (esel->get(ei)) {
      out.add(m.e.vs[ei][0]);
      out.add(m.e.vs[ei][1]);
    }
  }
  for (int fi : m.f) {
    if (!fsel->get(fi)) {
      continue;
    }
    for (int l = m.f.l[fi]; l != ELEM_NONE; l = m.l.next[l]) {
      int c0 = m.l.c[l], c = c0;
      do {
        out.add(m.c.v[c]);
        c = m.c.next[c];
      } while (c != c0);
    }
  }
  return out;
}

/* Shared resolve shape: explicit selection in the op's own domain, then either
 * prefer-explicit (derive only fills an empty domain) or union. */
template<typename ExplicitFn, typename DeriveFn>
static litestl::util::Set<int> resolveSelection(bool preferOpDomain,
                                                ExplicitFn &&explicitSel,
                                                DeriveFn &&derived)
{
  Set<int> explicitSet = explicitSel();
  if (preferOpDomain && explicitSet.size() > 0) {
    return explicitSet;
  }
  Set<int> out = derived();
  for (int i : explicitSet) {
    out.add(i);
  }
  return out;
}

litestl::util::Set<int> resolveFaceSelection(Mesh &m, bool preferOpDomain)
{
  return resolveSelection(
      preferOpDomain,
      [&] {
        auto *fsel = m.f.select.get_data();
        Set<int> s;
        for (int f : m.f) {
          if (fsel->get(f)) {
            s.add(f);
          }
        }
        return s;
      },
      [&] { return deriveFaceSelection(m, DeriveRule::All); });
}

litestl::util::Set<int> resolveEdgeSelection(Mesh &m, bool preferOpDomain)
{
  return resolveSelection(
      preferOpDomain,
      [&] {
        auto *esel = m.e.select.get_data();
        Set<int> s;
        for (int e : m.e) {
          if (esel->get(e)) {
            s.add(e);
          }
        }
        return s;
      },
      [&] { return deriveEdgeSelection(m, DeriveRule::All); });
}

litestl::util::Set<int> resolveVertSelection(Mesh &m, bool preferOpDomain)
{
  return resolveSelection(
      preferOpDomain,
      [&] {
        auto *vsel = m.v.select.get_data();
        Set<int> s;
        for (int v : m.v) {
          if (vsel->get(v)) {
            s.add(v);
          }
        }
        return s;
      },
      [&] { return deriveVertSelection(m); });
}

void regionBoundaryEdges(Mesh &m, litestl::util::Vector<int> &out)
{
  auto *fsel = m.f.select.get_data();
  for (int e : m.e) {
    int c0 = m.e.c[e];
    if (c0 == ELEM_NONE) {
      continue;
    }
    int sel = 0, c = c0;
    do {
      int f = m.l.f[m.c.l[c]];
      if (fsel->get(f)) {
        sel++;
      }
      c = m.c.radial_next[c];
    } while (c != c0);
    if (sel == 1) {
      out.append(e);
    }
  }
}

void gatherMovableVerts(Mesh &m, litestl::util::Vector<int> &out)
{
  Set<int> seen = deriveVertSelection(m);
  for (int vi : seen) {
    out.append(vi);
  }
}

} // namespace sculptcore::mesh
