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
#include "mesh/utils/pinch_off.h"
#include "mesh/utils/triangulate.h"
#include "mesh/uv_reproject.h"

#include "dyntopo/dyntopo_trace.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/map.h"
#include "litestl/util/rand.h"
#include "litestl/util/set.h"
#include "litestl/util/span.h"
#include "litestl/util/vector.h"

#include "platform/time.h"

#include <algorithm>
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

/** Which edges a dab may touch. The two values are an A/B pair: one is our own
 * region rule, the other a port of Blender's, so the difference can be measured
 * rather than argued about. */
enum class DynTopoRegion {
  /** An edge is a candidate when its midpoint lies inside the dab sphere.
   * Refinement stops at the rim and nothing outside the brush is ever touched.
   * The default, and what every shipped stroke has used. */
  Sphere,
  /** Blender's `pbvh_bmesh.cc` scheme. Seeds from faces whose closest point is
   * inside the sphere, so a triangle far larger than the brush still qualifies —
   * the case the midpoint test misses entirely. From each seed edge it then
   * walks outward across mesh adjacency with a goal length that grows
   * `graded_generation_scale` per hop, giving a geometric density falloff
   * outside the brush in place of a cliff. That falloff is also what stops the
   * split scheme fanning one apex into a high-valence hub. */
  GradedRecursive,
};

struct DynTopoParams {
  float l_max = 0.10f; /* split edges longer than this (at the brush center) */
  float l_min = 0.04f; /* collapse edges shorter than this (clamped to l_max/2) */
  DynTopoMode mode = DynTopoMode::Both;
  /* Graded target (sizing field, plan M7.1a): relax l_max/l_min outward from the
   * brush center by (1 + grade * dist/radius), so the refinement grades smoothly
   * into the surrounding mesh instead of cliffing at the brush rim — fewer
   * splits and no high-valence boundary hubs. 0 = uniform (original behavior). */
  float grade = 0.0f;
  /* Region gating; see DynTopoRegion. The four knobs below are inert under
   * Sphere, which is the default. */
  DynTopoRegion region = DynTopoRegion::Sphere;
  /* Blender's `even_generation_scale`. The goal length multiplies by this each
   * hop out from the seed, so an edge k hops away must exceed `l_max * this^k`
   * before it is refined. This factor is the density gradation. Applied to
   * linear length. */
  float graded_generation_scale = 1.6f;
  /* Blender's `even_edgelen_threshold`. The walk only steps into a neighbour
   * that is also longer than the edge which reached it, so a merely skinny
   * triangle does not drag its neighbours in. Applied to squared length, as
   * Blender applies it, which keeps the constant transferable: the default 1.2
   * is about 1.095x in linear length rather than 1.2x. */
  float graded_len_sq_factor = 1.2f;
  /* Hard cap on hop depth. Both gates above grow geometrically, so the walk
   * terminates well before this on any sane mesh. It exists so that zero-length
   * edges or NaN coordinates cannot spin it. */
  int graded_max_hops = 12;
  /* Blender's post-split valence relief. A vert carrying more than this many
   * edges has all of them offered for splitting at the unscaled l_max, with no
   * hop or distance gate, which is what stops one apex being fanned when the
   * edge opposite it is split over and over. 0 disables it. Note this triggers a
   * split rather than a flip: splitting only ever shortens an edge, so it cannot
   * create work, which is the objection that got the valence flip criterion
   * rejected at `do_flips` below. */
  int graded_valence_relief = 8;
  /* Tier 9 adaptive sizing: name of an optional per-vertex FLOAT attribute
   * holding a relative size scale s(v) (1 = nominal). When present each edge's
   * [l_min, l_max] band is multiplied by the mean of its endpoints' s, so the BK
   * loop refines where s < 1 and coarsens where s > 1 — a curvature size field,
   * not the radial `grade` (which is meaningless over a whole-mesh dab). Split
   * interpolates the attr onto the midpoint vert, so grading survives refinement.
   * null/absent = uniform band (default; takes precedence over `grade`). Aliases
   * caller storage (a string literal / longer-lived buffer); never bound to JS. */
  const char *size_attr = nullptr;
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
  /* Per-dab collapse budget — the max_splits analog for the decimation
   * direction. A brush whose detail target is coarser than the mesh
   * mass-collapses the region on first touch (tens of thousands of collapses
   * across rounds — a multi-hundred-ms dab at 1.5M). When > 0 the dab stops
   * after this many collapses (stats.budget_hit) and later dabs finish the
   * decimation. 0 = unlimited (tests/bench rely on full convergence). */
  int max_collapses = 0;
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
  /* Re-anchor corner UVs after the tangential smooth (uv_reproject.h): each
   * slid vertex's UVs are re-interpolated on its pre-smooth 1-ring so textures
   * don't swim. Only meaningful with do_smooth; off = pre-P11 behavior. */
  bool reproject_uvs = false;
  /* Boundary-condition preservation. When true, the operators consult the
   * mesh's boundary overlays (seam / sharp / projected / poly-group / UV-chart
   * edge flags + the per-vert class) so a dab never tears a feature: feature
   * edges may still SPLIT (the flag is propagated to both children), but feature
   * verts are pinned against flip/smooth and only collapse *along* their own
   * collinear feature curve. Off = the original feature-agnostic remesh. */
  bool preserve_features = true;
  /* Geometric corner gate on the collinear feature-curve collapse: refuse it
   * when either endpoint's two feature edges bend more than this angle (radians)
   * from straight — a corner the topological test can't see when both its edges
   * carry one feature type (e.g. a square rim's corners). 0 (default) = off. */
  float feature_corner_angle = 0.0f;

  /** Cut tubes thinned below the detail size. The link condition refuses every collapse of a
   * 3-vertex ring, which leaves such a tube as a string. When a collapse is refused because the
   * edge and a third vertex close a 3-cycle that is not a face, and all three edges are shorter
   * than `pinch_ring_factor` times their collapse threshold, the mesh is cut along that cycle
   * (mesh/utils/pinch_off.h) and both sides are capped. Off by default, so callers that do not
   * ask for it (the quad-remesh pre-pass, the tests) are unchanged. */
  bool pinch_thin = false;
  float pinch_ring_factor = 1.0f;
  /** A closed piece left by a cut is deleted when it has at most this many faces and its bounding
   * box diagonal is at most `cull_size` times the local split threshold. */
  int cull_max_faces = 64;
  float cull_size = 2.0f;
  /** Per-dab cap on cuts. 0 = unlimited. */
  int max_pinches = 0;

  /* Limit-cycle early-out. Stops a dab once it has run this many *consecutive*
   * low-progress rounds (<= 2 split+collapse ops each) — the signature of a
   * split<->collapse ping-pong that never reaches a fixed point: a freshly split
   * edge-half can land below l_min and be recollapsed, recreating the long edge,
   * so cands never empties and the dab spins to max_rounds. A healthy dab's churn
   * tail is only a handful of rounds, so 16 sits well clear of real convergence
   * (a no-op on it) and only trims the wasted tail of a genuine cycle. 0 =
   * disabled (the pre-fix spin-to-cap behavior, for A/B). */
  int max_stall_rounds = 16;

  /* Displacement-base coherence. 0 = off. When non-zero this is the active
   * stroke's generation stamp: the tangential smooth resamples each slid vert's
   * `.brush.disp.vec` so the derived base `co - disp` slides along the base
   * surface rather than picking up the slide's normal component. Split/collapse
   * merge the field through the attribute layer itself (attr_merge.cc). */
  uint32_t dispGen = 0;

