#include "remesh/triage.h"

#include "mesh/mesh.h"
#include "mesh/utils/attr_interp.h"
#include "mesh/utils/mesh_validate.h" // faceNewellNormal

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

} // namespace sculptcore::remesh
