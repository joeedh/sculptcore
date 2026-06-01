#pragma once

/* Dynamic topology — local remeshing under a sculpt dab.
 *
 * Given a sphere (center, radius), split edges longer than `l_max` and
 * collapse edges shorter than `l_min` until the region's edge lengths fall
 * within the band, keeping the mesh triangulated and manifold. This is the
 * CPU core of the dynamic-topology feature (plan: documentation/plans/
 * dynamic-topology.md, milestone M2). It is intentionally free of spatial /
 * brush / meshlog dependencies: it mutates only the `mesh::Mesh`. The caller
 * (the brush dab loop) is responsible for thawing/refreezing topology around
 * a stroke, opening a meshlog topo chunk, and marking touched spatial nodes
 * dirty afterward.
 *
 * Parallelism model: each round selects a maximal independent set of
 * candidate edges (no two sharing affected geometry) and applies them. This
 * is overkill for a single-threaded apply, but it is the structure that maps
 * directly onto the future GPU / multi-threaded path, and it keeps each
 * round's edits non-interfering and deterministic. Determinism is seeded so
 * the per-op trace and CPU/GPU parity (plan M5/M7) are reproducible.
 */

#include "mesh/mesh.h"
#include "mesh/mesh_callbacks.h"
#include "mesh/mesh_iter.h"
#include "mesh/utils/edge_collapse.h"
#include "mesh/utils/edge_split.h"

#include "litestl/math/vector.h"
#include "litestl/util/rand.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdint>

