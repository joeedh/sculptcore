#pragma once

/* Dynamic topology — local remeshing under a sculpt dab.
 *
 * Given a sphere (center, radius), split edges longer than `l_max` and
 * collapse edges shorter than `l_min` until the region's edge lengths fall
 * within the band, keeping the mesh triangulated and manifold. This is the
 * CPU core of the dynamic-topology feature (design: documentation/
 * dynamic-topology.md; plan: documentation/plans/dyntopo-m7-cascade.md). It
 * grew the graded target (M7.1a), a geometric flip sweep (M7.2), a per-dab
 * split budget, and tangential smoothing (M7.4); see the params below. It is
 * intentionally free of spatial /
 * brush / meshlog dependencies: it mutates only the `mesh::Mesh`. The caller
 * (the brush dab loop) is responsible for thawing/refreezing topology around
 * a stroke, opening a meshlog topo chunk, and marking touched spatial nodes
 * dirty afterward.
 *
 * Parallelism model: each round selects a maximal independent set of
 * candidate edges (no two sharing affected geometry) and applies them. This
 * is overkill for a single-threaded apply, but it keeps each round's edits
 * non-interfering and deterministic, and is the structure a multi-threaded (or
 * the now-optional GPU-assist; see the design doc's post-M7 re-evaluation) path
 * would build on. Determinism is seeded so results are reproducible for the
 * tests and cross-backend parity.
 */

#include "mesh/attribute_bool.h"
#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "mesh/mesh_callbacks.h"
#include "mesh/mesh_iter.h"
#include "mesh/utils/edge_collapse.h"
#include "mesh/utils/edge_flip.h"
#include "mesh/utils/edge_split.h"
#include "mesh/utils/triangulate.h"

#include "litestl/math/vector.h"
#include "litestl/util/rand.h"
#include "litestl/util/set.h"
#include "litestl/util/span.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdint>
#include <limits>

// Forward-declared so the param/stats structs can carry a `defineBindings()`
// hook without pulling the whole binding system into this hot header — the
// bodies live out-of-line in `dyntopo/bindings.cc`.
namespace litestl::binding::types {
template <typename CLS> struct Struct;
}

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
   * With do_flips on (M7.2) the spoke cascade is broken, so even aggressive 5M
   * dabs converge in well under 50 rounds (vs hitting this cap without flips). */
  int max_rounds = 50;
  /* M7.2: after each round, flip an interior edge to its opposite diagonal when
   * that diagonal is strictly shorter and the quad stays convex. The split
   * scheme connects each new midpoint to the triangle apex, and on slivers /
   * right-isoceles grid triangles that spoke is *longer* than the edge it
   * split, so it splits again — the cascade. Flipping the long spoke to the
   * short diagonal keeps triangles well-shaped during refinement and breaks the
   * chain. Length-only + convex is monotone (never lengthens an edge), so unlike
   * the rejected valence criterion it cannot create work. On by default: the
   * M7.2 A/B showed it makes aggressive refinement converge (vs hitting the
   * round cap), cuts splits ~3x, and drops the cascade's max valence from ~60-97
   * back to ~9-10. Set false for the pre-M7.2 baseline. */
  bool do_flips = true;
  /* Per-dab split budget (safety valve). 0 = unlimited (default; the bench /
   * tests rely on a dab fully converging). When > 0, the dab stops once it has
   * applied this many splits and sets stats.budget_hit; the still-out-of-band
   * edges are simply refined by the next dab (a moving brush re-touches the
   * region), so a one-shot heavy refine degrades to bounded latency instead of a
   * multi-hundred-ms frame. Calibrate to the frame budget: ~frame_ms / ms-per-
   * split (≈0.04ms/split at 5M with flips on). */
  int max_splits = 0;
  /* M7.4: tangential smoothing — the 4th Botsch-Kobbelt operator. After the
   * flips each round, slide region verts toward their 1-ring's area-weighted
   * centroid *in the tangent plane only* (the normal component is removed, so it
   * equalizes triangle sizes / kills residual slivers without shrinking the
   * surface or smoothing away sculpted detail). Off by default: it's a quality
   * nicety, not a perf/correctness fix, and it nudges geometry so it wants
   * interactive validation against the brush deform. Boundary verts are left
   * fixed; each move is clamped to half the shortest incident edge so it can't
   * fold a triangle. */
  bool do_smooth = false;
  float smooth_lambda = 0.5f; /* relaxation step (0..1) */
  /* Boundary-condition preservation. When true, the operators consult the
   * mesh's boundary overlays (seam / sharp / projected / poly-group / UV-chart
   * edge flags + the per-vert class) so a dab never tears a feature: feature
   * edges may still SPLIT (the flag is propagated to both children), but feature
   * verts are pinned against flip/smooth and only collapse *along* their own
   * collinear feature curve. Off = the original feature-agnostic remesh. */
  bool preserve_features = true;

  /* Bound out-of-line in dyntopo/bindings.cc (keeps binding headers out of this
   * hot header). Crosses the WASM/N-API seam by value, so the struct registers a
   * copy constructor there. */
  static litestl::binding::types::Struct<DynTopoParams> *defineBindings();
};

