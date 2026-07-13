#include "remesh/extract/reproject.h"
#include "remesh/preremesh.h" // kPreRemeshSrcFaceAttr

#include "mesh/attribute_builtin.h"
#include "mesh/utils/closest_point.h"
#include "mesh/utils/surface_walk.h"
#include "spatial/spatial.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

namespace sculptcore::remesh {

using litestl::math::float3;
using litestl::util::Vector;
using sculptcore::mesh::ClosestPointResult;
using sculptcore::mesh::findClosestPoint;
using sculptcore::mesh::Mesh;
using sculptcore::mesh::SurfaceWalkResult;
using sculptcore::mesh::walkClosestPoint;

ReprojectStats reprojectToSurface(Mesh &out, Mesh &input, const ReprojectParams &params)
{
  ReprojectStats stats;
  out.thawTopo();
  if (out.v.count == 0) return stats;

  spatial::SpatialTree tree(&input);
  tree.buildAll();

  const bool smoothing = params.smooth_iterations > 0 && params.smooth_lambda > 0.0f;

  int cap = int(out.v.capacity());

  /* 9g transported anchors: out-verts first locate themselves on the work
   * surface (whose verts carry src-face anchors into `input` from the
   * pre-pass), then every snap is a local walk on `input` from the transported
   * seed — sheet-correct by construction, no global query. Failures fall back
   * to the filtered global path below and re-seed from its result. */
  Mesh *work = params.anchor_work;
  bool anchored = false;
  mesh::BuiltinAttr<int, ".remesh.v.src_face"> wanchor;
  spatial::SpatialTree *wtree = nullptr;
  Vector<int> vAnchor; // per-out-vert current anchor face on `input`
  if (work && work->v.attrs.has(mesh::AttrType::INT,
                                litestl::util::string(kPreRemeshSrcFaceAttr))) {
    wanchor.ensure(work->v.attrs);
    wtree = litestl::alloc::New<spatial::SpatialTree>("Reproject work tree", work);
    wtree->buildAll();
    vAnchor.resize(cap);
    for (int i = 0; i < cap; i++) {
      vAnchor[i] = -1;
    }
    anchored = true;
  }
  auto anchorAlive = [&](int f) {
    return f >= 0 && f < int(input.f.capacity()) && !input.f.freemap[f];
  };
  Vector<Vector<int>> nbrs;  // output-mesh 1-ring per vertex (smoothing only)
  Vector<char> isBndV;       // boundary-loop vertex flag (pinned while smoothing)
  Vector<float3> np;         // Jacobi scratch for a smoothing pass
  if (smoothing) {
    nbrs.resize(cap);
    isBndV.resize(cap);
    np.resize(cap);
    for (int i = 0; i < cap; i++) isBndV[i] = 0;
    if (params.pin_boundary) {
      auto isBndE = [&](int e) {
        int c0 = out.e.c[e];
        if (c0 == ELEM_NONE) return true;
        int r = 0, cc = c0;
        do { r++; cc = out.c.radial_next[cc]; } while (cc != c0 && r < 100);
        return r == 1;
      };
      for (int e : out.e)
        if (isBndE(e)) { isBndV[out.e.vs[e][0]] = 1; isBndV[out.e.vs[e][1]] = 1; }
    }
    for (int e : out.e) {
      nbrs[out.e.vs[e][0]].append(out.e.vs[e][1]);
      nbrs[out.e.vs[e][1]].append(out.e.vs[e][0]);
    }
  }

  /* The output's winding may be globally flipped relative to the input (quad
   * extraction does not inherit orientation), which would aim the sheet filter
   * below at the far surface. Calibrate the filter sign once by voting normal
   * agreement at unfiltered closest points over a vertex sample. */
  const bool filtered = params.sheet_min_dot > -1.0f;
  out.recalc_normals();
  float orient = 1.0f;
  if (filtered) {
    double vote = 0.0;
    int step = out.v.count > 256 ? out.v.count / 256 : 1, k = 0;
    for (int v : out.v) {
      if (k++ % step) continue;
      ClosestPointResult r = findClosestPoint(tree, out.v.co[v]);
      if (!r.hit) continue;
      float3 a = input.v.co[input.c.v[r.tri_c[0]]];
      float3 b = input.v.co[input.c.v[r.tri_c[1]]];
      float3 c = input.v.co[input.c.v[r.tri_c[2]]];
      vote += out.v.no[v].dot((b - a).cross(c - a)) >= 0.0f ? 1.0 : -1.0;
    }
    if (vote < 0.0) orient = -1.0f;
  }

  /* Normal-coherence of v's 1-ring (|mean of unit face normals|): ~1 where the
   * umbrella is flat/clean, small where it folds. The sheet filter only engages
   * above kCoherence — a folded umbrella's vertex normal is the likelier liar,
   * and redirecting its snap manufactures the very fold-backs the filter is
   * meant to prevent. */
  auto vertCoherence = [&](int v) -> float {
    int e0 = out.v.e[v];
    if (e0 == ELEM_NONE) return 1.0f;
    float3 sum(0.0f, 0.0f, 0.0f);
    int cnt = 0;
    for (int ec : mesh::EdgeOfVertIter(&out, v, e0)) {
      int ci = out.e.c[ec];
      if (ci != ELEM_NONE) {
        sum += out.f.no[out.l.f[out.c.l[ci]]];
        cnt++;
      }
    }
    return cnt ? sum.length() / float(cnt) : 1.0f;
  };
  constexpr float kCoherence = 0.7f;

  int iters = params.iterations < 1 ? 1 : params.iterations;
  for (int it = 0; it < iters; it++) {
    if (smoothing) {
      for (int s = 0; s < params.smooth_iterations; s++) {
        for (int v : out.v) np[v] = out.v.co[v];
        for (int v : out.v) {
          if (isBndV[v] || nbrs[v].size() == 0) continue;
          float3 avg(0, 0, 0);
          for (int w : nbrs[v]) avg = avg + out.v.co[w];
          avg = avg * (1.0f / float(nbrs[v].size()));
          float3 cur = out.v.co[v];
          np[v] = cur + (avg - cur) * params.smooth_lambda;
        }
        for (int v : out.v) out.v.co[v] = np[v];
      }
    }

    bool last = it == iters - 1;
    double sum = 0.0;
    float mx = 0.0f;
    int n = 0;
    // Normal-compatible snap: when the plain closest point lies on an opposite
    // sheet (thin feature — snapping there folds the output back on itself),
    // re-query with the sheet filter. The filtered hit is only trusted while it
    // stays within kSheetRatio of the plain one; past that the vertex's own
    // normal is the likelier liar (extraction fold near a singularity), and the
    // plain snap irons such folds flat like it always did.
    constexpr float kSheetRatio = 3.0f;
    out.recalc_normals();
    for (int v : out.v) {
      if (anchored) {
        int seed = vAnchor[v];
        if (!anchorAlive(seed)) {
          // First snap (or post-fallback re-seed): transport the max-bary
          // corner's anchor from the closest work triangle.
          ClosestPointResult rw = findClosestPoint(*wtree, out.v.co[v]);
          if (rw.hit) {
            float best_b = -1.0f;
            for (int i = 0; i < 3; i++) {
              int wa = wanchor[work->c.v[rw.tri_c[i]]];
              if (anchorAlive(wa) && rw.bary[i] > best_b) {
                best_b = rw.bary[i];
                seed = wa;
              }
            }
          }
        }
        if (anchorAlive(seed)) {
          SurfaceWalkResult w = walkClosestPoint(input, seed, out.v.co[v]);
          if (w.hit && w.converged) {
            vAnchor[v] = w.face;
            if (last) {
              sum += w.dist;
              if (w.dist > mx) mx = w.dist;
              n++;
            }
            out.v.co[v] = w.point;
            continue;
          }
        }
        vAnchor[v] = -1; // walk failed — global path below re-seeds
      }
      ClosestPointResult r = findClosestPoint(tree, out.v.co[v]);
      if (!r.hit) continue;
      if (filtered) {
        float3 vn = out.v.no[v] * orient;
        float3 a = input.v.co[input.c.v[r.tri_c[0]]];
        float3 b = input.v.co[input.c.v[r.tri_c[1]]];
        float3 c = input.v.co[input.c.v[r.tri_c[2]]];
        float3 tn = (b - a).cross(c - a);
        if (tn.dot(vn) < params.sheet_min_dot * tn.length() &&
            vertCoherence(v) > kCoherence) {
          ClosestPointResult rf =
              findClosestPoint(tree, out.v.co[v], &vn, params.sheet_min_dot);
          if (rf.hit && rf.dist <= kSheetRatio * r.dist) {
            r = rf;
          }
        }
      }
      if (last) {
        sum += r.dist;
        if (r.dist > mx) mx = r.dist;
        n++;
      }
      if (anchored) {
        vAnchor[v] = r.face; // next iteration walks from the global result
      }
      out.v.co[v] = r.point;
    }
    if (last) {
      stats.num_verts = n;
      stats.max_dist = mx;
      stats.mean_dist = n ? float(sum / n) : 0.0f;
    }
  }

  if (wtree) {
    litestl::alloc::Delete<spatial::SpatialTree>(wtree);
  }
  out.recalc_normals();
  return stats;
}

} // namespace sculptcore::remesh
