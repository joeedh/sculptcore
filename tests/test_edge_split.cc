#include "test_util.h"

#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/utils/edge_split.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/rand.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace litestl;
using namespace litestl::math;
using namespace litestl::util;

/* --- Mesh integrity (mirrors test_edge_collapse.cc::validateMesh) --- */
static bool validateMesh(Mesh &m, const char *tag)
{
  for (int ei : m.e) {
    int v1 = m.e.vs[ei][0];
    int v2 = m.e.vs[ei][1];
    if (v1 < 0 || v2 < 0 || v1 >= int(m.v.capacity()) || v2 >= int(m.v.capacity())) {
      fprintf(stderr, "[%s] edge %d bad vert refs %d %d\n", tag, ei, v1, v2);
      return false;
    }
    if (m.v.freemap[v1] || m.v.freemap[v2]) {
      fprintf(stderr, "[%s] edge %d references freed vert\n", tag, ei);
      return false;
    }
    if (v1 == v2) {
      fprintf(stderr, "[%s] edge %d is a self-loop\n", tag, ei);
      return false;
    }
  }

  for (int vi : m.v) {
    int e0 = m.v.e[vi];
    if (e0 == ELEM_NONE) continue;
    int steps = 0, ec = e0;
    do {
      int side = m.e.vs[ec][0] == vi ? 0 : 1;
      if (m.e.vs[ec][side] != vi) {
        fprintf(stderr, "[%s] vert %d disk edge %d not incident\n", tag, vi, ec);
        return false;
      }
      int next = diskEdge(m.e.disk[ec][side * 2 + 1]);
      int prev = diskEdge(m.e.disk[ec][side * 2]);
      int side_n = m.e.vs[next][0] == vi ? 0 : 1;
      int side_p = m.e.vs[prev][0] == vi ? 0 : 1;
      if (m.e.disk[next][side_n * 2] != diskPack(ec, side) ||
          m.e.disk[prev][side_p * 2 + 1] != diskPack(ec, side)) {
        fprintf(stderr, "[%s] disk prev/next mismatch v=%d e=%d\n", tag, vi, ec);
        return false;
      }
      ec = next;
      if (++steps > 1000000) {
        fprintf(stderr, "[%s] vert %d disk did not close\n", tag, vi);
        return false;
      }
    } while (ec != e0);
  }

  for (int ei : m.e) {
    int c0 = m.e.c[ei];
    if (c0 == ELEM_NONE) continue;
    int steps = 0, cc = c0;
    do {
      if (m.c.e[cc] != ei) {
        fprintf(stderr, "[%s] edge %d radial corner %d c.e mismatch\n", tag, ei, cc);
        return false;
      }
      int rn = m.c.radial_next[cc];
      int rp = m.c.radial_prev[cc];
      if (m.c.radial_prev[rn] != cc || m.c.radial_next[rp] != cc) {
        fprintf(stderr, "[%s] radial prev/next mismatch e=%d c=%d\n", tag, ei, cc);
        return false;
      }
      cc = rn;
      if (++steps > 1000000) {
        fprintf(stderr, "[%s] edge %d radial did not close\n", tag, ei);
        return false;
      }
    } while (cc != c0);
  }

  for (int fi : m.f) {
    int li = m.f.l[fi];
    while (li != ELEM_NONE) {
      if (m.l.f[li] != fi) {
        fprintf(stderr, "[%s] list %d not owned by face %d\n", tag, li, fi);
        return false;
      }
      int c0 = m.l.c[li];
      int cc = c0;
      int n = 0;
      do {
        if (m.c.l[cc] != li) {
          fprintf(stderr, "[%s] corner %d c.l != list %d\n", tag, cc, li);
          return false;
        }
        int cn = m.c.next[cc];
        if (m.c.prev[cn] != cc) {
          fprintf(stderr, "[%s] corner prev/next mismatch f=%d c=%d\n", tag, fi, cc);
          return false;
        }
        int ce = m.c.e[cc];
        int v_here = m.c.v[cc];
        int v_next = m.c.v[cn];
        int ev0 = m.e.vs[ce][0], ev1 = m.e.vs[ce][1];
        if (!((ev0 == v_here && ev1 == v_next) ||
              (ev1 == v_here && ev0 == v_next))) {
          fprintf(stderr, "[%s] corner edge-vert mismatch f=%d c=%d\n", tag, fi, cc);
          return false;
        }
        cc = cn;
        if (++n > 1000000) {
          fprintf(stderr, "[%s] list %d did not close\n", tag, li);
          return false;
        }
      } while (cc != c0);
      if (n != m.l.size[li]) {
        fprintf(stderr, "[%s] list %d size %d != walked %d\n", tag, li, m.l.size[li], n);
        return false;
      }
      li = m.l.next[li];
    }
  }

  return true;
}