  /* Optional per-round triangle-quality trace (split-sliver oscillation
   * detection, dyntopo_trace.h). Null (default) = no tracing, zero cost; when
   * set, each round appends a RoundQuality snapshot. Native-only diagnostic —
   * deliberately NOT registered in bindings.cc, so it never crosses the seam. */
  DynTopoTrace *trace = nullptr;

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
  /* Deepest hop the graded walk reached over the whole dab. Measures how far
   * past the brush rim the refinement actually graded, and so is the headline
   * A/B number. Stays 0 under Sphere, and under GradedRecursive when nothing
   * outside the seed faces qualified. */
  int graded_hops = 0;
  /** Tubes cut by pinch_thin, faces deleted as small closed pieces, and valence-3 vertices merged
   * into the ring around them instead of being cut off. */
  int pinches = 0;
  int culled_faces = 0;
  int trivial_dissolves = 0;
  /* Bailed out of a split<->collapse limit cycle (max_stall_rounds). Native-only
   * diagnostic — deliberately NOT bound in bindings.cc, like DynTopoParams::trace. */
  bool stalled = false;

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

/* Generation-stamped dense-integer membership set. O(1) add/contains with no
 * hashing and zero per-dab allocation: a persistent (thread-local, see the
 * accessors) stamp buffer is reused across dabs and "cleared" each round by a
 * single generation bump. Replaces the per-round litestl Set<int> hash sets the
 * dyntopo profiling pinned as the scan/flip/MIS cost (and the 500ms+ rehash
 * spikes). Keys are dense element indices; add() auto-grows so a split creating
 * new verts/edges mid-round can never index out of bounds. */
struct GenSet {
  litestl::util::Vector<uint32_t> stamp;
  uint32_t gen = 0;

  void growTo(int n)
  {
    int old = int(stamp.size());
    if (old >= n) {
      return;
    }
    litestl::alloc::PermanentGuard guard; /* persistent buffer: not a leak */
    stamp.resize(n);
    for (int i = old; i < n; i++) {
      stamp[i] = 0;
    }
  }
  /* Pre-size to `n` and start a fresh generation. O(1) unless the buffer must
   * grow (rare) or the 32-bit generation wraps (re-zero, ~never). */
  void reset(int n)
  {
    growTo(n);
    if (++gen == 0) {
      for (int i = 0; i < int(stamp.size()); i++) {
        stamp[i] = 0;
      }
      gen = 1;
    }
  }
  bool add(int i) /* true iff newly added this generation */
  {
    if (uint32_t(i) >= uint32_t(stamp.size())) {
      growTo(i + 1);
    }
    if (stamp[i] == gen) {
      return false;
    }
    stamp[i] = gen;
    return true;
  }
  bool contains(int i) const
  {
    return uint32_t(i) < uint32_t(stamp.size()) && stamp[i] == gen;
  }
};

/* Persistent per-phase membership sets (one stamp buffer each, reused across
 * every dab of a stroke). Single-threaded per dab; thread_local guards against a
 * future parallel-dab caller without forcing a shared lock. */
inline GenSet &scanSeenSet()
{
  static thread_local GenSet s;
  return s;
}
inline GenSet &flipSeenSet()
{
  static thread_local GenSet s;
  return s;
}
inline GenSet &misLockedSet()
{
  static thread_local GenSet s;
  return s;
}
inline GenSet &traceFaceSeenSet() /* only used when DynTopoParams::trace is set */
{
  static thread_local GenSet s;
  return s;
}
inline GenSet &gradedFaceSeenSet() // DynTopoRegion::GradedRecursive seeding
{
  static thread_local GenSet s;
  return s;
}
inline GenSet &gradedRegionVertSet() // verts of every face the graded walk saw
{
  static thread_local GenSet s;
  return s;
}

/** GenSet's value-carrying sibling: a generation-stamped dense float map that
 * keeps only the smallest value written per key. The graded walk uses it as its
 * visited memo. An edge can be reached over several paths at different hop
 * depths, and the shallowest of those carries the smallest goal length and so is
 * the permissive one, which makes `improve` returning false mean the arriving
 * path has nothing new to say.
 *
 * The memo is what makes the port affordable. Blender's recursion dedups only
 * insertion (through BM_ELEM_TAG), never recursion, so it re-walks every path
 * carrying (parent length, limit). But the state that matters at an edge is (its
 * own length, limit), and an edge's own length does not depend on the path taken
 * to it. The limit is therefore the whole story, and keeping its minimum per
 * edge reproduces Blender's candidate set without the repeated descents. */
struct GenMinMap {
  litestl::util::Vector<uint32_t> stamp;
  litestl::util::Vector<float> value;
  uint32_t gen = 0;

  void growTo(int n)
  {
    int old = int(stamp.size());
    if (old >= n) {
      return;
    }
    litestl::alloc::PermanentGuard guard; /* persistent buffer: not a leak */
    stamp.resize(n);
    value.resize(n);
    for (int i = old; i < n; i++) {
      stamp[i] = 0;
      value[i] = 0.0f;
    }
  }
  void reset(int n)
  {
    growTo(n);
    if (++gen == 0) {
      for (int i = 0; i < int(stamp.size()); i++) {
        stamp[i] = 0;
      }
      gen = 1;
    }
  }
  /* True iff `i` was unset this generation, or `v` beats what it holds. */
  bool improve(int i, float v)
  {
    if (uint32_t(i) >= uint32_t(stamp.size())) {
      growTo(i + 1);
    }
    if (stamp[i] == gen && value[i] <= v) {
      return false;
    }
    stamp[i] = gen;
    value[i] = v;
    return true;
  }
  bool contains(int i) const
  {
    return uint32_t(i) < uint32_t(stamp.size()) && stamp[i] == gen;
  }
};
inline GenMinMap &gradedLimitMap()
{
  static thread_local GenMinMap m;
  return m;
}

/** Closest point to `p` on triangle (a, b, c) — Ericson, Real-Time Collision
 * Detection 5.1.5 (barycentric region test). Needed because the graded region
 * seeds on faces rather than edge midpoints. Blender's
 * `edge_queue_tri_in_sphere` runs exactly this, and it is the reason a triangle
 * far larger than the brush still qualifies there. */
inline litestl::math::float3 closestPointTri(litestl::math::float3 p,
                                             litestl::math::float3 a,
                                             litestl::math::float3 b,
                                             litestl::math::float3 c)
{
  using litestl::math::float3;
  float3 ab = b - a, ac = c - a, ap = p - a;
  float d1 = ab.dot(ap), d2 = ac.dot(ap);
  if (d1 <= 0.0f && d2 <= 0.0f) {
    return a;
  }
  float3 bp = p - b;
  float d3 = ab.dot(bp), d4 = ac.dot(bp);
  if (d3 >= 0.0f && d4 <= d3) {
    return b;
  }
  float vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
    float denom = d1 - d3;
    return a + ab * (denom != 0.0f ? d1 / denom : 0.0f);
  }
  float3 cp = p - c;
  float d5 = ab.dot(cp), d6 = ac.dot(cp);
  if (d6 >= 0.0f && d5 <= d6) {
    return c;
  }
  float vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
    float denom = d2 - d6;
    return a + ac * (denom != 0.0f ? d2 / denom : 0.0f);
  }
  float va = d3 * d6 - d5 * d4;
  if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
    float denom = (d4 - d3) + (d5 - d6);
    return b + (c - b) * (denom != 0.0f ? (d4 - d3) / denom : 0.0f);
  }
  float denom = va + vb + vc;
  if (denom == 0.0f) {
    return a; /* degenerate triangle */
  }
  denom = 1.0f / denom;
  return a + ab * (vb * denom) + ac * (vc * denom);
}

/** Fetch face `f`'s three corner verts. False if it isn't a plain triangle. */
inline bool triVerts(mesh::Mesh &m, int f, int out[3])
{
  if (f < 0 || f >= int(m.f.capacity()) || m.f.freemap[f] || m.f.list_count[f] != 1) {
    return false;
  }
  int li = m.f.l[f];
  if (m.l.size[li] != 3) {
    return false;
  }
  int c0 = m.l.c[li];
  out[0] = m.c.v[c0];
  out[1] = m.c.v[m.c.next[c0]];
  out[2] = m.c.v[m.c.prev[c0]];
  return true;
}

