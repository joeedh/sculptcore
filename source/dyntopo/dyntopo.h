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
#include "litestl/util/span.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdint>

namespace sculptcore::dyntopo {

enum class DynTopoMode { Subdivide, Collapse, Both };

struct DynTopoParams {
  float l_max = 0.10f;  /* split edges longer than this (at the brush center) */
  float l_min = 0.04f;  /* collapse edges shorter than this (keep < l_max) */
  DynTopoMode mode = DynTopoMode::Both;
  /* Graded target (sizing field, plan M7.1a): relax l_max/l_min outward from the
   * brush center by (1 + grade * dist/radius), so the refinement grades smoothly
   * into the surrounding mesh instead of cliffing at the brush rim — fewer
   * splits and no high-valence boundary hubs. 0 = uniform (original behavior). */
  float grade = 0.0f;
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
 * Deterministic given `seed`. Does not touch spatial/meshlog state.
 *
 * `seedVerts` is an optional set of verts known to cover the brush region (the
 * caller derives it from the spatial tree's in-region leaves). When given, round
 * 0 examines only the edges incident to those verts instead of scanning the
 * whole mesh — keeping the dab O(brush region) without a spatial dependency here
 * (inversion of control). Empty (the default) falls back to a full scan, which
 * the bare-mesh unit tests and any caller without a tree rely on. */
inline DynTopoStats applyBrushDab(mesh::Mesh &m, litestl::math::float3 center,
                                  float radius, const DynTopoParams &p,
                                  uint32_t seed, mesh::MeshCallbacks *cb = nullptr,
                                  litestl::util::span<const int> seedVerts = {})
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

  /* Frontier of verts whose incident edges might have fallen out of band since
   * last round (the previous round's candidate + created-edge endpoints). Round
   * 0 scans the whole mesh once to seed; later rounds stay local to the brush,
   * so a dab is O(brush region) per round rather than O(total edges) (see the
   * bench_dyntopo profiling finding). */
  Set<int> frontier;
  bool firstRound = true;

  for (int round = 0; round < p.max_rounds; round++) {
    /* 1. Build candidates: in-region edges outside the [l_min, l_max] band. */
    Vector<Cand> cands;
    Set<int> seen;
    auto consider = [&](int e) {
      if (m.e.freemap[e] || m.e.c[e] == ELEM_NONE || !seen.add(e)) {
        return; /* freed, wire, or already considered this round */
      }
      float d2 = (detail::edgeMid(m, e) - center).lengthSqr();
      if (d2 > r2) {
        return; /* outside the dab */
      }
      /* Graded target: relax the goal outward from the center (sizing field). */
      float tmax = p.l_max, tmin = p.l_min;
      if (p.grade > 0.0f && radius > 0.0f) {
        float scale = 1.0f + p.grade * (std::sqrt(d2) / radius);
        tmax *= scale;
        tmin *= scale;
      }
      float L = detail::edgeLen(m, e);
      if (doSplit && L > tmax) {
        cands.append({e, true});
      } else if (doCollapse && L < tmin) {
        cands.append({e, false});
      }
    };
    auto considerVertEdges = [&](int v) {
      if (v < 0 || v >= int(m.v.capacity()) || m.v.freemap[v] ||
          m.v.e[v] == ELEM_NONE) {
        return;
      }
      for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
        consider(e);
      }
    };
    if (firstRound) {
      if (seedVerts.size() > 0) {
        for (int v : seedVerts) {
          considerVertEdges(v); /* round 0, seeded: local to the brush */
        }
      } else {
        for (int e : m.e) {
          consider(e); /* round 0, unseeded: one full-mesh scan */
        }
      }
      firstRound = false;
    } else {
      for (int v : frontier) {
        considerVertEdges(v);
      }
    }
    if (cands.isEmpty()) {
      break; /* converged */
    }

    /* Seed next frontier with this round's candidate endpoints so deferred
     * (unpicked) candidates are re-examined next round. */
    Set<int> nextFrontier;
    for (const Cand &c : cands) {
      nextFrontier.add(m.e.vs[c.edge][0]);
      nextFrontier.add(m.e.vs[c.edge][1]);
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
    auto addCreated = [&](const Vector<int> &edges) {
      for (int e : edges) {
        if (!m.e.freemap[e]) {
          nextFrontier.add(m.e.vs[e][0]);
          nextFrontier.add(m.e.vs[e][1]);
        }
      }
    };
    for (const Cand &c : picked) {
      if (m.e.freemap[c.edge]) {
        continue;
      }
      if (c.split) {
        mesh::EdgeSplitResult res;
        if (mesh::splitEdge(m, c.edge, &res, cb)) {
          stats.splits++;
          applied++;
          addCreated(res.created_edges);
        }
      } else {
        math::float3 mid = detail::edgeMid(m, c.edge);
        mesh::EdgeCollapseResult res;
        if (mesh::collapseEdge(m, c.edge, mid, /*blend=*/0.5f, &res, cb)) {
          stats.collapses++;
          applied++;
          addCreated(res.created_edges);
        }
      }
    }
    frontier = std::move(nextFrontier);

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