static int64_t edgeKey(int a, int b)
{
  int lo = a < b ? a : b;
  int hi = a < b ? b : a;
  return (int64_t(lo) << 32) | uint32_t(hi);
}

static bool noDuplicateEdges(Mesh &m, const char *tag)
{
  Set<int64_t> seen;
  for (int ei : m.e) {
    int64_t k = edgeKey(m.e.vs[ei][0], m.e.vs[ei][1]);
    if (!seen.add(k)) {
      fprintf(stderr, "[%s] duplicate edge between %d-%d\n",
              tag, m.e.vs[ei][0], m.e.vs[ei][1]);
      return false;
    }
  }
  return true;
}

static int eulerChar(Mesh &m)
{
  return m.v.count - m.e.count + m.f.count;
}

/* Number of triangle faces incident to edge `ei`. */
static int edgeFaceCount(Mesh &m, int ei)
{
  int c0 = m.e.c[ei];
  if (c0 == ELEM_NONE) return 0;
  int n = 0, cc = c0;
  do {
    n++;
    cc = m.c.radial_next[cc];
  } while (cc != c0);
  return n;
}

/* Add a float4 "color" vertex attr seeded from position so interpolation
 * is verifiable (color == f(co)). */
static void addColorAttr(Mesh &m)
{
  AttrRef &ref = m.v.attrs.ensure(AttrType::FLOAT4, "color", /*materialize=*/true);
  ref.use = AttrUse::COLOR;
  auto *data = static_cast<AttrData<float4> *>(ref.data);
  data->materialize_all();
  for (int vi : m.v) {
    float3 co = m.v.co[vi];
    (*data)[vi] = float4(co[0], co[1], co[2], 1.0f);
  }
}

static AttrData<float4> *colorData(Mesh &m)
{
  AttrRef ref = m.v.attrs.find_attribute(AttrType::FLOAT4, "color");
  return ref.exists() ? static_cast<AttrData<float4> *>(ref.data) : nullptr;
}

/* --- Generators (triangle-only meshes) --- */

/* Triangulated NxN grid in the z=0 plane. Has a single boundary loop. */
static Mesh *makeTriGrid(int n)
{
  Mesh *m = alloc::New<Mesh>("test_edge_split grid");
  Vector<int> grid;
  grid.resize(n * n);
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      float fx = float(x) / float(n - 1) - 0.5f;
      float fy = float(y) / float(n - 1) - 0.5f;
      grid[y * n + x] = m->make_vertex(float3(fx, fy, 0.0f));
    }
  }
  for (int y = 0; y < n - 1; y++) {
    for (int x = 0; x < n - 1; x++) {
      int a = grid[y * n + x];
      int b = grid[y * n + x + 1];
      int c = grid[(y + 1) * n + x + 1];
      int d = grid[(y + 1) * n + x];
      int t0[3] = {a, b, c};
      int t1[3] = {a, c, d};
      m->make_face(std::span<int>(t0, 3));
      m->make_face(std::span<int>(t1, 3));
    }
  }
  return m;
}

/* Triangulated UV sphere (closed, no boundary). */
static Mesh *makeTriSphere(int rings, int segs)
{
  Mesh *m = alloc::New<Mesh>("test_edge_split sphere");
  const float PI = 3.14159265358979323846f;

  int top = m->make_vertex(float3(0, 0, 1));
  int bot = m->make_vertex(float3(0, 0, -1));

  /* interior ring verts: rings-1 latitude bands. */
  Vector<int> ringVerts;
  ringVerts.resize((rings - 1) * segs);
  for (int r = 1; r < rings; r++) {
    float theta = PI * float(r) / float(rings);
    float z = std::cos(theta);
    float sr = std::sin(theta);
    for (int s = 0; s < segs; s++) {
      float phi = 2.0f * PI * float(s) / float(segs);
      float3 co(sr * std::cos(phi), sr * std::sin(phi), z);
      ringVerts[(r - 1) * segs + s] = m->make_vertex(co);
    }
  }

  auto rv = [&](int r, int s) { return ringVerts[(r - 1) * segs + (s % segs)]; };

  /* top cap */
  for (int s = 0; s < segs; s++) {
    int a = rv(1, s);
    int b = rv(1, s + 1);
    int t[3] = {top, b, a};
    m->make_face(std::span<int>(t, 3));
  }
  /* middle quads -> two tris */
  for (int r = 1; r < rings - 1; r++) {
    for (int s = 0; s < segs; s++) {
      int a = rv(r, s);
      int b = rv(r, s + 1);
      int c = rv(r + 1, s + 1);
      int d = rv(r + 1, s);
      int t0[3] = {a, b, c};
      int t1[3] = {a, c, d};
      m->make_face(std::span<int>(t0, 3));
      m->make_face(std::span<int>(t1, 3));
    }
  }
  /* bottom cap */
  for (int s = 0; s < segs; s++) {
    int a = rv(rings - 1, s);
    int b = rv(rings - 1, s + 1);
    int t[3] = {bot, a, b};
    m->make_face(std::span<int>(t, 3));
  }
  return m;
}

