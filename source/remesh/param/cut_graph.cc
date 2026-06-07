#include "remesh/param/cut_graph.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

namespace sculptcore::remesh {

using litestl::util::Vector;
using sculptcore::mesh::AttrFlag;
using sculptcore::mesh::AttrType;
using sculptcore::mesh::BuiltinAttr;
using sculptcore::mesh::Mesh;

namespace {
// True for an interior, manifold (exactly two faces) edge; fills fa/fb.
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
} // namespace

CutGraphStats buildCutGraph(Mesh &m)
{
  CutGraphStats stats;
  const int fcap = int(m.f.capacity());

  BuiltinAttr<bool, ".remesh.e.is_cut", AttrFlag::TEMP> is_cut;
  is_cut.ensure(m.e.attrs);

  // Every interior manifold edge starts cut; the dual tree frees its branches.
  for (int e : m.e) {
    is_cut.set(e, false);
    int fa, fb;
    if (interiorEdge(m, e, fa, fb)) {
      is_cut.set(e, true);
    }
  }

  // Root the tree at a singular face when we have an index field.
  int root = ELEM_NONE;
  bool have_pole =
      m.v.attrs.has(AttrType::SHORT, litestl::util::string(".remesh.v.pole_index"));
  if (have_pole) {
    BuiltinAttr<short, ".remesh.v.pole_index"> pole;
    pole.ensure(m.v.attrs);
    for (int v : m.v) {
      if (pole[v] == 0) {
        continue;
      }
      stats.num_singularities++;
      if (root != ELEM_NONE) {
        continue;
      }
      int e0 = m.v.e[v];
      if (e0 == ELEM_NONE) {
        continue;
      }
      int c = m.e.c[e0];
      if (c != ELEM_NONE) {
        root = m.l.f[m.c.l[c]];
      }
    }
  }
  if (root == ELEM_NONE) {
    for (int f : m.f) {
      root = f;
      break;
    }
  }
  if (root == ELEM_NONE) {
    return stats; // empty mesh
  }

  // Dual spanning tree (DFS): tree edges are freed (non-cut), cotree stays cut.
  Vector<char> visited;
  visited.resize(fcap);
  for (int i = 0; i < fcap; i++) {
    visited[i] = 0;
  }
  Vector<int> stack;
  visited[root] = 1;
  stack.append(root);
  while (stack.size() > 0) {
    int f = stack.pop_back();
    int c0 = m.l.c[m.f.l[f]], cc = c0;
    do {
      int e = m.c.e[cc];
      int cr = m.c.radial_next[cc];
      if (cr != cc && m.c.radial_next[cr] == cc) {
        int fb = m.l.f[m.c.l[cr]];
        if (fb != f && !visited[fb]) {
          is_cut.set(e, false);
          visited[fb] = 1;
          stack.append(fb);
        }
      }
      cc = m.c.next[cc];
    } while (cc != c0);
  }

  for (int e : m.e) {
    int fa, fb;
    if (!interiorEdge(m, e, fa, fb)) {
      continue;
    }
    if (is_cut[e]) {
      stats.num_cut_edges++;
    } else {
      stats.num_tree_edges++;
    }
  }
  return stats;
}

} // namespace sculptcore::remesh