/** Blender's `edge_queue_tri_in_sphere`: does the triangle itself reach into
 * the dab? This test is strictly weaker than asking whether an edge midpoint is
 * in the dab, and the case it adds is a triangle that contains the brush. */
inline bool triInSphere(mesh::Mesh &m,
                        int f,
                        litestl::math::float3 center,
                        float r2,
                        int vs[3])
{
  if (!triVerts(m, f, vs)) {
    return false;
  }
  litestl::math::float3 cl =
      closestPointTri(center, m.v.co[vs[0]], m.v.co[vs[1]], m.v.co[vs[2]]);
  return (cl - center).lengthSqr() <= r2;
}

/** Does `v`'s disk hold more than `over` edges? Stops counting at the
 * threshold, so the valence relief costs O(threshold) rather than O(valence) on
 * the hubs it exists to catch. */
inline bool vertValenceOver(mesh::Mesh &m, int v, int over)
{
  if (v < 0 || v >= int(m.v.capacity()) || m.v.freemap[v] || m.v.e[v] == ELEM_NONE) {
    return false;
  }
  int n = 0;
  for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
    (void)e;
    if (++n > over) {
      return true;
    }
  }
  return false;
}

/* Smallest interior angle (radians) of triangle face f. Matches the survey
 * metric in mesh_validate.h (computeTier0Metrics) so the per-round trace and the
 * final whole-mesh survey are directly comparable. */
inline float triMinAngle(mesh::Mesh &m, int f)
{
  int li = m.f.l[f], c0 = m.l.c[li], cc = c0;
  float amin = 3.14159265f;
  do {
    int cn = m.c.next[cc], cp = m.c.prev[cc];
    litestl::math::float3 pco = m.v.co[m.c.v[cc]];
    litestl::math::float3 a = m.v.co[m.c.v[cn]] - pco;
    litestl::math::float3 b = m.v.co[m.c.v[cp]] - pco;
    float la = a.length(), lb = b.length();
    if (la > 1e-12f && lb > 1e-12f) {
      float cosa = a.dot(b) / (la * lb);
      cosa = cosa < -1.0f ? -1.0f : (cosa > 1.0f ? 1.0f : cosa);
      float ang = std::acos(cosa);
      if (ang < amin) {
        amin = ang;
      }
    }
    cc = cn;
  } while (cc != c0);
  return amin;
}

/* Snapshot the triangle quality of the faces incident to `verts` whose centroid
 * lies inside the dab (center, r2), into `q`. Each face is measured once. */
inline void measureRoundQuality(mesh::Mesh &m,
                                litestl::util::Set<int> &verts,
                                litestl::math::float3 center,
                                float r2,
                                float thin_angle,
                                RoundQuality &q)
{
  GenSet &fseen = traceFaceSeenSet();
  fseen.reset(int(m.f.capacity()));
  double angSum = 0.0;
  for (int v : verts) {
    if (v < 0 || v >= int(m.v.capacity()) || m.v.freemap[v] || m.v.e[v] == ELEM_NONE) {
      continue;
    }
    for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
      int rc0 = m.e.c[e];
      if (rc0 == ELEM_NONE) {
        continue;
      }
      int rcc = rc0;
      do {
        int f = m.l.f[m.c.l[rcc]];
        if (fseen.add(f)) {
          litestl::math::float3 c0co = m.v.co[m.c.v[m.l.c[m.f.l[f]]]];
          if ((c0co - center).lengthSqr() <= r2) {
            float ang = triMinAngle(m, f);
            if (q.tri_count == 0 || ang < q.min_angle) {
              q.min_angle = ang;
            }
            q.tri_count++;
            angSum += ang;
            if (ang < thin_angle) {
              q.thin_count++;
            }
          }
        }
        rcc = m.c.radial_next[rcc];
      } while (rcc != rc0);
    }
  }
  q.mean_min_angle = q.tri_count > 0 ? float(angSum / double(q.tri_count)) : 0.0f;
}

/* Lock the verts a split affects: just the two edge endpoints. Any two edges
 * of one triangle share an endpoint, so endpoint-locking already makes the
 * round's splits face-disjoint (no two touch the same triangle) and
 * disk-race-free (no two write the same vertex's disk), without the apex
 * over-conservatism that would defer most edges to later rounds. */
inline void lockSplit(mesh::Mesh &m, int e, GenSet &locked)
{
  locked.add(m.e.vs[e][0]);
  locked.add(m.e.vs[e][1]);
}

/* Lock the verts a collapse affects: the full one-ring of both endpoints. */
inline void lockCollapse(mesh::Mesh &m, int e, GenSet &locked)
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

/** Returns the edge joining v0 and v1, or ELEM_NONE. */
inline int edgeBetween(mesh::Mesh &m, int v0, int v1)
{
  if (m.v.e[v0] == ELEM_NONE) {
    return ELEM_NONE;
  }
  for (int e : mesh::EdgeOfVertIter(&m, v0, m.v.e[v0])) {
    if (m.e.vs[e][0] == v1 || m.e.vs[e][1] == v1) {
      return e;
    }
  }
  return ELEM_NONE;
}

/* True if none of the verts a candidate would affect are already locked. */
inline bool splitFree(mesh::Mesh &m, int e, const GenSet &locked)
{
  if (m.e.c[e] == ELEM_NONE) {
    return false; /* wire edge: no triangle to split */
  }
  return !locked.contains(m.e.vs[e][0]) && !locked.contains(m.e.vs[e][1]);
}

inline bool collapseFree(mesh::Mesh &m, int e, const GenSet &locked)
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

/* The brush's accumulated-displacement field, resolved for the active stroke.
 * `get` returns zero for a vert this stroke has not stamped — an untouched
 * vert's base *is* its live position. */
struct DispField {
  mesh::AttrData<litestl::math::float3> *vec = nullptr;
  mesh::AttrData<int> *gen = nullptr;
  int stamp = 0;

  litestl::math::float3 get(int v) const
  {
    if (gen->safe_get(v) != stamp) {
      return litestl::math::float3(0.0f);
    }
    return vec->safe_get(v);
  }
  explicit operator bool() const
  {
    return vec != nullptr && gen != nullptr;
  }
};

/* Tangential-smoothing target for vertex v (M7.4): slide v toward the
 * area-weighted centroid of its incident triangles, keeping only the in-tangent-
 * plane component (so the surface isn't shrunk / flattened). Vertex normal and
 * centroid are computed live from the 1-ring (stored normals go stale across
 * splits). Returns false (no move) for a vertex touching a boundary / non-
 * manifold / non-triangle edge, or a degenerate ring; otherwise `out` is the new
 * position, with the move clamped to half the shortest incident edge so a thin
 * triangle can't fold. Reads positions only — caller writes simultaneously.
 *
 * `df`/`dispOut`, when non-null, resample the displacement field at the vert's
 * post-slide location (same area weights and blend factor as the position), so
 * the derived base `co - disp` slides *along* the base surface instead of
 * inheriting the slide's normal component. */
inline bool smoothTangent(mesh::Mesh &m,
                          int v,
                          float lambda,
                          litestl::math::float3 &out,
                          const DispField *df = nullptr,
                          litestl::math::float3 *dispOut = nullptr)
{
  using litestl::math::float3;
  float3 dAccum(0.0f);
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
      if (df) {
        float3 dsum = df->get(m.c.v[c]) + df->get(m.c.v[c2]) + df->get(m.c.v[c3]);
        dAccum += dsum * (area * (1.0f / 3.0f));
      }
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
  float blend = lambda;
  float move2 = delta.lengthSqr();
  float cap2 = minLen2 * 0.25f; /* <= half the shortest incident edge */
  if (move2 > cap2 && move2 > 0.0f) {
    float s = std::sqrt(cap2 / move2);
    delta *= s;
    blend *= s;
  }
  out = P + delta;
  if (df && dispOut) {
    float3 d = df->get(v);
    *dispOut = d + ((dAccum / areaSum) - d) * blend;
  }
  return true;
}

