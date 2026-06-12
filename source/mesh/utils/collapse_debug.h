#pragma once

/* Compile-time-gated edge-collapse self-check + neighborhood capture, modeled on
 * Blender's JVKE_DEBUG (bmesh_collapse.cc: bm_save_local_obj_text /
 * JVKE_CHECK_ELEMENT). When SCULPTCORE_COLLAPSE_DEBUG is set, collapseEdge()
 * snapshots the 2-ring patch around the edge BEFORE the op, then AFTER scans the
 * survivor's neighborhood for defects a collapse must never introduce — a
 * non-manifold edge (radial face count != 2) or a hole/open boundary opening in
 * a region that was closed. On a defect it writes the captured BEFORE patch as
 * an OBJ with the collapsed edge's two endpoints marked, so the failing collapse
 * can be replayed in isolation (see tests/test_collapse_repro.cc).
 *
 * This is PERMANENT debug scaffolding: keep it #if-guarded, do not strip it.
 * Enable with `-DSCULPTCORE_COLLAPSE_DEBUG=ON` at configure (CMake option) and
 * point the dumps at a dir with $SCULPTCORE_COLLAPSE_DEBUG_DIR (default cwd). */

#ifndef SCULPTCORE_COLLAPSE_DEBUG
#define SCULPTCORE_COLLAPSE_DEBUG 0
#endif

#if SCULPTCORE_COLLAPSE_DEBUG

#include "../mesh.h"
#include "../mesh_iter.h"

#include "litestl/math/vector.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sculptcore::mesh::collapse_debug {

using litestl::math::float3;
using litestl::util::Set;
using litestl::util::Vector;

/* Unnormalized Newell normal of a polygon (length ~ 2*area); robust for
 * non-planar polys and degeneracy-safe (returns ~0 vector for slivers). */
static inline float3 newellNormal(const float3 *p, int n)
{
  float3 nrm(0.0f, 0.0f, 0.0f);
  for (int i = 0; i < n; i++) {
    const float3 &a = p[i];
    const float3 &b = p[(i + 1) % n];
    nrm[0] += (a[1] - b[1]) * (a[2] + b[2]);
    nrm[1] += (a[2] - b[2]) * (a[0] + b[0]);
    nrm[2] += (a[0] - b[0]) * (a[1] + b[1]);
  }
  return nrm;
}

