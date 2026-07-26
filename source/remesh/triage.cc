#include "remesh/triage.h"

#include "mesh/mesh.h"
#include "mesh/utils/attr_interp.h"
#include "mesh/utils/mesh_validate.h" // faceNewellNormal

#include "litestl/math/geom.h" // closestPointOnTri
#include "litestl/util/hash.h"
#include "litestl/util/map.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <utility>

namespace sculptcore::remesh {

using namespace litestl;
using math::float3;
using mesh::Mesh;

namespace {

// Spatial-hash cell key (integer lattice coords) and a canonical (sorted) tri
// vertex key for dedup. FNV-1a mix over the three components.
struct CellKey {
  int x, y, z;
  bool operator==(const CellKey &o) const { return x == o.x && y == o.y && z == o.z; }
  hash::HashInt computeHash() const
  {
    hash::HashInt h = 1469598103934665603ull;
    for (int v : {x, y, z})
      h = (h ^ hash::HashInt(uint32_t(v))) * 1099511628211ull;
    return h;
  }
};

struct TriKey {
  int a, b, c; // sorted ascending
  bool operator==(const TriKey &o) const { return a == o.a && b == o.b && c == o.c; }
  hash::HashInt computeHash() const
  {
    hash::HashInt h = 1469598103934665603ull;
    for (int v : {a, b, c})
      h = (h ^ hash::HashInt(uint32_t(v))) * 1099511628211ull;
    return h;
  }
};

struct Survivor {
  util::Vector<int, 4> verts;
  mesh::AttrRowSnapshot snap;
};

// Union-find with path halving; root = the lowest index (deterministic, and
// keeps the original low-index vertex as the weld representative).
int ufFind(util::Vector<int> &uf, int x)
{
  while (uf[x] != x) {
    uf[x] = uf[uf[x]];
    x = uf[x];
  }
  return x;
}
void ufUnite(util::Vector<int> &uf, int a, int b)
{
  a = ufFind(uf, a);
  b = ufFind(uf, b);
  if (a == b)
    return;
  if (a < b)
    uf[b] = a;
  else
    uf[a] = b;
}

} // namespace

void triageMesh(Mesh &m, const TriageParams &params, TriageReport &report)
{
  report.ran = true;

  int vcap = int(m.v.capacity());
  if (vcap == 0) {
    return;
  }

  // Bounding-box diagonal → weld tolerance.
  bool have = false;
  float3 bmin{}, bmax{};
  for (int v : m.v) {
    float3 co = m.v.co[v];
    if (!have) {
      bmin = bmax = co;
      have = true;
      continue;
    }
    bmin.min(co);
    bmax.max(co);
  }
  float diag = have ? (bmax - bmin).length() : 0.0f;
  float tol = params.weld_rel > 0.0f ? params.weld_rel * diag : 0.0f;

  bool mutated = false;

  // --- 1. Weld near-coincident verts: spatial hash (cell = tol) → union-find. ---
  if (tol > 0.0f && diag > 0.0f) {
    util::Vector<int> vuf;
    vuf.resize(vcap);
    for (int v : m.v) {
      vuf[v] = v;
    }

    const float inv = 1.0f / tol;
    auto cellOf = [&](const float3 &p) {
      return CellKey{int(std::floor(p[0] * inv)), int(std::floor(p[1] * inv)),
                     int(std::floor(p[2] * inv))};
    };

    util::Map<CellKey, util::Vector<int>> grid;
    const float tol2 = tol * tol;
    for (int v : m.v) {
      float3 pv = m.v.co[v];
      CellKey base = cellOf(pv);
      for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
          for (int dz = -1; dz <= 1; dz++) {
            CellKey k{base.x + dx, base.y + dy, base.z + dz};
            util::Vector<int> *bucket = grid.lookup_ptr(k);
            if (!bucket)
              continue;
            for (int w : *bucket) {
              float3 d = m.v.co[w] - pv;
              if (d.dot(d) <= tol2)
                ufUnite(vuf, v, w);
            }
          }
        }
      }
      grid[base].append(v);
    }