/* Resolved boundary-overlay edge views + per-vert class, for feature-preserving
 * remeshing. `init` is a no-op (and `active` stays false) when the caller didn't
 * ask to preserve features, so every query is a cheap early-out. */
struct FeatureViews {
  mesh::Mesh *m = nullptr;
  mesh::BoolAttrView *proj = nullptr, *sharp = nullptr, *seam = nullptr;
  mesh::BoolAttrView *pg = nullptr, *uv = nullptr, *layer = nullptr;
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
    layer = findBoolEdgeView(&mesh, EDGE_LAYER_REGION);
  }

  /* The feature-type bitmask (boundary::BoundaryClass bits) carried by edge e. */
  int edgeMask(int e) const
  {
    using namespace mesh::boundary;
    int mask = 0;
    if (proj && (*proj)[e])
      mask |= BC_PROJECTED;
    if (sharp && (*sharp)[e])
      mask |= BC_SHARP;
    if (seam && (*seam)[e])
      mask |= BC_SEAM;
    if (pg && (*pg)[e])
      mask |= BC_POLYGROUP;
    if (uv && (*uv)[e])
      mask |= BC_UVCHART;
    if (layer && (*layer)[e])
      mask |= BC_LAYER_REGION;
    return mask;
  }

  bool isFeatureEdge(int e) const
  {
    return active && edgeMask(e) != 0;
  }

  /* A vertex is a feature vert iff it carries any incident feature edge. Derived
   * from the LIVE edge overlay (split/collapse propagate edge flags immediately),
   * not the persistent per-vertex class — that attr is only rebuilt by the caller's
   * recomputeDirty after the dab, so a freshly-split crease midpoint would read as
   * stale non-feature and let a non-feature edge collapse pinch the crease. */
  bool isFeatureVert(int v) const
  {
    if (!active || v < 0 || v >= int(m->v.capacity()) || m->v.freemap[v] ||
        m->v.e[v] == ELEM_NONE)
    {
      return false;
    }
    for (int e : mesh::EdgeOfVertIter(m, v, m->v.e[v])) {
      if (edgeMask(e) != 0) {
        return true;
      }
    }
    return false;
  }
};

/* A feature edge may collapse only *along its own collinear curve*: both
 * endpoints must be simple interior points of one uniform feature curve (exactly
 * two incident feature edges, all sharing e's exact feature-type signature, no
 * junction/corner). This lets a feature line coarsen without tearing or eroding
 * corners. (Decision B: pin + collinear collapse.) corner_angle > 0 adds the
 * geometric gate: an endpoint whose curve bends more than that from straight is
 * a corner too, even though it carries exactly two same-type edges. */