namespace sculptcore::dyntopo {

enum class DynTopoMode { Subdivide, Collapse, Both };

struct DynTopoParams {
  float l_max = 0.10f;  /* split edges longer than this */
  float l_min = 0.04f;  /* collapse edges shorter than this (keep < l_max) */
  DynTopoMode mode = DynTopoMode::Both;
  /* The 1-triangle -> 2 split scheme cascades through spoke edges, so a dab
   * needs more independent-set rounds than a naive length-halving estimate.
   * 50 converges small/medium dabs; M7 will tune round efficiency for 5M tris. */
  int max_rounds = 50;
};

struct DynTopoStats {
  int splits = 0;
  int collapses = 0;
  int rounds = 0;
  bool capped = false; /* hit max_rounds with work still pending */
};

namespace detail {

inline float edgeLen(mesh::Mesh &m, int e)
{
  return (m.v.co[m.e.vs[e][0]] - m.v.co[m.e.vs[e][1]]).length();
}

inline litestl::math::float3 edgeMid(mesh::Mesh &m, int e)
{
  return (m.v.co[m.e.vs[e][0]] + m.v.co[m.e.vs[e][1]]) * 0.5f;
}

/* Lock the verts a split affects: just the two edge endpoints. Any two edges
 * of one triangle share an endpoint, so endpoint-locking already makes the
 * round's splits face-disjoint (no two touch the same triangle) and
 * disk-race-free (no two write the same vertex's disk), without the apex
 * over-conservatism that would defer most edges to later rounds. */
inline void lockSplit(mesh::Mesh &m, int e, litestl::util::Set<int> &locked)
{
  locked.add(m.e.vs[e][0]);
  locked.add(m.e.vs[e][1]);
}

/* Lock the verts a collapse affects: the full one-ring of both endpoints. */
inline void lockCollapse(mesh::Mesh &m, int e, litestl::util::Set<int> &locked)
{
  for (int side = 0; side < 2; side++) {
    int v = m.e.vs[e][side];
    locked.add(v);
    if (m.v.e[v] == ELEM_NONE) {
      continue;
    }
    for (int ei : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
      int o = (m.e.vs[ei][0] == v) ? m.e.vs[ei][1] : m.e.vs[ei][0];
      locked.add(o);
    }
  }
}

/* True if none of the verts a candidate would affect are already locked. */
inline bool splitFree(mesh::Mesh &m, int e, const litestl::util::Set<int> &locked)
{
  if (m.e.c[e] == ELEM_NONE) {
    return false; /* wire edge: no triangle to split */
  }
  return !locked.contains(m.e.vs[e][0]) && !locked.contains(m.e.vs[e][1]);
}

inline bool collapseFree(mesh::Mesh &m, int e, const litestl::util::Set<int> &locked)
{
  for (int side = 0; side < 2; side++) {
    int v = m.e.vs[e][side];
    if (locked.contains(v)) {
      return false;
    }
    if (m.v.e[v] == ELEM_NONE) {
      continue;
    }
    for (int ei : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
      int o = (m.e.vs[ei][0] == v) ? m.e.vs[ei][1] : m.e.vs[ei][0];
      if (locked.contains(o)) {
        return false;
      }
    }
  }
  return true;
}

} // namespace detail

/* Remesh the region of `m` within sphere(center, radius). Returns op counts.
 * Deterministic given `seed`. Does not touch spatial/meshlog state. */
inline DynTopoStats applyBrushDab(mesh::Mesh &m, litestl::math::float3 center,
                                  float radius, const DynTopoParams &p,
                                  uint32_t seed, mesh::MeshCallbacks *cb = nullptr)
{
  using namespace litestl;
  using namespace litestl::util;

  const bool doSplit =
      p.mode == DynTopoMode::Subdivide || p.mode == DynTopoMode::Both;
  const bool doCollapse =
      p.mode == DynTopoMode::Collapse || p.mode == DynTopoMode::Both;
  const float r2 = radius * radius;

  DynTopoStats stats;

  struct Cand {
    int edge;
    bool split;
  };

  for (int round = 0; round < p.max_rounds; round++) {
    /* 1. Build candidates: in-region edges outside the [l_min, l_max] band. */
    Vector<Cand> cands;
    for (int e : m.e) {
      if (m.e.c[e] == ELEM_NONE) {
        continue; /* skip wire edges */
      }
      if ((detail::edgeMid(m, e) - center).lengthSqr() > r2) {
        continue; /* outside the dab */
      }
      float L = detail::edgeLen(m, e);
      if (doSplit && L > p.l_max) {
        cands.append({e, true});
      } else if (doCollapse && L < p.l_min) {
        cands.append({e, false});
      }
    }
    if (cands.isEmpty()) {
      break; /* converged */
    }

    /* 2. Deterministic shuffle so selection isn't biased by edge index and
     *    so a seed reproduces the exact sequence (Fisher-Yates). */
    Random rnd(seed + uint32_t(round) * 2654435761u);
    for (int i = int(cands.size()) - 1; i > 0; i--) {
      int j = int(rnd.get_int() % uint32_t(i + 1));
      Cand tmp = cands[i];
      cands[i] = cands[j];
      cands[j] = tmp;
    }

    /* 3. Greedily select a maximal independent set. */
    Set<int> locked;
    Vector<Cand> picked;
    for (const Cand &c : cands) {
      bool free = c.split ? detail::splitFree(m, c.edge, locked)
                          : detail::collapseFree(m, c.edge, locked);
      if (!free) {
        continue;
      }
      if (c.split) {
        detail::lockSplit(m, c.edge, locked);
      } else {
        detail::lockCollapse(m, c.edge, locked);
      }
      picked.append(c);
    }

    /* 4. Apply the independent set. Edits are non-interfering, so a stale
     *    index is impossible; an op may still no-op (e.g. a collapse the link
     *    condition refuses) — that just doesn't count. */
    int applied = 0;
    for (const Cand &c : picked) {
      if (m.e.freemap[c.edge]) {
        continue;
      }
      if (c.split) {
        mesh::EdgeSplitResult res;
        if (mesh::splitEdge(m, c.edge, &res, cb)) {
          stats.splits++;
          applied++;
        }
      } else {
        math::float3 mid = detail::edgeMid(m, c.edge);
        mesh::EdgeCollapseResult res;
        if (mesh::collapseEdge(m, c.edge, mid, /*blend=*/0.5f, &res, cb)) {
          stats.collapses++;
          applied++;
        }
      }
    }

    stats.rounds = round + 1;
    if (applied == 0) {
      break; /* nothing progressed (all refused) — avoid spinning */
    }
    if (round == p.max_rounds - 1) {
      stats.capped = true;
    }
  }

  return stats;
}

} // namespace sculptcore::dyntopo