    int welded = 0;
    for (int v : m.v) {
      if (ufFind(vuf, v) != v)
        welded++;
    }
    report.welded_verts = welded;

    // --- Rebuild topology on the remapped verts (only when verts actually merged). ---
    if (welded > 0) {
      mutated = true;

      util::Vector<int> faceList;
      for (int f : m.f) {
        faceList.append(f);
      }

      util::Vector<Survivor> survivors;
      util::Set<TriKey> seen;

      for (int f : faceList) {
        // Remap each corner to its weld root, collapsing consecutive duplicates.
        util::Vector<int, 8> rv;
        int li = m.f.l[f], c0 = m.l.c[li], cc = c0;
        do {
          int r = ufFind(vuf, m.c.v[cc]);
          if (rv.size() == 0 || rv[int(rv.size()) - 1] != r)
            rv.append(r);
          cc = m.c.next[cc];
        } while (cc != c0);
        int n = int(rv.size());
        if (n >= 2 && rv[0] == rv[n - 1])
          n--; // wrap-around duplicate
        if (n < 3) {
          report.removed_degenerate_faces++;
          continue;
        }
        if (n == 3) {
          int a = rv[0], b = rv[1], c = rv[2];
          if (a > b) std::swap(a, b);
          if (b > c) std::swap(b, c);
          if (a > b) std::swap(a, b);
          if (!seen.add(TriKey{a, b, c})) {
            report.removed_duplicate_faces++;
            continue;
          }
        }
        Survivor s;
        for (int i = 0; i < n; i++)
          s.verts.append(rv[i]);
        snapshotAttrRow(m.f.attrs, f, s.snap);
        survivors.append(std::move(s));
      }

      util::Vector<int> killVerts;
      for (int v : m.v) {
        if (ufFind(vuf, v) != v)
          killVerts.append(v);
      }

      for (int f : faceList)
        m.kill_face(f);
      for (int v : killVerts)
        m.kill_vertex(v);

      for (Survivor &s : survivors) {
        int nf = m.make_face(std::span<int>(s.verts.data(), int(s.verts.size())));
        restoreAttrRow(m.f.attrs, nf, s.snap);
      }
    }
  }

  // --- 2. Drop degenerate / zero-area faces (any face left near-zero area). ---
  {
    float areaEps = tol > 0.0f ? tol * tol : 1e-12f;
    float lenThresh = 2.0f * areaEps; // Newell length == 2*area
    util::Vector<int> degen;
    for (int f : m.f) {
      if (mesh::faceNewellNormal(m, f).length() < lenThresh)
        degen.append(f);
    }
    for (int f : degen)
      m.kill_face(f);
    if (degen.size() > 0) {
      report.removed_degenerate_faces += int(degen.size());
      mutated = true;
    }
  }

  // --- 3. Drop tiny disconnected components (opt-in: frac > 0). ---
  if (params.min_component_frac > 0.0f) {
    vcap = int(m.v.capacity());
    util::Vector<int> cuf;
    cuf.resize(vcap);
    for (int v : m.v) {
      cuf[v] = v;
    }
    for (int e : m.e) {
      ufUnite(cuf, m.e.vs[e][0], m.e.vs[e][1]);
    }
    util::Map<int, int> compCount;
    int total = 0;
    for (int v : m.v) {
      compCount[ufFind(cuf, v)]++;
      total++;
    }
    int thresh = int(std::floor(double(params.min_component_frac) * double(total)));
    util::Vector<int> dropVerts;
    util::Set<int> droppedRoots;
    for (int v : m.v) {
      int r = ufFind(cuf, v);
      if (compCount.lookup(r) < thresh) {
        dropVerts.append(v);
        droppedRoots.add(r);
      }
    }
    for (int v : dropVerts)
      m.kill_vertex(v);
    report.removed_components = int(droppedRoots.size());
    report.removed_component_verts = int(dropVerts.size());
    if (dropVerts.size() > 0)
      mutated = true;
  }

  // --- 4. Kill residual face-less (wire) edges, incl. any zero-length leftovers. ---
  {
    util::Vector<int> wire;
    for (int e : m.e) {
      if (m.e.c[e] == ELEM_NONE)
        wire.append(e);
    }
    for (int e : wire)
      m.kill_edge(e);
    if (wire.size() > 0) {
      report.removed_wire_edges = int(wire.size());
      mutated = true;
    }
  }

  // --- 5. Detect (do NOT repair) non-manifold edges/verts on the cleaned mesh. ---
  {
    vcap = int(m.v.capacity());
    util::Vector<int> bndCount;
    bndCount.resize(vcap);
    for (int i = 0; i < vcap; i++)
      bndCount[i] = 0;

    int nmEdges = 0;
    for (int e : m.e) {
      int c0 = m.e.c[e];
      if (c0 == ELEM_NONE) {
        nmEdges++; // wire (should be gone after step 4)
        continue;
      }
      int radial = 0, cc = c0;
      do {
        radial++;
        cc = m.c.radial_next[cc];
      } while (cc != c0 && radial < 1000000);
      if (radial != 1 && radial != 2)
        nmEdges++;
      else if (radial == 1) {
        bndCount[m.e.vs[e][0]]++;
        bndCount[m.e.vs[e][1]]++;
      }
    }
    int nmVerts = 0;
    for (int v : m.v) {
      if (bndCount[v] > 2)
        nmVerts++;
    }
    report.non_manifold_edges = nmEdges;
    report.non_manifold_verts = nmVerts;
  }

  if (mutated) {
    m.recalc_normals();
  }
}