inline bool featureCollapseOk(mesh::Mesh &m,
                              int e,
                              const FeatureViews &feat,
                              float corner_angle = 0.0f)
{
  int em = feat.edgeMask(e);
  if (em == 0) {
    return false;
  }
  const float corner_dot = corner_angle > 0.0f ? -std::cos(corner_angle) : 2.0f;
  for (int side = 0; side < 2; side++) {
    int v = m.e.vs[e][side];
    if (m.v.e[v] == ELEM_NONE) {
      return false;
    }
    int sameType = 0;
    litestl::math::float3 dir[2];
    for (int ei : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
      int eim = feat.edgeMask(ei);
      if (eim == 0) {
        continue;
      }
      if (eim != em) {
        return false; /* junction / mixed feature types -> corner, don't collapse */
      }
      if (sameType < 2) {
        int ov = m.e.vs[ei][0] == v ? m.e.vs[ei][1] : m.e.vs[ei][0];
        dir[sameType] = m.v.co[ov] - m.v.co[v];
      }
      sameType++;
    }
    if (sameType != 2) {
      return false; /* endpoint is a feature end / corner, not a clean interior */
    }
    if (corner_angle > 0.0f) {
      float l0 = dir[0].length(), l1 = dir[1].length();
      if (l0 > 1e-20f && l1 > 1e-20f && dir[0].dot(dir[1]) > corner_dot * l0 * l1) {
        return false; /* geometric corner: the curve bends here */
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
inline DynTopoStats runDyntopoRemesh(mesh::Mesh &m,
                                     litestl::math::float3 center,
                                     float radius,
                                     const DynTopoParams &p,
                                     uint32_t seed,
                                     mesh::MeshCallbacks *cb = nullptr,
                                     litestl::util::span<const int> seedVerts = {})
{
  using namespace litestl;
  using namespace litestl::util;

  const bool doSplit = p.mode == DynTopoMode::Subdivide || p.mode == DynTopoMode::Both;
  const bool doCollapse = p.mode == DynTopoMode::Collapse || p.mode == DynTopoMode::Both;
  const float r2 = radius * radius;
  /* Band-overlap guard: split children land at exactly l_max/2, so any l_min
   * above that feeds fresh children straight into the collapse band and the dab
   * churns split<->collapse instead of converging. Clamp every caller's band. */
  const float l_min = p.l_min > 0.5f * p.l_max ? 0.5f * p.l_max : p.l_min;

  /* Boundary-overlay views for feature-preserving remeshing (inert when
   * p.preserve_features is false). */
  detail::FeatureViews feat;
  feat.init(m, p.preserve_features);

  /* Resolve the brush's displacement field (non-creating) for the smooth step. */
  detail::DispField dispField;
  if (p.dispGen != 0 && m.v.attrs.has(mesh::AttrType::FLOAT3, ".brush.disp.vec") &&
      m.v.attrs.has(mesh::AttrType::INT, ".brush.disp.gen"))
  {
    dispField.vec = m.v.attrs.find_attribute(mesh::AttrType::FLOAT3, ".brush.disp.vec")
                        .get_data<litestl::math::float3>();
    dispField.gen =
        m.v.attrs.find_attribute(mesh::AttrType::INT, ".brush.disp.gen").get_data<int>();
    dispField.stamp = int(p.dispGen);
  }

  /* Tier 9 adaptive sizing: resolve the optional per-vertex size-scale attr once
   * (non-creating). When set, the candidate band is scaled per edge by the mean
   * of its endpoints' s; new verts get an interpolated s from splitEdge. */
  mesh::AttrData<float> *sizeField = nullptr;
  if (p.size_attr && m.v.attrs.has(mesh::AttrType::FLOAT, p.size_attr)) {
    sizeField =
        m.v.attrs.find_attribute(mesh::AttrType::FLOAT, p.size_attr).get_data<float>();
  }

  /* Split / flip / smooth are triangle-only; dyntopo dynamically triangulates any
   * non-triangle face it encounters in the region first (incl. the graded-target
   * faces — already tris — and any imported/procedural n-gon). Fan-triangulate
   * with the callbacks so spatial/meshlog stay in sync; attrs are carried.
   * Skipped wholesale when the mesh is known all-triangle (n_ngon_faces == 0),
   * the common dyntopo case (dyntopo never creates n-gons). */
  if (m.n_ngon_faces != 0) {
    Set<int> triFaces;
    auto considerFaceTri = [&](int f) {
      if (f < 0 || f >= int(m.f.capacity()) || m.f.freemap[f] || m.f.list_count[f] != 1) {
        return;
      }
      if (m.l.size[m.f.l[f]] == 3) {
        return;
      }
      triFaces.add(f);
    };
    auto considerVertFaces = [&](int v) {
      if (v < 0 || v >= int(m.v.capacity()) || m.v.freemap[v] || m.v.e[v] == ELEM_NONE) {
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

  // The [tmin, tmax] band an edge is judged against, by the same rule as the candidate scan
  // that emits it: the graded seed uses the unscaled band, the sphere scan scales it.
  auto bandFor = [&](int e, float &tmin, float &tmax) {
    tmax = p.l_max;
    tmin = l_min;
    if (p.region == DynTopoRegion::GradedRecursive) {
      return;
    }
    if (sizeField) {
      float s = 0.5f * (sizeField->safe_get(m.e.vs[e][0]) + sizeField->safe_get(m.e.vs[e][1]));
      if (s > 1e-6f) {
        tmax *= s;
        tmin *= s;
      }
    } else if (p.grade > 0.0f && radius > 0.0f) {
      float d = (detail::edgeMid(m, e) - center).length();
      float scale = 1.0f + p.grade * (d / radius);
      tmax *= scale;
      tmin *= scale;
    }
  };

  struct PinchReq {
    int a, b;
    Vector<int, 4> extras;
    float tmax; // split threshold at the refused edge; scales the cull box
  };
  struct CullSeed {
    int face;
    float box;
  };

  /* Frontier of verts whose incident edges might have fallen out of band since
   * last round (the previous round's candidate + created-edge endpoints). Round
   * 0 scans the whole mesh once to seed; later rounds stay local to the brush,
   * so a dab is O(brush region) per round rather than O(total edges) (see the
   * bench_dyntopo profiling finding). */
  Set<int> frontier;
  bool firstRound = true;
  bool budgetHit = false;
  int stallRun = 0; /* consecutive low-progress rounds (limit-cycle early-out) */

  /* Cumulative op counts at the start of the round, for per-round trace deltas
   * (only written when tracing). */
  int traceS0 = 0, traceC0 = 0, traceF0 = 0, traceSm0 = 0;
  Set<int> touched;

  for (int round = 0; round < p.max_rounds; round++) {
    if (p.trace) {
      traceS0 = stats.splits;
      traceC0 = stats.collapses;
      traceF0 = stats.flips;
      traceSm0 = stats.smooths;
    }
    /* Per-round band pressure (trace only): candidate counts + worst band
     * overshoot/undershoot, accumulated as `consider` queues them. */
    int trSplitCands = 0, trCollapseCands = 0;
    float trMaxOver = 0.0f, trMinUnder = 0.0f;
    /* 1. Build candidates: in-region edges outside the [l_min, l_max] band.
     * When a per-dab budget is active, cap collection at 8x the REMAINING
     * budget (slack for MIS lock rejections): a decimation-mode dab otherwise
     * collects the whole region's edges (100k+) — paying the shuffle, MIS
     * walk, and frontier-set inserts for all of them — to then apply only the
     * budgeted few. Capped-out edges are simply next dab's work, like the
     * budget itself. */
    const int splitCandCap = (doSplit && p.max_splits > 0)
                                 ? std::max(64, 8 * (p.max_splits - stats.splits))
                                 : 0;
    const int collapseCandCap =
        (doCollapse && p.max_collapses > 0)
            ? std::max(64, 8 * (p.max_collapses - stats.collapses))
            : 0;
    int splitCands = 0, collapseCands = 0;
    Vector<Cand> cands;
    Vector<Cand> picked;
    detail::GenSet &seen = detail::scanSeenSet();
    seen.reset(int(m.e.capacity()));
    // Queue `e` as a split; the caller has already length-tested it against
    // `tmax`. Both region modes append through here, so the budget cap and the
    // band-pressure trace stay in one place.
    auto emitSplit = [&](int e, float L, float tmax) {
      if (splitCandCap > 0 && splitCands >= splitCandCap) {
        return;
      }
      cands.append({e, true}); /* split always allowed; flags propagate */
      splitCands++;
      if (p.trace) {
        trSplitCands++;
        float over = L / tmax;
        if (over > trMaxOver) {
          trMaxOver = over;
        }
      }
    };
    // The collapse counterpart. The feature gate lives here, so unlike splits
    // this can still refuse the edge, and it reports whether it queued one.
    auto emitCollapse = [&](int e, float L, float tmin) {
      if (collapseCandCap > 0 && collapseCands >= collapseCandCap) {
        return false;
      }
      /* Feature preservation (Decision B): pin feature verts, but allow a
       * feature edge to collapse along its own collinear curve. */
      if (feat.active) {
        bool fv0 = feat.isFeatureVert(m.e.vs[e][0]);
        bool fv1 = feat.isFeatureVert(m.e.vs[e][1]);
        if (fv0 || fv1) {
          if (feat.isFeatureEdge(e)) {
            if (!detail::featureCollapseOk(m, e, feat, p.feature_corner_angle)) {
              return false; /* corner / junction / mixed curve: don't collapse */
            }
          } else {
            return false; /* non-feature edge touching a feature vert: would tear */
          }
        }
      }
      cands.append({e, false});
      collapseCands++;
      if (p.trace) {
        trCollapseCands++;
        float under = tmin > 1e-20f ? L / tmin : 0.0f;
        if (trCollapseCands == 1 || under < trMinUnder) {
          trMinUnder = under;
        }
      }
      return true;
    };
    auto consider = [&](int e) {
      if (m.e.freemap[e] || m.e.c[e] == ELEM_NONE || !seen.add(e)) {
        return; /* freed, wire, or already considered this round */
      }
      float d2 = (detail::edgeMid(m, e) - center).lengthSqr();
      if (d2 > r2) {
        return; /* outside the dab */
      }
      /* Graded target: relax the goal per edge (sizing field). The per-vertex
       * size-scale attr (Tier 9) takes precedence over the radial `grade`. */
      float tmax = p.l_max, tmin = l_min;
      if (sizeField) {
        float s = 0.5f *
                  (sizeField->safe_get(m.e.vs[e][0]) + sizeField->safe_get(m.e.vs[e][1]));
        if (s > 1e-6f) {
          tmax *= s;
          tmin *= s;
        }
      } else if (p.grade > 0.0f && radius > 0.0f) {
        float scale = 1.0f + p.grade * (std::sqrt(d2) / radius);
        tmax *= scale;
        tmin *= scale;
      }
      float L = detail::edgeLen(m, e);
      if (doSplit && L > tmax) {
        emitSplit(e, L, tmax);
      } else if (doCollapse && L < tmin) {
        emitCollapse(e, L, tmin);
      }
    };
    auto considerVertEdges = [&](int v) {
      if (v < 0 || v >= int(m.v.capacity()) || m.v.freemap[v] || m.v.e[v] == ELEM_NONE) {
        return;
      }
      for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
        consider(e);
      }
    };

    /* DynTopoRegion::GradedRecursive has three pieces, mirroring Blender's
     `long_edge_queue_face_add`, `long_edge_queue_edge_add_recursive` and its
     post-split valence relief in turn:

       1. seed on faces reaching into the dab, taking all three edges at the
          uniform band. That seed is also the whole of the collapse rule, since
          Blender grades subdivision only and `short_edge_queue_face_add` does
          not recurse;
       2. walk outward from each over-length seed edge, multiplying the goal
          length by `graded_generation_scale` per hop, so that refinement decays
          into the surrounding mesh instead of cliffing at the rim;
       3. offer every edge of an over-valence vert at the unscaled l_max.

     `seen` means "already queued" in this mode rather than "already examined".
     The walk can reach an edge at several depths and has to stay free to re-test
     it when a shallower path turns up, so it is the `gradedLimits` memo that
     bounds the walk. The two modes never run together and share the buffer. */
    const bool graded = p.region == DynTopoRegion::GradedRecursive;
    detail::GenMinMap &gradedLimits = detail::gradedLimitMap();
    detail::GenSet &regionVerts = detail::gradedRegionVertSet();
    if (graded) {
      detail::GenSet &fseen = detail::gradedFaceSeenSet();
      gradedLimits.reset(int(m.e.capacity()));
      regionVerts.reset(int(m.v.capacity()));
      fseen.reset(int(m.f.capacity()));

      struct Hop {
        int edge;
        float limit;
        int hop;
      };
      Vector<Hop, 64> stack;
      const float genScale = p.graded_generation_scale > 1.0f ? p.graded_generation_scale
                                                             : 1.0f + 1e-3f;
      const int maxHops = p.graded_max_hops > 0 ? p.graded_max_hops : 0;

      auto push = [&](int e, float limit, int hop) {
        if (hop > maxHops || m.e.freemap[e] || m.e.c[e] == ELEM_NONE) {
          return;
        }
        if (!gradedLimits.improve(e, limit)) {
          return; // already reachable at least this permissively
        }
        stack.append({e, limit, hop});
      };

      // 1. Seed: every face reaching into the dab.
      auto seedFace = [&](int f) {
        int vs[3];
        if (!fseen.add(f) || !detail::triInSphere(m, f, center, r2, vs)) {
          return;
        }
        for (int i = 0; i < 3; i++) {
          regionVerts.add(vs[i]);
        }
        int li = m.f.l[f], c0 = m.l.c[li], cc = c0;
        do {
          int e = m.c.e[cc];
          cc = m.c.next[cc];
          if (m.e.freemap[e] || m.e.c[e] == ELEM_NONE) {
            continue;
          }
          float L = detail::edgeLen(m, e);
          if (doSplit && L > p.l_max) {
            if (seen.add(e)) {
              emitSplit(e, L, p.l_max);
            }
            push(e, p.l_max, 0); // and grade outward from it
          } else if (doCollapse && L < l_min && seen.add(e)) {
            emitCollapse(e, L, l_min);
          }
        } while (cc != c0);
      };
      auto seedVertFaces = [&](int v) {
        if (v < 0 || v >= int(m.v.capacity()) || m.v.freemap[v] || m.v.e[v] == ELEM_NONE)
        {
          return;
        }
        for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
          int rc0 = m.e.c[e];
          if (rc0 == ELEM_NONE) {
            continue;
          }
          int rcc = rc0;
          do {
            seedFace(m.l.f[m.c.l[rcc]]);
            rcc = m.c.radial_next[rcc];
          } while (rcc != rc0);
        }
      };
      /* 3. Valence relief. Blender fires this on the apex of a face it has just
       split, so the hub is queued geometry by construction; the round-loop
       equivalent is a vert the seed or the walk reached, hence the regionVerts
       gate. Without it the relief is the one rule here with no distance bound at
       all, and re-running it on the whole frontier every round walks the
       refinement steadily off across the mesh. */
      auto valenceRelief = [&](int v) {
        if (!doSplit || p.graded_valence_relief <= 0 || !regionVerts.contains(v) ||
            !detail::vertValenceOver(m, v, p.graded_valence_relief))
        {
          return;
        }
        for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
          if (m.e.freemap[e] || m.e.c[e] == ELEM_NONE) {
            continue;
          }
          float L = detail::edgeLen(m, e);
          if (L > p.l_max && seen.add(e)) {
            emitSplit(e, L, p.l_max);
          }
        }
      };

      const bool fullScan = firstRound && seedVerts.size() == 0;
      auto forEachSeedVert = [&](auto &&fn) {
        if (fullScan) {
          for (int v : m.v) {
            fn(v);
          }
        } else if (firstRound) {
          for (int v : seedVerts) {
            fn(v);
          }
        } else {
          for (int v : frontier) {
            fn(v);
          }
        }
      };
      if (fullScan) {
        for (int f : m.f) {
          seedFace(f); /* round 0, unseeded: one full-mesh scan */
        }
      } else {
        forEachSeedVert(seedVertFaces);
      }

      // 2. The graded walk. Lengths are compared squared throughout, so that
      // `graded_len_sq_factor` carries Blender's constant verbatim.
      while (!stack.isEmpty()) {
        Hop h = stack.pop_back();
        if (m.e.freemap[h.edge] || m.e.c[h.edge] == ELEM_NONE) {
          continue;
        }
        math::float3 ev0 = m.v.co[m.e.vs[h.edge][0]], ev1 = m.v.co[m.e.vs[h.edge][1]];
        float lenSq = (ev0 - ev1).lengthSqr();
        if (lenSq <= h.limit * h.limit) {
          continue;
        }
        if (h.hop > stats.graded_hops) {
          stats.graded_hops = h.hop;
        }
        if (doSplit && seen.add(h.edge)) {
          emitSplit(h.edge, std::sqrt(lenSq), h.limit);
        }
        const float newLimit = h.limit * genScale;
        const float gateSq = std::max(lenSq * p.graded_len_sq_factor, newLimit * newLimit);
        int rc0 = m.e.c[h.edge], rcc = rc0;
        do {
          int li = m.c.l[rcc];
          if (m.l.size[li] == 3) {
            // The face's other two edges, Blender's `l_iter->next/prev`.
            int adj[2] = {m.c.e[m.c.next[rcc]], m.c.e[m.c.prev[rcc]]};
            regionVerts.add(m.c.v[rcc]);
            regionVerts.add(m.c.v[m.c.next[rcc]]);
            regionVerts.add(m.c.v[m.c.prev[rcc]]);
            for (int i = 0; i < 2; i++) {
              int e2 = adj[i];
              if (m.e.freemap[e2]) {
                continue;
              }
              math::float3 a = m.v.co[m.e.vs[e2][0]], b = m.v.co[m.e.vs[e2][1]];
              if ((a - b).lengthSqr() > gateSq) {
                push(e2, newLimit, h.hop + 1);
              }
            }
          }
          rcc = m.c.radial_next[rcc];
        } while (rcc != rc0);
      }

      // The relief runs last so regionVerts is complete when it reads it.
      if (doSplit && p.graded_valence_relief > 0) {
        forEachSeedVert(valenceRelief);
      }
      firstRound = false;
    } else if (firstRound) {
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
    detail::GenSet &locked = detail::misLockedSet();
    picked.clear();
    locked.reset(int(m.v.capacity()));
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
    // Pinch-pass ops count as progress but must not reset the stall run, or a large piece that
    // keeps being cut and never culled would spin to max_rounds.
    int appliedPinchOps = 0;
    Vector<PinchReq> pinchReqs;
    Vector<CullSeed> cullSeeds;
    touched.clear();

    /* Verts whose 1-ring the flip sweep must re-examine: only the geometry an
     * applied split/collapse actually created — NOT every unpicked candidate
     * endpoint (those didn't change this round). Driving the flip sweep from
     * this set instead of the whole frontier cuts flip-candidate collection
     * ~4-5x (the dominant per-dab phase). */
    auto addCreated = [&](const auto &edges) {
      for (int e : edges) {
        if (!m.e.freemap[e]) {
          nextFrontier.add(m.e.vs[e][0]);
          nextFrontier.add(m.e.vs[e][1]);
          touched.add(m.e.vs[e][0]);
          touched.add(m.e.vs[e][1]);
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
        /* No base shift here: collapseEdge already merges `.brush.disp.vec` over
         * both endpoints (blend=0.5), so the derived base `co - disp` lands on
         * the stroke-start surface's own midpoint. Shifting again by the
         * survivor's motion would add half the edge vector on top of it. */
        mesh::EdgeCollapseResult res;
        if (mesh::collapseEdge(m,
                               c.edge,
                               mid,
                               /*blend=*/0.5f,
                               &res,
                               cb,
                               /*prevent_inversion=*/true))
        {
          stats.collapses++;
          applied++;
          addCreated(res.created_edges);
          if (p.max_collapses > 0 && stats.collapses >= p.max_collapses) {
            budgetHit = true;
            break; /* stop applying; flip sweep below still runs on what we did */
          }
        } else if (p.pinch_thin) {
          // The refused collapse locked both one-rings, so these vertices survive the round.
          float tmin, tmax;
          bandFor(c.edge, tmin, tmax);
          if (res.refusal == mesh::CollapseRefusal::Link && !res.link_extra.isEmpty()) {
            pinchReqs.append({m.e.vs[c.edge][0], m.e.vs[c.edge][1], res.link_extra, tmax});
          } else if (res.refusal == mesh::CollapseRefusal::Tet) {
            // Proven an isolated tetrahedron; hand it to the cull.
            cullSeeds.append({m.l.f[m.c.l[m.e.c[c.edge]]], p.cull_size * tmax});
          }
        }
      }
    }

    // Pinch pass: cut the thin rings refused above, then delete the small closed pieces left.
    // It runs after the independent set is spent, because a cut rewires faces around the ring's
    // third vertex, whose neighbours no lock covers.
    if (p.pinch_thin && (!pinchReqs.isEmpty() || !cullSeeds.isEmpty())) {
      Map<int, int> copyOf; // ring vertex -> its copy from a cut earlier in this pass
      Set<int64_t> doneRings;
      auto edgeBetween = [&](int v0, int v1) { return detail::edgeBetween(m, v0, v1); };
      auto isFace = [&](int v0, int v1, int v2) {
        int e = edgeBetween(v0, v1);
        if (e == ELEM_NONE || m.e.c[e] == ELEM_NONE) {
          return false;
        }
        int c0 = m.e.c[e], cc = c0;
        do {
          if (m.l.size[m.c.l[cc]] == 3 && m.c.v[m.c.next[m.c.next[cc]]] == v2) {
            return true;
          }
          cc = m.c.radial_next[cc];
        } while (cc != c0);
        return false;
      };
      auto live = [&](int v) { return v >= 0 && v < int(m.v.capacity()) && !m.v.freemap[v]; };
      // Finds a live ring on the original vertices or their copies: three edges, not a face.
      auto resolveRing = [&](int r0, int r1, int r2, int out3[3]) {
        int opts[3][2] = {{r0, r0}, {r1, r1}, {r2, r2}};
        const int *orig[3] = {&r0, &r1, &r2};
        for (int i = 0; i < 3; i++) {
          if (copyOf.contains(*orig[i])) {
            opts[i][1] = copyOf.lookup(*orig[i]);
          }
        }
        for (int k = 0; k < 8; k++) {
          int v0 = opts[0][k & 1], v1 = opts[1][(k >> 1) & 1], v2 = opts[2][(k >> 2) & 1];
          if (!live(v0) || !live(v1) || !live(v2)) {
            continue;
          }
          if (edgeBetween(v0, v1) == ELEM_NONE || edgeBetween(v1, v2) == ELEM_NONE ||
              edgeBetween(v2, v0) == ELEM_NONE || isFace(v0, v1, v2))
          {
            continue;
          }
          out3[0] = v0, out3[1] = v1, out3[2] = v2;
          return true;
        }
        return false;
      };
      // A single valence-3 vertex bounded by the ring, if either side is one.
      auto trivialSide = [&](const int r[3]) {
        int e = edgeBetween(r[0], r[1]);
        int c0 = m.e.c[e], cc = c0;
        do {
          int v = m.c.v[m.c.next[m.c.next[cc]]];
          // Adjacent to all three ring vertices with nothing else: valence exactly 3.
          if (!detail::vertValenceOver(m, v, 3) && edgeBetween(v, r[2]) != ELEM_NONE) {
            return v;
          }
          cc = m.c.radial_next[cc];
        } while (cc != c0);
        return int(ELEM_NONE);
      };

      for (const PinchReq &rq : pinchReqs) {
        if (p.max_pinches > 0 && stats.pinches >= p.max_pinches) {
          break;
        }
        for (int extra : rq.extras) {
          int r[3];
          if (!resolveRing(rq.a, rq.b, extra, r)) {
            continue;
          }
          int s[3] = {r[0], r[1], r[2]};
          std::sort(s, s + 3);
          if (!doneRings.add((int64_t(s[0]) << 42) ^ (int64_t(s[1]) << 21) ^ int64_t(s[2]))) {
            continue;
          }
          bool thin = true;
          for (int i = 0; i < 3 && thin; i++) {
            int e = edgeBetween(r[i], r[(i + 1) % 3]);
            float tmin, tmax;
            bandFor(e, tmin, tmax);
            thin = detail::edgeLen(m, e) < p.pinch_ring_factor * tmin;
          }
          if (!thin) {
            continue;
          }
          if (feat.active && (feat.isFeatureVert(r[0]) || feat.isFeatureVert(r[1]) ||
                              feat.isFeatureVert(r[2])))
          {
            continue;
          }

          int v = trivialSide(r);
          if (v != ELEM_NONE && feat.isFeatureVert(v)) {
            continue;
          }
          if (v != ELEM_NONE) {
            // Merge the lone vertex into the nearest ring vertex rather than cutting it off; a
            // spoke longer than the split threshold is real shape and stays.
            int best = ELEM_NONE;
            float bestLen = 0.0f;
            for (int i = 0; i < 3; i++) {
              int e = edgeBetween(v, r[i]);
              float L = detail::edgeLen(m, e);
              if (best == ELEM_NONE || L < bestLen) {
                best = e;
                bestLen = L;
              }
            }
            float tmin, tmax;
            bandFor(best, tmin, tmax);
            if (bestLen >= tmax) {
              continue;
            }
            mesh::EdgeCollapseResult cres;
            if (mesh::collapseEdge(
                    m, best, detail::edgeMid(m, best), 0.5f, &cres, cb, /*prevent_inversion=*/true))
            {
              stats.trivial_dissolves++;
              appliedPinchOps++;
              addCreated(cres.created_edges);
              break;
            }
            // The merge would fold a face. Cutting instead removes the vertex without moving
            // anything, and its short spokes keep the cut-off tetrahedron inside the cull limits.
          }

          mesh::PinchResult pres;
          if (!mesh::pinchSeparatingTriangle(m, r[0], r[1], r[2], &pres, cb)) {
            continue;
          }
          stats.pinches++;
          appliedPinchOps++;
          for (int i = 0; i < 3; i++) {
            copyOf.add_overwrite(r[i], pres.copies[i]);
          }
          addCreated(pres.created_edges);
          for (int i = 0; i < 3; i++) {
            touched.add(r[i]);
            touched.add(pres.copies[i]);
            nextFrontier.add(r[i]);
            nextFrontier.add(pres.copies[i]);
          }
          cullSeeds.append({pres.cap_l, p.cull_size * rq.tmax});
          cullSeeds.append({pres.cap_r, p.cull_size * rq.tmax});
          break;
        }
      }

      // Cull: flood each seed's piece; delete it when it closes within the face and size limits.
      for (const CullSeed &cs : cullSeeds) {
        if (cs.face < 0 || cs.face >= int(m.f.capacity()) || m.f.freemap[cs.face]) {
          continue; // an earlier flood already deleted it
        }
        Vector<int, 64> faces;
        Set<int, 64> seenF;
        Vector<int, 64> stack;
        seenF.add(cs.face);
        stack.append(cs.face);
        math::float3 bmin(std::numeric_limits<float>::max());
        math::float3 bmax(-std::numeric_limits<float>::max());
        bool keep = false;
        while (!stack.isEmpty() && !keep) {
          int f = stack.pop_back();
          faces.append(f);
          if (int(faces.size()) > p.cull_max_faces) {
            keep = true;
            break;
          }
          int c0 = m.l.c[m.f.l[f]], cc = c0;
          do {
            bmin.min(m.v.co[m.c.v[cc]]);
            bmax.max(m.v.co[m.c.v[cc]]);
            int rn = m.c.radial_next[cc];
            if (rn == cc || m.c.radial_next[rn] != cc) {
              keep = true; // boundary or non-manifold edge: not a closed piece
              break;
            }
            int g = m.l.f[m.c.l[rn]];
            if (seenF.add(g)) {
              stack.append(g);
            }
            cc = m.c.next[cc];
          } while (cc != c0);
          if ((bmax - bmin).length() > cs.box) {
            keep = true;
          }
        }
        if (keep) {
          continue;
        }
        Set<int, 64> edges, verts;
        for (int f : faces) {
          int c0 = m.l.c[m.f.l[f]], cc = c0;
          do {
            edges.add(m.c.e[cc]);
            verts.add(m.c.v[cc]);
            cc = m.c.next[cc];
          } while (cc != c0);
        }
        for (int f : faces) {
          m.kill_face(f, cb);
        }
        for (int e : edges) {
          if (!m.e.freemap[e] && m.e.c[e] == ELEM_NONE) {
            m.kill_edge(e, cb);
          }
        }
        for (int v : verts) {
          if (!m.v.freemap[v] && m.v.e[v] == ELEM_NONE) {
            m.kill_vertex(v, cb);
          }
        }
        stats.culled_faces += int(faces.size());
        appliedPinchOps++;
      }
      applied += appliedPinchOps;
    }

    /* Under GradedRecursive the outward splits land outside the dab sphere, so
     the flip and smooth gates below follow the walk's region instead. Left on
     the sphere they would refuse to clean up the geometry the grading had just
     created, which is the cascade the flip sweep exists to break. The verts an
     applied edit touched join the region for the same reason, a spoke created
     this round having been unreachable when the walk ran. */
    if (graded) {
      for (int v : touched) {
        regionVerts.add(v);
      }
    }
    auto inRegionVert = [&](int v) {
      return graded ? regionVerts.contains(v) : (m.v.co[v] - center).lengthSqr() <= r2;
    };
    auto inRegionEdge = [&](int e) {
      return graded ? (regionVerts.contains(m.e.vs[e][0]) || regionVerts.contains(m.e.vs[e][1]))
                    : (detail::edgeMid(m, e) - center).lengthSqr() <= r2;
    };

    /* 5. Geometric flip sweep (M7.2): shorten the long spokes this round's
     *    splits just created, before they cascade into more splits. Collect the
     *    in-region interior edges around the touched verts first (read-only),
     *    then apply — flipping mutates disks, so we never flip while walking
     *    one. Each helper re-validates, so a flip invalidating a later candidate
     *    is safe. Flipped apexes re-enter the frontier (their lengths changed). */
    if (p.do_flips) {
      Vector<int, 32> flipCands;
      detail::GenSet &eseen = detail::flipSeenSet();
      eseen.reset(int(m.e.capacity()));
      for (int v : touched) {
        if (v < 0 || v >= int(m.v.capacity()) || m.v.freemap[v] || m.v.e[v] == ELEM_NONE)
        {
          continue;
        }
        for (int e : mesh::EdgeOfVertIter(&m, v, m.v.e[v])) {
          if (m.e.freemap[e] || !eseen.add(e)) {
            continue;
          }
          if (!inRegionEdge(e)) {
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
            !detail::flipShortens(m, a, b, cc, dd))
        {
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
     *    order-independent and deterministic. Position-only (the region's leaves
     *    are already bounds-dirty from the splits), but it still fires
     *    cb->onVertChange before moving each vert so the meshlog captures the
     *    pre-smooth position for undo — otherwise a vert moved only by smoothing
     *    is never recorded and undo leaves it displaced. */
    if (p.do_smooth) {
      Vector<int, 32> sverts;
      Vector<math::float3, 32> spos;
      Vector<math::float3, 32> sold;
      Vector<math::float3, 32> sdisp;
      const detail::DispField *df = dispField ? &dispField : nullptr;
      for (int v : nextFrontier) {
        if (v < 0 || v >= int(m.v.capacity()) || m.v.freemap[v]) {
          continue;
        }
        if (!inRegionVert(v)) {
          continue;
        }
        if (feat.isFeatureVert(v)) {
          continue; /* pin feature verts on their curve (v1: no tangent slide) */
        }
        math::float3 np, ndisp;
        if (detail::smoothTangent(m, v, p.smooth_lambda, np, df, &ndisp)) {
          sverts.append(v);
          spos.append(np);
          if (df) {
            sdisp.append(ndisp);
          }
          if (p.reproject_uvs) {
            sold.append(m.v.co[v]);
          }
        }
      }
      for (int i = 0; i < int(sverts.size()); i++) {
        if (cb && cb->onVertChange) {
          cb->onVertChange(sverts[i]); /* capture pre-smooth position for undo */
        }
        m.v.co[sverts[i]] = spos[i];
        if (df) {
          /* Jacobi like the positions: every target was read before any write. */
          dispField.vec->materialize(sverts[i]);
          dispField.gen->materialize(sverts[i]);
          (*dispField.vec)[sverts[i]] = sdisp[i];
          (*dispField.gen)[sverts[i]] = dispField.stamp;
        }
      }
      if (p.reproject_uvs && sverts.size() > 0) {
        /* Re-anchor the slid verts' UVs on their pre-smooth ring (Jacobi:
         * every moved vert's old position rides `sold`, so neighbors read
         * pre-pass geometry). */
        mesh::uvproj::reprojectVertUVs(
            &m,
            std::span<const int>(sverts.data(), sverts.size()),
            std::span<const math::float3>(sold.data(), sold.size()),
            cb);
      }
      stats.smooths += int(sverts.size());
    }

    frontier = std::move(nextFrontier);

    stats.rounds = round + 1;

    /* Granular split-sliver detection: snapshot this round's frontier-face
     * quality so an oscillation that a final survey would miss is visible. */
    if (p.trace) {
      RoundQuality q;
      q.round = round;
      q.splits = stats.splits - traceS0;
      q.collapses = stats.collapses - traceC0;
      q.flips = stats.flips - traceF0;
      q.smooths = stats.smooths - traceSm0;
      q.split_cands = trSplitCands;
      q.collapse_cands = trCollapseCands;
      q.max_over = trMaxOver;
      q.min_under = trMinUnder;
      detail::measureRoundQuality(m, frontier, center, r2, p.trace->thin_angle, q);
      p.trace->rounds.append(q);
    }

    if (budgetHit) {
      stats.budget_hit = true;
      stats.capped = true;
      break; /* hit the per-dab split budget — rest is the next dab's work */
    }
    if (applied == 0) {
      break; /* nothing progressed (all refused) — avoid spinning */
    }
    /* Limit-cycle early-out: a long run of low-progress rounds is a split<->
     * collapse ping-pong, not convergence (convergence empties cands and breaks
     * above), so bail before the dab spins to max_rounds. See max_stall_rounds. */
    if (p.max_stall_rounds > 0) {
      constexpr int kStallOpMax = 2; /* matches dyntopo_trace's churn_op_max */
      stallRun = applied - appliedPinchOps <= kStallOpMax ? stallRun + 1 : 0;
      if (stallRun >= p.max_stall_rounds) {
        stats.stalled = true;
        stats.capped = true;
        break;
      }
    }
    if (round == p.max_rounds - 1) {
      stats.capped = true;
    }
  }

  /* Topology near features changed: the split-propagated source flags need the
   * derived poly-group / UV-chart flags + per-vert class recomputed. Flag it; the
   * caller folds it in (recomputeDirty) at the next dab / stroke end while links
   * are live. */
  if (feat.active && (stats.splits + stats.collapses + stats.pinches + stats.trivial_dissolves +
                      stats.culled_faces) > 0)
  {
    m.boundaryDirty = true;
  }

  return stats;
}

} // namespace sculptcore::dyntopo