static int randInRange(Random &r, int lo, int hi)
{
  if (hi <= lo) return lo;
  return lo + int(r.get_int() % uint32_t(hi - lo));
}

static bool finite4(const float4 &v)
{
  for (int i = 0; i < 4; i++) {
    if (!std::isfinite(v[i])) return false;
  }
  return true;
}

struct Stats {
  int splits = 0;
  int meshes = 0;
};

static bool runSplitSession(const char *tag, Mesh &m, Random &rnd,
                            int maxSplits, Stats &stats)
{
  if (!validateMesh(m, tag)) return false;
  if (!noDuplicateEdges(m, tag)) return false;

  for (int iter = 0; iter < maxSplits; iter++) {
    /* Pick a random live, non-wire edge. */
    Vector<int> candidates;
    for (int ei : m.e) {
      if (m.e.c[ei] == ELEM_NONE) continue;
      candidates.append(ei);
    }
    if (candidates.isEmpty()) break;
    int ei = candidates[randInRange(rnd, 0, int(candidates.size()))];

    int v0 = m.e.vs[ei][0];
    int v1 = m.e.vs[ei][1];
    int incident = edgeFaceCount(m, ei);

    /* Expected Euler delta for a triangle-mesh split. */
    int dV_expected = 1;
    int dE_expected = 1 + incident; /* v0-vm, vm-v1, plus one spoke per face */
    int dF_expected = incident;

    int V_before = m.v.count;
    int E_before = m.e.count;
    int F_before = m.f.count;
    int chi_before = eulerChar(m);

    float3 co0 = m.v.co[v0];
    float3 co1 = m.v.co[v1];
    float3 mid = (co0 + co1) * 0.5f;

    float4 col0(0.0f), col1(0.0f);
    AttrData<float4> *cd = colorData(m);
    if (cd) {
      col0 = (*cd)[v0];
      col1 = (*cd)[v1];
    }

    EdgeSplitResult res;
    auto ok = splitEdge(m, ei, &res);
    test_assert(bool(ok));
    if (!ok) return false;

    if (!validateMesh(m, tag)) {
      fprintf(stderr, "[%s] integrity failed after split iter=%d ei=%d\n",
              tag, iter, ei);
      return false;
    }
    if (!noDuplicateEdges(m, tag)) return false;

    int dV = m.v.count - V_before;
    int dE = m.e.count - E_before;
    int dF = m.f.count - F_before;
    int chi_after = eulerChar(m);

    if (dV != dV_expected || dE != dE_expected || dF != dF_expected) {
      fprintf(stderr,
              "[%s] euler delta mismatch iter=%d incident=%d: "
              "dV=%d/%d dE=%d/%d dF=%d/%d\n",
              tag, iter, incident, dV, dV_expected, dE, dE_expected,
              dF, dF_expected);
      return false;
    }
    if (chi_after != chi_before) {
      fprintf(stderr, "[%s] chi changed %d -> %d (iter %d)\n",
              tag, chi_before, chi_after, iter);
      return false;
    }

    /* New vertex position == midpoint, finite. */
    int vm = res.new_vert;
    if (vm == ELEM_NONE || m.v.freemap[vm]) {
      fprintf(stderr, "[%s] bad new vert\n", tag);
      return false;
    }
    float3 vmco = m.v.co[vm];
    for (int i = 0; i < 3; i++) {
      if (!std::isfinite(vmco[i]) || std::fabs(vmco[i] - mid[i]) > 1e-5f) {
        fprintf(stderr, "[%s] midpoint mismatch iter=%d axis=%d %f vs %f\n",
                tag, iter, i, vmco[i], mid[i]);
        return false;
      }
    }

    /* Color interpolated to the midpoint, finite. */
    if (cd) {
      float4 cm = (*cd)[vm];
      float4 expect = col0 * 0.5f + col1 * 0.5f;
      if (!finite4(cm)) {
        fprintf(stderr, "[%s] non-finite color iter=%d\n", tag, iter);
        return false;
      }
      for (int i = 0; i < 4; i++) {
        if (std::fabs(cm[i] - expect[i]) > 1e-5f) {
          fprintf(stderr, "[%s] color lerp mismatch iter=%d c=%d %f vs %f\n",
                  tag, iter, i, cm[i], expect[i]);
          return false;
        }
      }
    }

    /* Result struct sanity. created_faces is the GROSS set of new faces
     * (each incident triangle is replaced by two), so 2 per incident face —
     * not the net face delta. created_edges counts edges actually created
     * (v0-vm, vm-v1, plus one spoke per incident face); the original edge
     * is killed, so the net delta is one less. */
    int createdFaces_expected = 2 * incident;
    int createdEdges_expected = 2 + incident;
    if (int(res.created_faces.size()) != createdFaces_expected ||
        int(res.killed_faces.size()) != incident ||
        int(res.created_edges.size()) != createdEdges_expected) {
      fprintf(stderr,
              "[%s] result struct count mismatch iter=%d: "
              "cf=%zu kf=%zu ce=%zu\n",
              tag, iter, res.created_faces.size(), res.killed_faces.size(),
              res.created_edges.size());
      return false;
    }

    stats.splits++;
  }

  return true;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  Stats stats;

  /* Triangulated grids of several sizes (1 boundary loop; mixed
   * interior/boundary edge splits). */
  int gridSizes[] = {2, 3, 5, 9, 16};
  for (int N : gridSizes) {
    Mesh *m = makeTriGrid(N);
    addColorAttr(*m);
    Random rnd(uint32_t(N * 131 + 7));
    char tag[64];
    snprintf(tag, sizeof(tag), "grid-N%d", N);
    bool ok = runSplitSession(tag, *m, rnd, 100, stats);
    test_assert(ok);
    alloc::Delete<Mesh>(m);
    stats.meshes++;
  }

  /* Triangulated spheres (closed, no boundary -> all interior splits). */
  int sphereCfg[][2] = {{4, 6}, {6, 10}, {8, 16}};
  for (auto &cfg : sphereCfg) {
    Mesh *m = makeTriSphere(cfg[0], cfg[1]);
    addColorAttr(*m);
    Random rnd(uint32_t(cfg[0] * 977 + cfg[1] * 13));
    char tag[64];
    snprintf(tag, sizeof(tag), "sphere-%dx%d", cfg[0], cfg[1]);
    bool ok = runSplitSession(tag, *m, rnd, 100, stats);
    test_assert(ok);
    alloc::Delete<Mesh>(m);
    stats.meshes++;
  }

  /* Wire-edge edge case: split an isolated wire edge. */
  {
    Mesh m;
    int a = m.make_vertex(float3(0, 0, 0));
    int b = m.make_vertex(float3(2, 0, 0));
    int e = m.make_edge(a, b);
    EdgeSplitResult res;
    auto ok = splitEdge(m, e, &res);
    test_assert(bool(ok));
    test_assert(m.v.count == 3);
    test_assert(m.e.count == 2);
    test_assert(m.f.count == 0);
    test_assert(res.new_vert != ELEM_NONE);
    float3 mid = m.v.co[res.new_vert];
    test_assert(std::fabs(mid[0] - 1.0f) < 1e-5f);
    test_assert(validateMesh(m, "wire"));
  }

  /* Regression: a lazily-paged vert attr (the `.brush.orig.*` pattern) whose
   * pages are materialized only where the brush stamped, split so the new vert
   * lands in a never-materialized page. interpAttrs used to write through the
   * page's null data pointer (dyntopo-stroke-after-plain-stroke crash). */
  {
    Mesh *m = makeTriGrid(70); /* ~4900 verts: spans 2 attr pages */
    test_assert(m->v.count > ATTR_PAGESIZE + 2);

    AttrRef &ref =
        m->v.attrs.ensure(AttrType::FLOAT3, ".brush.orig.co", /*materialize=*/false);
    auto *orig = static_cast<AttrData<float3> *>(ref.data);
    orig->materialize(0); /* stamp page 0 only, like an in-region brush pass */
    (*orig)[0] = float3(1.0f, 2.0f, 3.0f);
    (*orig)[1] = float3(3.0f, 4.0f, 5.0f);

    int e = m->find_edge(0, 1);
    test_assert(e != ELEM_NONE);
    EdgeSplitResult res;
    auto ok = splitEdge(*m, e, &res);
    test_assert(bool(ok));

    int vm = res.new_vert;
    test_assert(vm >= ATTR_PAGESIZE); /* landed in the lazily-unmaterialized page */
    float3 got = orig->safe_get(vm);
    float3 expect(2.0f, 3.0f, 4.0f);
    for (int i = 0; i < 3; i++) {
      test_assert(std::fabs(got[i] - expect[i]) < 1e-5f);
    }
    test_assert(validateMesh(*m, "lazy-attr"));
    alloc::Delete<Mesh>(m);
    stats.meshes++;
  }

  /* Note: splitEdge does not validate its edge argument — callers must pass a
   * live edge. An invalid-edge guard test was removed deliberately. */

  printf("edge_split test: %d meshes, %d splits\n", stats.meshes, stats.splits);

  return test_end();
}