void fillInputHoles(Mesh &m, float max_frac, TriageReport &report)
{
  if (max_frac <= 0.0f) {
    return;
  }

  // Boundary edges (radial 1) + per-vert boundary incidence. A vert with != 2
  // incident boundary edges is a pinch; loops through one are preserved.
  int vcap = int(m.v.capacity());
  util::Vector<int> bcount, bedge0, bedge1;
  bcount.resize(vcap);
  bedge0.resize(vcap);
  bedge1.resize(vcap);
  for (int i = 0; i < vcap; i++) {
    bcount[i] = 0;
    bedge0[i] = bedge1[i] = ELEM_NONE;
  }

  auto edgeLen = [&](int e) {
    return double((m.v.co[m.e.vs[e][0]] - m.v.co[m.e.vs[e][1]]).length());
  };

  util::Vector<int> bedges;
  double totalLen = 0.0;
  for (int e : m.e) {
    int c = m.e.c[e];
    if (c == ELEM_NONE || m.c.radial_next[c] != c) {
      continue;
    }
    bedges.append(e);
    totalLen += edgeLen(e);
    for (int s = 0; s < 2; s++) {
      int v = m.e.vs[e][s];
      if (bcount[v] == 0)
        bedge0[v] = e;
      else if (bcount[v] == 1)
        bedge1[v] = e;
      bcount[v]++;
    }
  }
  if (!bedges.size() || totalLen <= 0.0) {
    return;
  }
  const double thresh = double(max_frac) * totalLen;

  util::Vector<char> evisited;
  evisited.resize(int(m.e.capacity()));
  for (int i = 0; i < int(evisited.size()); i++)
    evisited[i] = 0;

  util::Vector<int> loop, fan;
  int filled = 0;
  for (int e0 : bedges) {
    if (evisited[e0]) {
      continue;
    }
    // Trace the loop opposite the face winding (sole radial corner c0 runs
    // a->b face-wise, so the cap traverses b->a): cap faces emitted in trace
    // order then wind consistently with their existing neighbours.
    int c0 = m.e.c[e0];
    int vStart = m.c.v[m.c.next[c0]];
    int vCur = m.c.v[c0];
    int eCur = e0;
    loop.clear();
    loop.append(vStart);
    evisited[e0] = 1;
    double rimLen = edgeLen(e0);
    bool simple = bcount[vStart] == 2;
    while (vCur != vStart) {
      loop.append(vCur);
      if (bcount[vCur] != 2) {
        simple = false;
        break;
      }
      int eNext = bedge0[vCur] == eCur ? bedge1[vCur] : bedge0[vCur];
      if (eNext == ELEM_NONE || evisited[eNext]) {
        simple = false;
        break;
      }
      evisited[eNext] = 1;
      rimLen += edgeLen(eNext);
      vCur = m.e.vs[eNext][0] == vCur ? m.e.vs[eNext][1] : m.e.vs[eNext][0];
      eCur = eNext;
    }
    if (!simple) {
      // Consume the rest of this pinched/tangled chain backward from vStart so
      // it is counted (and preserved) exactly once.
      vCur = vStart;
      eCur = e0;
      while (bcount[vCur] == 2) {
        int eNext = bedge0[vCur] == eCur ? bedge1[vCur] : bedge0[vCur];
        if (eNext == ELEM_NONE || evisited[eNext])
          break;
        evisited[eNext] = 1;
        vCur = m.e.vs[eNext][0] == vCur ? m.e.vs[eNext][1] : m.e.vs[eNext][0];
        eCur = eNext;
      }
      report.input_holes_kept++;
      continue;
    }

    int n = int(loop.size());
    if (n < 3 || rimLen >= thresh) {
      report.input_holes_kept++;
      continue;
    }

    if (n == 3) {
      m.make_face(std::span<int>(loop.data(), 3));
      report.input_hole_fill_faces++;
    }
    else {
      // Centroid fan: robust on the non-convex tiny rims this targets.
      float3 cen(0.0f, 0.0f, 0.0f);
      for (int i = 0; i < n; i++)
        cen += m.v.co[loop[i]];
      cen *= 1.0f / float(n);
      int nc = m.make_vertex(cen);
      mesh::interpAttrs(m.v.attrs, nc, loop[0], loop[0], 0.0f, &m);
      m.v.co[nc] = cen;
      for (int i = 0; i < n; i++) {
        fan.clear();
        fan.append(loop[i]);
        fan.append(loop[(i + 1) % n]);
        fan.append(nc);
        m.make_face(fan);
        report.input_hole_fill_faces++;
      }
    }
    report.input_holes_filled++;
    filled++;
  }

  if (filled > 0) {
    m.recalc_normals();
  }
}