struct DynTopoStats {
  int splits = 0;
  int collapses = 0;
  int flips = 0;
  int smooths = 0;
  int rounds = 0;
  bool capped = false;     /* hit max_rounds with work still pending */
  bool budget_hit = false; /* stopped early on max_splits (more work remains) */

  static litestl::binding::types::Struct<DynTopoStats> *defineBindings();
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

/* Recover the flip quad of edge `e`: endpoints a,b and the two triangle apexes
 * c,d. False unless `e` is an interior manifold edge bounded by exactly two
 * triangles — the only shape flipEdge accepts (re-checks at apply, so a stale
 * candidate from an earlier flip this round is harmless). */
inline bool flipQuad(mesh::Mesh &m, int e, int &a, int &b, int &c, int &d)
{
  if (m.e.freemap[e]) {
    return false;
  }
  int c0 = m.e.c[e];
  if (c0 == ELEM_NONE) {
    return false; /* wire edge */
  }
  a = m.e.vs[e][0];
  b = m.e.vs[e][1];
  if (a == b) {
    return false;
  }
  c = d = ELEM_NONE;
  int nfaces = 0, cc = c0;
  do {
    int li = m.c.l[cc];
    if (m.l.size[li] != 3 || m.f.list_count[m.l.f[li]] != 1) {
      return false; /* non-triangle incident face */
    }
    int cn = m.c.next[cc];
    int cnn = m.c.next[cn];
    int va = m.c.v[cc], vb = m.c.v[cn], apex = m.c.v[cnn];
    if (va == a && vb == b) {
      c = apex;
    } else if (va == b && vb == a) {
      d = apex;
    }
    nfaces++;
    cc = m.c.radial_next[cc];
  } while (cc != c0);
  return nfaces == 2 && c != ELEM_NONE && d != ELEM_NONE && c != d;
}

/* Flip a-b -> c-d improves the mesh iff the new diagonal is strictly shorter
 * (monotone: tames the cascade, can't create longer edges) AND the quad
 * a-c-b-d is convex in its average plane (else the flip folds geometry — the
 * topological flipEdge doesn't check this). Convex iff a,b lie on opposite
 * sides of the c-d line (c,d are already on opposite sides of a-b, being apexes
 * of the two faces). The 0.998 epsilon makes flips strictly length-decreasing
 * so a pass can't cycle. */
inline bool flipShortens(mesh::Mesh &m, int a, int b, int c, int d)
{
  using litestl::math::float3;
  float3 A = m.v.co[a], B = m.v.co[b], C = m.v.co[c], D = m.v.co[d];
  if ((C - D).lengthSqr() >= (A - B).lengthSqr() * 0.998f) {
    return false;
  }
  float3 n = (B - A).cross(C - A) + (A - B).cross(D - B); /* avg face normal */
  if (n.normalize() == 0.0f) {
    return false; /* folded / degenerate quad */
  }
  float3 cd = D - C;
  float sa = cd.cross(A - C).dot(n);
  float sb = cd.cross(B - C).dot(n);
  return sa * sb < 0.0f; /* a,b strictly opposite sides of c-d => convex */
}

/* Tangential-smoothing target for vertex v (M7.4): slide v toward the
 * area-weighted centroid of its incident triangles, keeping only the in-tangent-
 * plane component (so the surface isn't shrunk / flattened). Vertex normal and
 * centroid are computed live from the 1-ring (stored normals go stale across
 * splits). Returns false (no move) for a vertex touching a boundary / non-
 * manifold / non-triangle edge, or a degenerate ring; otherwise `out` is the new
 * position, with the move clamped to half the shortest incident edge so a thin
 * triangle can't fold. Reads positions only — caller writes simultaneously. */
inline bool smoothTangent(mesh::Mesh &m, int v, float lambda,
                          litestl::math::float3 &out)
{
  using litestl::math::float3;
  int e0 = m.v.e[v];
  if (e0 == ELEM_NONE) {
    return false;
  }
  float3 P = m.v.co[v];
  float3 nAccum(0.0f), cAccum(0.0f);
  float areaSum = 0.0f;
  float minLen2 = std::numeric_limits<float>::max();

  for (int e : mesh::EdgeOfVertIter(&m, v, e0)) {
    int cc = m.e.c[e];
    if (cc == ELEM_NONE) {
      return false; /* wire edge in the ring */
    }
    int o = (m.e.vs[e][0] == v) ? m.e.vs[e][1] : m.e.vs[e][0];
    float l2 = (m.v.co[o] - P).lengthSqr();
    if (l2 < minLen2) {
      minLen2 = l2;
    }
    /* Walk the edge's radial: require exactly two triangle faces (interior
     * manifold). Each incident face is reached from both of v's edges that
     * touch it, so it's counted twice — uniform, and cancels in the ratios. */
    int nf = 0, c = cc;
    do {
      int li = m.c.l[c];
      if (m.l.size[li] != 3 || m.f.list_count[m.l.f[li]] != 1) {
        return false; /* non-triangle incident face */
      }
      int c2 = m.c.next[c], c3 = m.c.next[c2];
      float3 A = m.v.co[m.c.v[c]], B = m.v.co[m.c.v[c2]], C = m.v.co[m.c.v[c3]];
      float3 fn = (B - A).cross(C - A); /* |fn| = 2 * area, dir = face normal */
      float area = fn.length();
      nAccum += fn;
      cAccum += (A + B + C) * (area * (1.0f / 3.0f));
      areaSum += area;
      nf++;
      c = m.c.radial_next[c];
    } while (c != cc);
    if (nf != 2) {
      return false; /* boundary / non-manifold edge -> leave v fixed */
    }
  }

  if (areaSum < 1e-20f) {
    return false;
  }
  float3 n = nAccum;
  if (n.normalize() == 0.0f) {
    return false;
  }
  float3 delta = (cAccum / areaSum) - P;
  delta -= n * delta.dot(n); /* tangential only */
  delta *= lambda;
  float move2 = delta.lengthSqr();
  float cap2 = minLen2 * 0.25f; /* <= half the shortest incident edge */
  if (move2 > cap2 && move2 > 0.0f) {
    delta *= std::sqrt(cap2 / move2);
  }
  out = P + delta;
  return true;
}

/* Resolved boundary-overlay edge views + per-vert class, for feature-preserving
 * remeshing. `init` is a no-op (and `active` stays false) when the caller didn't
 * ask to preserve features, so every query is a cheap early-out. */
struct FeatureViews {
  mesh::Mesh *m = nullptr;
  mesh::BoolAttrView *proj = nullptr, *sharp = nullptr, *seam = nullptr;
  mesh::BoolAttrView *pg = nullptr, *uv = nullptr;
  bool active = false;