static inline float vdot(const float3 &a, const float3 &b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline float vlen(const float3 &a)
{
  return std::sqrt(vdot(a, a));
}

/* Same vertex multiset (order/winding-independent); triangles in the patch are
 * uniquely identified by their 3 vertices, so this matches a face before/after
 * the v_kill->v_keep remap. */
static inline bool sameVertSet(const Vector<int, 8> &a, const Vector<int, 8> &b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (int x : a) {
    bool found = false;
    for (int y : b) {
      if (x == y) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

/* Faces on `edge`'s radial cycle (0 = wire, 1 = boundary, 2 = manifold,
 * >2 = non-manifold). */
static inline int radialFaceCount(Mesh &m, int edge)
{
  int c0 = m.e.c[edge];
  if (c0 == ELEM_NONE) {
    return 0;
  }
  int n = 0, cc = c0;
  do {
    n++;
    cc = m.c.radial_next[cc];
  } while (cc != c0 && n < 1000000);
  return n;
}

/* Boundary (radial==1) + non-manifold (radial 0 or >2) edge counts over the
 * live mesh, restricted to edges incident to any vert in `verts`. Dead/edgeless
 * verts are skipped, so the same vert list works before and after the collapse
 * (v_kill simply drops out). */
static inline void census(Mesh &m, const Vector<int> &verts, int &boundary,
                          int &nonmanifold)
{
  boundary = 0;
  nonmanifold = 0;
  Set<int> seen;
  for (int v : verts) {
    if (v < 0 || v >= int(m.v.capacity()) || m.v.freemap[v] ||
        m.v.e[v] == ELEM_NONE) {
      continue;
    }
    for (int e : EdgeOfVertIter(&m, v, m.v.e[v])) {
      if (!seen.add(e)) {
        continue;
      }
      int rc = radialFaceCount(m, e);
      if (rc == 1) {
        boundary++;
      } else if (rc == 0 || rc > 2) {
        nonmanifold++;
      }
    }
  }
}

/* Self-contained copy of the 2-ring patch around a collapse edge: positions +
 * face vertex-sequences in local (0-based) indices, the local indices of the
 * collapsed edge's endpoints, and the before-collapse boundary/non-manifold
 * census the after-check compares against. */
struct PatchSnapshot {
  Vector<float3> co;            // local index -> position
  Vector<int> globalVid;        // local index -> global vert id
  Vector<Vector<int, 8>> faces; // each face as a local-index sequence
  // Geometric-fold detection: per face (parallel to `faces`), its vertices as
  // GLOBAL ids with v_kill remapped to v_keep, and its before-collapse Newell
  // normal (original positions). The after-check matches a surviving live face
  // by vertex set and flags an orientation flip.
  Vector<Vector<int, 8>> faceGlobalRemap;
  Vector<float3> faceNormalBefore;
  int keepLocal = -1, killLocal = -1;
  int keepGlobal = -1, killGlobal = -1;
  int boundaryBefore = 0, nonmanifoldBefore = 0;
  bool valid = false;
};

/* Map a global vert id to its local patch index (linear; patches are small). */
static inline int localOf(const PatchSnapshot &snap, int gv)
{
  for (int i = 0; i < int(snap.globalVid.size()); i++) {
    if (snap.globalVid[i] == gv) {
      return i;
    }
  }
  return -1;
}

/* Capture the 2-ring face patch around `edge` (BFS two vert-rings out from the
 * two endpoints, collecting incident faces + their verts), build a local
 * indexing, and record the before census. Call immediately before the mutation. */
static inline void capture(Mesh &m, int edge, PatchSnapshot &snap)
{
  int v_keep = m.e.vs[edge][0];
  int v_kill = m.e.vs[edge][1];

  Set<int> vset, fset;
  vset.add(v_keep);
  vset.add(v_kill);
  Vector<int> cur;
  cur.append(v_keep);
  cur.append(v_kill);

  for (int ring = 0; ring < 2; ring++) {
    Vector<int> nxt;
    for (int v : cur) {
      if (v < 0 || v >= int(m.v.capacity()) || m.v.freemap[v] ||
          m.v.e[v] == ELEM_NONE) {
        continue;
      }
      for (int e : EdgeOfVertIter(&m, v, m.v.e[v])) {
        int c0 = m.e.c[e];
        if (c0 == ELEM_NONE) {
          continue;
        }
        int cc = c0;
        do {
          int f = m.l.f[m.c.l[cc]];
          if (fset.add(f)) {
            int li = m.f.l[f], fc0 = m.l.c[li], fcc = fc0;
            do {
              int fv = m.c.v[fcc];
              if (vset.add(fv)) {
                nxt.append(fv);
              }
              fcc = m.c.next[fcc];
            } while (fcc != fc0);
          }
          cc = m.c.radial_next[cc];
        } while (cc != c0);
      }
    }
    cur = std::move(nxt);
  }

  // Local indexing: globalVid order defines the OBJ vertex order.
  for (int v : vset) {
    snap.globalVid.append(v);
    snap.co.append(m.v.co[v]);
  }
  snap.keepGlobal = v_keep;
  snap.killGlobal = v_kill;
  snap.keepLocal = localOf(snap, v_keep);
  snap.killLocal = localOf(snap, v_kill);

  for (int f : fset) {
    Vector<int, 8> seq;       // local indices (OBJ order)
    Vector<int, 8> gseq;      // global ids, v_kill -> v_keep remapped
    Vector<float3, 8> pts;    // original positions, for the before normal
    int li = m.f.l[f], c0 = m.l.c[li], cc = c0;
    do {
      int gv = m.c.v[cc];
      seq.append(localOf(snap, gv));
      gseq.append(gv == v_kill ? v_keep : gv);
      pts.append(m.v.co[gv]);
      cc = m.c.next[cc];
    } while (cc != c0);
    snap.faceNormalBefore.append(newellNormal(pts.data(), int(pts.size())));
    snap.faces.append(std::move(seq));
    snap.faceGlobalRemap.append(std::move(gseq));
  }

  census(m, snap.globalVid, snap.boundaryBefore, snap.nonmanifoldBefore);
  snap.valid = true;
}

/* Scan faces incident to v_keep after the collapse; if any matches a captured
 * before-face by vertex set and its orientation flipped (normalized normal dot
 * < 0), the collapse folded a neighbor. Returns the worst dot found (1 = none). */
static inline float foldCheck(Mesh &m, const PatchSnapshot &snap)
{
  int v_keep = snap.keepGlobal;
  if (v_keep < 0 || v_keep >= int(m.v.capacity()) || m.v.freemap[v_keep] ||
      m.v.e[v_keep] == ELEM_NONE) {
    return 1.0f;
  }
  float worst = 1.0f;
  Set<int> seenFace;
  for (int e : EdgeOfVertIter(&m, v_keep, m.v.e[v_keep])) {
    int c0 = m.e.c[e];
    if (c0 == ELEM_NONE) {
      continue;
    }
    int cc = c0;
    do {
      int f = m.l.f[m.c.l[cc]];
      if (seenFace.add(f)) {
        Vector<int, 8> g;
        Vector<float3, 8> pts;
        int li = m.f.l[f], fc0 = m.l.c[li], fcc = fc0;
        do {
          int gv = m.c.v[fcc];
          g.append(gv);
          pts.append(m.v.co[gv]);
          fcc = m.c.next[fcc];
        } while (fcc != fc0);
        float3 nAfter = newellNormal(pts.data(), int(pts.size()));
        float la = vlen(nAfter);
        for (int i = 0; i < int(snap.faceGlobalRemap.size()); i++) {
          if (!sameVertSet(g, snap.faceGlobalRemap[i])) {
            continue;
          }
          const float3 &nB = snap.faceNormalBefore[i];
          float lb = vlen(nB);
          if (lb > 1e-12f && la > 1e-12f) {
            float d = vdot(nB, nAfter) / (lb * la);
            if (d < worst) {
              worst = d;
            }
          }
          break;
        }
      }
      cc = m.c.radial_next[cc];
    } while (cc != c0);
  }
  return worst;
}

/* After the collapse, re-census the surviving patch verts and scan for a folded
 * neighbor. If a non-manifold edge appeared (count rose), a hole opened in a
 * region that was closed (boundary went 0 -> >0), or an incident face flipped
 * orientation, dump the captured BEFORE patch as a replayable OBJ and return
 * true. Dumps are capped so a broken run can't fill the disk. */
static inline bool checkAndDump(Mesh &m, const PatchSnapshot &snap)
{
  if (!snap.valid) {
    return false;
  }
  int b1 = 0, nm1 = 0;
  census(m, snap.globalVid, b1, nm1);
  bool newNonmanifold = nm1 > snap.nonmanifoldBefore;
  bool newHole = snap.boundaryBefore == 0 && b1 > 0;
  float foldDot = foldCheck(m, snap);
  /* A flip (dot < 0) is always a defect; the threshold can be loosened via
   * $SCULPTCORE_COLLAPSE_FOLD_DOT to survey near-folds (e.g. 0.7 catches > ~45
   * deg turns). */
  float foldThresh = 0.0f;
  if (const char *t = std::getenv("SCULPTCORE_COLLAPSE_FOLD_DOT")) {
    foldThresh = float(std::atof(t));
  }
  bool fold = foldDot < foldThresh;
  if (!newNonmanifold && !newHole && !fold) {
    return false;
  }

  static int idx = 0;
  static const int kMaxDumps = 64;
  if (idx >= kMaxDumps) {
    if (idx == kMaxDumps) {
      fprintf(stderr, "[collapse_debug] dump cap (%d) hit; suppressing further\n",
              kMaxDumps);
      idx++;
    }
    return true;
  }

  const char *dir = std::getenv("SCULPTCORE_COLLAPSE_DEBUG_DIR");
  char path[512];
  std::snprintf(path, sizeof(path), "%s/collapse_defect_%04d.obj",
                dir && dir[0] ? dir : ".", idx);
  std::FILE *fp = std::fopen(path, "wb");
  if (!fp) {
    fprintf(stderr, "[collapse_debug] cannot open %s\n", path);
    return true;
  }
  std::fprintf(fp, "# collapse defect:%s%s%s\n",
               newNonmanifold ? " nonmanifold" : "", newHole ? " hole" : "",
               fold ? " fold" : "");
  std::fprintf(fp, "# census before: boundary=%d nonmanifold=%d\n",
               snap.boundaryBefore, snap.nonmanifoldBefore);
  std::fprintf(fp, "# census after:  boundary=%d nonmanifold=%d\n", b1, nm1);
  std::fprintf(fp, "# fold: worst normal dot = %.4g (< 0 = flipped neighbor)\n",
               foldDot);
  std::fprintf(fp,
               "# collapse_edge keep=%d kill=%d (1-based OBJ vertex indices; "
               "v_keep survives, v_kill is removed)\n",
               snap.keepLocal + 1, snap.killLocal + 1);
  std::fprintf(fp, "#select %d %d\n", snap.keepLocal + 1, snap.killLocal + 1);
  for (int i = 0; i < int(snap.co.size()); i++) {
    std::fprintf(fp, "v %.7g %.7g %.7g\n", snap.co[i][0], snap.co[i][1],
                 snap.co[i][2]);
  }
  for (const auto &seq : snap.faces) {
    std::fprintf(fp, "f");
    for (int lv : seq) {
      std::fprintf(fp, " %d", lv + 1);
    }
    std::fprintf(fp, "\n");
  }
  std::fclose(fp);
  fprintf(stderr,
          "[collapse_debug] defect%s%s%s -> %s (v_keep=%d v_kill=%d, V=%d F=%d, "
          "foldDot=%.3g)\n",
          newNonmanifold ? " nonmanifold" : "", newHole ? " hole" : "",
          fold ? " fold" : "", path, snap.keepGlobal, snap.killGlobal,
          int(snap.co.size()), int(snap.faces.size()), foldDot);
  idx++;
  return true;
}

} // namespace sculptcore::mesh::collapse_debug

#endif // SCULPTCORE_COLLAPSE_DEBUG