void detectThinSheets(Mesh &m, float thickness, TriageReport &report)
{
  report.thin_thickness = thickness;
  if (thickness <= 0.0f || m.f.count == 0) {
    return;
  }
  m.thawTopo();

  // Per-triangle verts / unit normal / centroid / area from current geometry
  // (Newell — no dependency on cached normals). Non-tris and degenerates skip.
  util::Vector<int> tv;
  util::Vector<float3> tno, tcen;
  util::Vector<float> tarea;
  double esum = 0.0;
  int ecount = 0;
  for (int f : m.f) {
    int c0 = m.l.c[m.f.l[f]], cc = c0, n = 0;
    int vs[3];
    do {
      if (n < 3) {
        vs[n] = m.c.v[cc];
      }
      n++;
      cc = m.c.next[cc];
    } while (cc != c0);
    if (n != 3) {
      continue;
    }
    float3 nrm = mesh::faceNewellNormal(m, f);
    float len = nrm.length();
    if (len < 1e-20f) {
      continue;
    }
    for (int k = 0; k < 3; k++) {
      tv.append(vs[k]);
      esum += double((m.v.co[vs[(k + 1) % 3]] - m.v.co[vs[k]]).length());
      ecount++;
    }
    tno.append(nrm * (1.0f / len));
    tcen.append((m.v.co[vs[0]] + m.v.co[vs[1]] + m.v.co[vs[2]]) * (1.0f / 3.0f));
    tarea.append(0.5f * len);
  }
  int nt = int(tarea.size());
  if (nt == 0) {
    return;
  }

  // Triangles smear into every cell their AABB overlaps; with cell >= max of
  // (thickness, 2x mean edge) both the smear and the query box stay O(1) cells.
  float mean_edge = ecount > 0 ? float(esum / ecount) : 0.0f;
  const float cell = std::fmax(thickness, 2.0f * mean_edge);
  const float inv = 1.0f / cell;
  util::Map<CellKey, util::Vector<int>> grid;
  for (int i = 0; i < nt; i++) {
    float3 a = m.v.co[tv[i * 3]], b = m.v.co[tv[i * 3 + 1]],
           c = m.v.co[tv[i * 3 + 2]];
    int lo[3], hi[3];
    for (int k = 0; k < 3; k++) {
      float mn = std::fmin(a[k], std::fmin(b[k], c[k]));
      float mx = std::fmax(a[k], std::fmax(b[k], c[k]));
      lo[k] = int(std::floor(mn * inv));
      hi[k] = int(std::floor(mx * inv));
    }
    for (int x = lo[0]; x <= hi[0]; x++) {
      for (int y = lo[1]; y <= hi[1]; y++) {
        for (int z = lo[2]; z <= hi[2]; z++) {
          grid[CellKey{x, y, z}].append(i);
        }
      }
    }
  }

  const int stride = nt > 4096 ? (nt + 4095) / 4096 : 1;
  const float t2 = thickness * thickness;
  double area_sampled = 0.0, area_thin = 0.0;
  for (int i = 0; i < nt; i += stride) {
    report.thin_sampled_faces++;
    area_sampled += double(tarea[i]);
    const float3 cen = tcen[i], nf = tno[i];
    bool thin = false;
    int lo[3], hi[3];
    for (int k = 0; k < 3; k++) {
      lo[k] = int(std::floor((cen[k] - thickness) * inv));
      hi[k] = int(std::floor((cen[k] + thickness) * inv));
    }
    for (int x = lo[0]; x <= hi[0] && !thin; x++) {
      for (int y = lo[1]; y <= hi[1] && !thin; y++) {
        for (int z = lo[2]; z <= hi[2] && !thin; z++) {
          util::Vector<int> *cand = grid.lookup_ptr(CellKey{x, y, z});
          if (!cand) {
            continue;
          }
          for (int j : *cand) {
            if (j == i || nf.dot(tno[j]) >= -0.5f) {
              continue;
            }
            bool shared = false;
            for (int p = 0; p < 3 && !shared; p++) {
              for (int q = 0; q < 3; q++) {
                if (tv[i * 3 + p] == tv[j * 3 + q]) {
                  shared = true;
                  break;
                }
              }
            }
            if (shared) {
              continue;
            }
            float3 cp = math::closestPointOnTri(cen, m.v.co[tv[j * 3]],
                                                m.v.co[tv[j * 3 + 1]],
                                                m.v.co[tv[j * 3 + 2]]);
            float3 d = cp - cen;
            float d2 = d.lengthSqr();
            if (d2 >= t2) {
              continue;
            }
            // Stacked along the normal (front/behind), not laterally adjacent
            // across a groove; a near-coincident hit is stacked by definition.
            float dl = std::sqrt(d2);
            if (dl > 1e-6f * thickness && std::fabs(d.dot(nf)) < 0.5f * dl) {
              continue;
            }
            thin = true;
            break;
          }
        }
      }
    }
    if (thin) {
      report.thin_paired_faces++;
      area_thin += double(tarea[i]);
    }
  }
  report.thin_area_frac =
      area_sampled > 0.0 ? float(area_thin / area_sampled) : 0.0f;
  report.thin_sheet = report.thin_area_frac > 0.5f;
}

} // namespace sculptcore::remesh
