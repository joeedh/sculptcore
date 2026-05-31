#include "mesh_path.h"

#include "mesh.h"
#include "mesh_iter.h"

#include "litestl/util/binaryHeap.h"

namespace sculptcore::mesh {

using litestl::util::Vector;

bool shortestEdgePath(MeshBase *m, int vStart, int vEnd, Vector<int> &outVerts)
{
  outVerts.clear();
  const int n = m->v.count;
  if (vStart < 0 || vEnd < 0 || vStart >= n || vEnd >= n) {
    return false;
  }

  constexpr double INF = 1e300;
  Vector<double> dist;
  Vector<int> prev;
  Vector<char> done;
  dist.resize(n);
  prev.resize(n);
  done.resize(n);
  for (int i = 0; i < n; i++) {
    dist[i] = INF;
    prev[i] = -1;
    done[i] = 0;
  }

  // Lazy Dijkstra: the heap has no decrease-key, so a vertex may be pushed
  // several times; the first (smallest) pop finalizes it and later stale pops
  // are skipped via `done`.
  dist[vStart] = 0.0;
  litestl::util::BinaryHeap<int> heap;
  heap.push(vStart, 0.0);

  while (!heap.empty()) {
    int v = heap.pop();
    if (done[v]) {
      continue;
    }
    done[v] = 1;
    if (v == vEnd) {
      break;
    }
    auto cv = m->v.co[v];
    for (int e : EdgeOfVertIter(m, v, m->v.e[v])) {
      int other = m->e.vs[e][0] == v ? m->e.vs[e][1] : m->e.vs[e][0];
      if (done[other]) {
        continue;
      }
      double w = double((m->v.co[other] - cv).length());
      double nd = dist[v] + w;
      if (nd < dist[other]) {
        dist[other] = nd;
        prev[other] = v;
        heap.push(other, nd);
      }
    }
  }

  if (vEnd != vStart && !done[vEnd]) {
    return false; // unreachable
  }

  // Reconstruct [vEnd .. vStart] via prev, then reverse into outVerts.
  Vector<int> rev;
  for (int v = vEnd; v != -1; v = prev[v]) {
    rev.append(v);
    if (v == vStart) {
      break;
    }
  }
  if (rev.size() == 0 || rev[rev.size() - 1] != vStart) {
    return false;
  }
  for (int i = (int)rev.size() - 1; i >= 0; i--) {
    outVerts.append(rev[i]);
  }
  return true;
}

} // namespace sculptcore::mesh