  void init(mesh::Mesh &mesh, bool preserve)
  {
    m = &mesh;
    active = preserve;
    if (!preserve) {
      return;
    }
    using namespace mesh::boundary;
    proj = findBoolEdgeView(&mesh, EDGE_PROJECTED);
    sharp = findBoolEdgeView(&mesh, EDGE_SHARP);
    seam = findBoolEdgeView(&mesh, EDGE_SEAM);
    pg = findBoolEdgeView(&mesh, EDGE_POLYGROUP);
    uv = findBoolEdgeView(&mesh, EDGE_UVCHART);
  }

  /* The feature-type bitmask (boundary::BoundaryClass bits) carried by edge e. */
  int edgeMask(int e) const
  {
    using namespace mesh::boundary;
    int mask = 0;
    if (proj && (*proj)[e]) mask |= BC_PROJECTED;
    if (sharp && (*sharp)[e]) mask |= BC_SHARP;
    if (seam && (*seam)[e]) mask |= BC_SEAM;
    if (pg && (*pg)[e]) mask |= BC_POLYGROUP;
    if (uv && (*uv)[e]) mask |= BC_UVCHART;
    return mask;
  }

  bool isFeatureEdge(int e) const
  {
    return active && edgeMask(e) != 0;
  }

  bool isFeatureVert(int v) const
  {
    return active && mesh::boundary::vertClass(m, v) != 0;
  }
};

/* A feature edge may collapse only *along its own collinear curve*: both
 * endpoints must be simple interior points of one uniform feature curve (exactly
 * two incident feature edges, all sharing e's exact feature-type signature, no
 * junction/corner). This lets a feature line coarsen without tearing or eroding
 * corners. (Decision B: pin + collinear collapse.) */
inline bool featureCollapseOk(mesh::Mesh &m, int e, const FeatureViews &feat)
{
  int em = feat.edgeMask(e);
  if (em == 0) {
    return false;
  }
  for (int side = 0; side < 2; side++) {
    int v = m.e.vs[e][side];
    if (m.v.e[v] == ELEM_NONE) {
      return false;
    }
    int sameType = 0;
    for (int ei : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
      int eim = feat.edgeMask(ei);
      if (eim == 0) {
        continue;
      }
      if (eim != em) {
        return false; /* junction / mixed feature types -> corner, don't collapse */
      }
      sameType++;
    }
    if (sameType != 2) {
      return false; /* endpoint is a feature end / corner, not a clean interior */
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

  /* Boundary-overlay views for feature-preserving remeshing (inert when
   * p.preserve_features is false). */
  detail::FeatureViews feat;
  feat.init(m, p.preserve_features);

  /* Split / flip / smooth are triangle-only; dyntopo dynamically triangulates any
   * non-triangle face it encounters in the region first (incl. the graded-target
   * faces — already tris — and any imported/procedural n-gon). Fan-triangulate
   * with the callbacks so spatial/meshlog stay in sync; attrs are carried. Cheap
   * no-op on an all-triangle region (the common case). */
  {
    Set<int> triFaces;
    auto considerFaceTri = [&](int f) {
      if (f < 0 || f >= int(m.f.capacity()) || m.f.freemap[f] ||
          m.f.list_count[f] != 1) {
        return;
      }
      if (m.l.size[m.f.l[f]] == 3) {
        return;
      }
      triFaces.add(f);
    };
    auto considerVertFaces = [&](int v) {
      if (v < 0 || v >= int(m.v.capacity()) || m.v.freemap[v] ||
          m.v.e[v] == ELEM_NONE) {
        return;
      }
      for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
        int rc0 = m.e.c[e];
        if (rc0 == ELEM_NONE) {
          continue;
        }
        int rcc = rc0;
        do {
          considerFaceTri(m.l.f[m.c.l[rcc]]);
          rcc = m.c.radial_next[rcc];
        } while (rcc != rc0);
      }
    };
    if (seedVerts.size() > 0) {
      for (int v : seedVerts) {
        considerVertFaces(v);
      }
    } else {
      for (int f : m.f) {
        considerFaceTri(f);
      }
    }
    for (int f : triFaces) {
      mesh::triangulateFaceFanCb(m, f, cb);
    }
  }

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
  bool budgetHit = false;

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
        cands.append({e, true}); /* split always allowed; flags propagate */
      } else if (doCollapse && L < tmin) {
        /* Feature preservation (Decision B): pin feature verts, but allow a
         * feature edge to collapse along its own collinear curve. */
        if (feat.active) {
          bool fv0 = feat.isFeatureVert(m.e.vs[e][0]);
          bool fv1 = feat.isFeatureVert(m.e.vs[e][1]);
          if (fv0 || fv1) {
            if (feat.isFeatureEdge(e)) {
              if (!detail::featureCollapseOk(m, e, feat)) {
                return; /* corner / junction / mixed curve: don't collapse */
              }
            } else {
              return; /* non-feature edge touching a feature vert: would tear */
            }
          }
        }
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
          /* New geometry carries split-propagated source flags; mark it so the
           * caller's recomputeDirty refreshes the derived flags + vert class. */
          if (feat.active) {
            mesh::boundary::markEdgeDirty(&m, e);
            mesh::boundary::markVertDirty(&m, m.e.vs[e][0]);
            mesh::boundary::markVertDirty(&m, m.e.vs[e][1]);
          }
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
          if (p.max_splits > 0 && stats.splits >= p.max_splits) {
            budgetHit = true;
            break; /* stop applying; flip sweep below still runs on what we did */
          }
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

    /* 5. Geometric flip sweep (M7.2): shorten the long spokes this round's
     *    splits just created, before they cascade into more splits. Collect the
     *    in-region interior edges around the touched verts first (read-only),
     *    then apply — flipping mutates disks, so we never flip while walking
     *    one. Each helper re-validates, so a flip invalidating a later candidate
     *    is safe. Flipped apexes re-enter the frontier (their lengths changed). */
    if (p.do_flips) {
      Vector<int> fverts;
      for (int v : nextFrontier) {
        fverts.append(v);
      }
      Vector<int> flipCands;
      Set<int> eseen;
      for (int v : fverts) {
        if (v < 0 || v >= int(m.v.capacity()) || m.v.freemap[v] ||
            m.v.e[v] == ELEM_NONE) {
          continue;
        }
        for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
          if (m.e.freemap[e] || !eseen.add(e)) {
            continue;
          }
          if ((detail::edgeMid(m, e) - center).lengthSqr() > r2) {
            continue;
          }
          if (feat.isFeatureEdge(e)) {
            continue; /* never flip a feature edge (would destroy the curve) */
          }
          flipCands.append(e);
        }
      }
      for (int e : flipCands) {
        int a, b, cc, dd;
        if (!detail::flipQuad(m, e, a, b, cc, dd) ||
            !detail::flipShortens(m, a, b, cc, dd)) {
          continue;
        }
        if (mesh::flipEdge(m, e, nullptr, cb)) {
          stats.flips++;
          nextFrontier.add(cc);
          nextFrontier.add(dd);
        }
      }
    }

    /* 6. Tangential smoothing (M7.4): relax the touched in-region verts toward
     *    their 1-ring centroid, in-plane. Simultaneous (Jacobi) update — all
     *    targets are read from current positions, then written — so it's
     *    order-independent and deterministic. Position-only: no topology event,
     *    so no cb; the region's leaves are already bounds-dirty from the splits. */
    if (p.do_smooth) {
      Vector<int> sverts;
      Vector<math::float3> spos;
      for (int v : nextFrontier) {
        if (v < 0 || v >= int(m.v.capacity()) || m.v.freemap[v]) {
          continue;
        }
        if ((m.v.co[v] - center).lengthSqr() > r2) {
          continue;
        }
        if (feat.isFeatureVert(v)) {
          continue; /* pin feature verts on their curve (v1: no tangent slide) */
        }
        math::float3 np;
        if (detail::smoothTangent(m, v, p.smooth_lambda, np)) {
          sverts.append(v);
          spos.append(np);
        }
      }
      for (int i = 0; i < int(sverts.size()); i++) {
        m.v.co[sverts[i]] = spos[i];
      }
      stats.smooths += int(sverts.size());
    }

    frontier = std::move(nextFrontier);

    stats.rounds = round + 1;
    if (budgetHit) {
      stats.budget_hit = true;
      stats.capped = true;
      break; /* hit the per-dab split budget — rest is the next dab's work */
    }
    if (applied == 0) {
      break; /* nothing progressed (all refused) — avoid spinning */
    }
    if (round == p.max_rounds - 1) {
      stats.capped = true;
    }
  }

  /* Topology near features changed: the split-propagated source flags need the
   * derived poly-group / UV-chart flags + per-vert class recomputed. Flag it; the
   * caller folds it in (recomputeDirty) at the next dab / stroke end while links
   * are live. */
  if (feat.active && (stats.splits + stats.collapses) > 0) {
    m.boundaryDirty = true;
  }

  return stats;
}

} // namespace sculptcore::dyntopo
