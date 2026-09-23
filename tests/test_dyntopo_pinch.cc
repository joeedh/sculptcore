/** Gates for dyntopo's thin-tube pinch: the collapseEdge tet guard, the pinchSeparatingTriangle
 * operator, the pinch and cull passes in runDyntopoRemesh, and undo of both through the meshlog
 * with a live spatial tree. See claudeMemory/plans/dyntopo-thin-tube-pinch.md in the addon repo. */
#include "test_util.h"
#include "spatial_ownership.h"

#include "dyntopo/dyntopo.h"
#include "mesh/attr_weights.h"
#include "mesh/boundary.h"
#include "mesh/deform_pool.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/utils/pinch_off.h"
#include "meshlog/meshlog_base.h"
#include "spatial/spatial.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace sculptcore::dyntopo;
using namespace litestl;
using namespace litestl::math;
using namespace litestl::util;
namespace bnd = sculptcore::mesh::boundary;

static constexpr float kPi = 3.14159265358979f;

struct RevRing {
  float x, r;
  int n;
};

/** Builds a closed surface of revolution about the x axis, outward-facing. Each ring is a loop of
 * `n` verts starting at angle 0; neighbouring rings are zipped together by angle, so their vertex
 * counts may differ. The first and last rings are closed with a pole vertex each. */
static Mesh *makeRevolution(const Vector<RevRing> &rings, float x_start, float x_end)
{
  Mesh *m = alloc::New<Mesh>("test_dyntopo_pinch revolution");
  Vector<Vector<int>> ids;
  for (const RevRing &rr : rings) {
    Vector<int> ring;
    for (int k = 0; k < rr.n; k++) {
      float phi = 2.0f * kPi * float(k) / float(rr.n);
      ring.append(m->make_vertex(float3(rr.x, rr.r * std::cos(phi), rr.r * std::sin(phi))));
    }
    ids.append(std::move(ring));
  }
  auto tri = [&](int a, int b, int c) {
    int t[3] = {a, b, c};
    m->make_face(std::span<int>(t, 3));
  };

  int p0 = m->make_vertex(float3(x_start, 0.0f, 0.0f));
  const Vector<int> &first = ids[0];
  for (int k = 0; k < int(first.size()); k++) {
    tri(p0, first[(k + 1) % first.size()], first[k]);
  }

  for (int ri = 0; ri + 1 < int(ids.size()); ri++) {
    const Vector<int> &A = ids[ri];
    const Vector<int> &B = ids[ri + 1];
    int na = int(A.size()), nb = int(B.size());
    int i = 0, j = 0;
    while (i < na || j < nb) {
      float angA = float(i + 1) / float(na);
      float angB = float(j + 1) / float(nb);
      if (j == nb || (i < na && angA <= angB)) {
        tri(A[i % na], A[(i + 1) % na], B[j % nb]);
        i++;
      } else {
        tri(A[i % na], B[(j + 1) % nb], B[j % nb]);
        j++;
      }
    }
  }

  int p1 = m->make_vertex(float3(x_end, 0.0f, 0.0f));
  const Vector<int> &last = ids[ids.size() - 1];
  for (int k = 0; k < int(last.size()); k++) {
    tri(p1, last[k], last[(k + 1) % last.size()]);
  }
  return m;
}

/** Builds a torus about the z axis with `sections` cross-sections of 8 verts and radius 0.1. The
 * section at index 0 is a 3-vertex ring of radius `thin_r` instead, so the handle is pinched
 * there. That ring's vertices are 0, 1 and 2. */
static Mesh *makeThinTorus(int sections = 16, float thin_r = 0.004f)
{
  const float R = 0.5f;
  Mesh *m = alloc::New<Mesh>("test_dyntopo_pinch torus");
  Vector<Vector<int>> ids;
  for (int s = 0; s < sections; s++) {
    float th = 2.0f * kPi * float(s) / float(sections);
    float3 radial(std::cos(th), std::sin(th), 0.0f);
    float3 up(0.0f, 0.0f, 1.0f);
    int n = s == 0 ? 3 : 8;
    float r = s == 0 ? thin_r : 0.1f;
    Vector<int> ring;
    for (int k = 0; k < n; k++) {
      float phi = 2.0f * kPi * float(k) / float(n);
      ring.append(m->make_vertex(radial * (R + r * std::cos(phi)) + up * (r * std::sin(phi))));
    }
    ids.append(std::move(ring));
  }
  for (int s = 0; s < sections; s++) {
    const Vector<int> &A = ids[s];
    const Vector<int> &B = ids[(s + 1) % sections];
    int na = int(A.size()), nb = int(B.size());
    int i = 0, j = 0;
    while (i < na || j < nb) {
      float angA = float(i + 1) / float(na);
      float angB = float(j + 1) / float(nb);
      int t[3];
      if (j == nb || (i < na && angA <= angB)) {
        t[0] = A[i % na], t[1] = B[j % nb], t[2] = A[(i + 1) % na];
        i++;
      } else {
        t[0] = A[i % na], t[1] = B[j % nb], t[2] = B[(j + 1) % nb];
        j++;
      }
      m->make_face(std::span<int>(t, 3));
    }
  }
  return m;
}

static int ringCount(float r, float spacing)
{
  int n = int(std::lround(2.0f * kPi * r / spacing));
  return n < 3 ? 3 : n;
}

/** Builds two spheres of radius 0.25 at x = ±0.55, joined by a neck of `neck_n`-vertex rings with
 * radius `neck_r` spaced `neck_step` apart between x = ±0.28. */
static Mesh *makeDumbbell(float neck_r = 0.004f, float neck_step = 0.04f, int neck_n = 3)
{
  const float R = 0.25f, cx = 0.55f, spacing = 0.05f;
  Vector<RevRing> rings;
  const float th0 = 0.2f;
  const int bulbSteps = 14;
  for (int s = 1; s <= bulbSteps; s++) {
    float th = kPi - (kPi - th0) * float(s) / float(bulbSteps + 1);
    float r = R * std::sin(th);
    rings.append({-cx + R * std::cos(th), r, ringCount(r, spacing)});
  }
  for (float x = -0.28f; x <= 0.28f + 1e-4f; x += neck_step) {
    rings.append({x, neck_r, neck_n});
  }
  for (int s = bulbSteps; s >= 1; s--) {
    float th = kPi - (kPi - th0) * float(s) / float(bulbSteps + 1);
    float r = R * std::sin(th);
    rings.append({cx - R * std::cos(th), r, ringCount(r, spacing)});
  }
  return makeRevolution(rings, -cx - R, cx + R);
}

/** Builds a triangulated n×n grid in z = 0 over [-0.5, 0.5]², then pokes every third triangle at
 * its centroid, lifted by `bump`. Each poke leaves a valence-3 vertex whose opposite triangle is a
 * 3-cycle that is not a face. The poke nearest the origin is lifted by `spike` instead. */
static Mesh *makeBumpyPatch(int n = 17, float bump = 0.01f, float spike = 0.3f)
{
  Mesh *m = alloc::New<Mesh>("test_dyntopo_pinch patch");
  Vector<int> grid;
  grid.resize(n * n);
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      float fx = float(x) / float(n - 1) - 0.5f;
      float fy = float(y) / float(n - 1) - 0.5f;
      grid[y * n + x] = m->make_vertex(float3(fx, fy, 0.0f));
    }
  }
  Vector<int> tris;
  for (int y = 0; y < n - 1; y++) {
    for (int x = 0; x < n - 1; x++) {
      int a = grid[y * n + x], b = grid[y * n + x + 1];
      int c = grid[(y + 1) * n + x + 1], d = grid[(y + 1) * n + x];
      tris.append(a);
      tris.append(b);
      tris.append(c);
      tris.append(a);
      tris.append(c);
      tris.append(d);
    }
  }
  int ntri = int(tris.size()) / 3;
  int spikeTri = -1;
  float best = 1e30f;
  for (int t = 0; t < ntri; t += 3) {
    float3 cen = (m->v.co[tris[t * 3]] + m->v.co[tris[t * 3 + 1]] + m->v.co[tris[t * 3 + 2]]) *
                 (1.0f / 3.0f);
    if (cen.length() < best) {
      best = cen.length();
      spikeTri = t;
    }
  }
  for (int t = 0; t < ntri; t++) {
    int a = tris[t * 3], b = tris[t * 3 + 1], c = tris[t * 3 + 2];
    if (t % 3 != 0) {
      int f[3] = {a, b, c};
      m->make_face(std::span<int>(f, 3));
      continue;
    }
    float3 cen = (m->v.co[a] + m->v.co[b] + m->v.co[c]) * (1.0f / 3.0f);
    cen[2] = t == spikeTri ? spike : bump;
    int v = m->make_vertex(cen);
    int f0[3] = {a, b, v}, f1[3] = {b, c, v}, f2[3] = {c, a, v};
    m->make_face(std::span<int>(f0, 3));
    m->make_face(std::span<int>(f1, 3));
    m->make_face(std::span<int>(f2, 3));
  }
  return m;
}

static int edgeFaceCount(Mesh &m, int e)
{
  int c0 = m.e.c[e];
  if (c0 == ELEM_NONE) {
    return 0;
  }
  int n = 0, cc = c0;
  do {
    n++;
    cc = m.c.radial_next[cc];
  } while (cc != c0);
  return n;
}

/** True when the mesh passes the engine's structural validation, every face is a triangle, and
 * every edge has exactly two faces. */
static bool closedManifold(Mesh &m, const char *tag)
{
  if (m.validateAndRepair() != 0) {
    fprintf(stderr, "[%s] validateAndRepair reported problems\n", tag);
    return false;
  }
  for (int f : m.f) {
    if (m.f.list_count[f] != 1 || m.l.size[m.f.l[f]] != 3) {
      fprintf(stderr, "[%s] face %d is not a triangle\n", tag, f);
      return false;
    }
  }
  for (int e : m.e) {
    if (edgeFaceCount(m, e) != 2) {
      fprintf(stderr, "[%s] edge %d has %d faces\n", tag, e, edgeFaceCount(m, e));
      return false;
    }
    int c0 = m.e.c[e], c1 = m.c.radial_next[c0];
    if (m.c.v[c0] == m.c.v[c1]) {
      fprintf(stderr, "[%s] edge %d: both faces run the same way\n", tag, e);
      return false;
    }
  }
  return true;
}

struct Component {
  int verts = 0, edges = 0, faces = 0;
  int euler() const
  {
    return verts - edges + faces;
  }
};

/** Splits the mesh's faces into edge-connected components. */
static Vector<Component> components(Mesh &m)
{
  Vector<int> label;
  label.resize(m.f.capacity());
  for (int i = 0; i < int(label.size()); i++) {
    label[i] = -1;
  }
  Vector<Component> out;
  for (int f0 : m.f) {
    if (label[f0] >= 0) {
      continue;
    }
    int id = int(out.size());
    Set<int> vs, es;
    Component comp;
    Vector<int> stack;
    stack.append(f0);
    label[f0] = id;
    while (!stack.isEmpty()) {
      int f = stack.pop_back();
      comp.faces++;
      int c0 = m.l.c[m.f.l[f]], cc = c0;
      do {
        vs.add(m.c.v[cc]);
        es.add(m.c.e[cc]);
        for (int r = m.c.radial_next[cc]; r != cc; r = m.c.radial_next[r]) {
          int g = m.l.f[m.c.l[r]];
          if (label[g] < 0) {
            label[g] = id;
            stack.append(g);
          }
        }
        cc = m.c.next[cc];
      } while (cc != c0);
    }
    comp.verts = int(vs.size());
    comp.edges = int(es.size());
    out.append(comp);
  }
  return out;
}

/** Sums the signed volume of every triangle's tetrahedron with the origin. Positive for a closed,
 * outward-facing mesh. */
static float signedVolume(Mesh &m)
{
  double vol = 0.0;
  for (int f : m.f) {
    int c0 = m.l.c[m.f.l[f]];
    float3 a = m.v.co[m.c.v[c0]];
    float3 b = m.v.co[m.c.v[m.c.next[c0]]];
    float3 c = m.v.co[m.c.v[m.c.next[m.c.next[c0]]]];
    vol += double(a.dot(b.cross(c))) / 6.0;
  }
  return float(vol);
}

/** Hashes the element counts, the op counts and every live vertex position (FNV-1a over the raw
 * bits), so any change in what dyntopo did shows up. */
static uint64_t hashResult(Mesh &m, const DynTopoStats &st)
{
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint32_t x) {
    for (int i = 0; i < 4; i++) {
      h ^= (x >> (i * 8)) & 0xffu;
      h *= 1099511628211ull;
    }
  };
  mix(uint32_t(m.v.count));
  mix(uint32_t(m.e.count));
  mix(uint32_t(m.f.count));
  mix(uint32_t(st.splits));
  mix(uint32_t(st.collapses));
  mix(uint32_t(st.flips));
  for (int v : m.v) {
    mix(uint32_t(v));
    for (int k = 0; k < 3; k++) {
      uint32_t bits;
      float f = m.v.co[v][k];
      std::memcpy(&bits, &f, 4);
      mix(bits);
    }
  }
  return h;
}

static void addStats(DynTopoStats &sum, const DynTopoStats &st)
{
  sum.splits += st.splits;
  sum.collapses += st.collapses;
  sum.flips += st.flips;
}

static DynTopoParams dumbbellParams()
{
  DynTopoParams p;
  p.l_max = 0.045f;
  p.l_min = 0.015f;
  p.mode = DynTopoMode::Both;
  return p;
}

static DynTopoParams patchParams()
{
  DynTopoParams p;
  p.l_max = 0.2f;
  p.l_min = 0.1f;
  p.mode = DynTopoMode::Collapse;
  return p;
}

/** Runs `dabs` dyntopo passes over the dumbbell and returns the result hash. Even dabs cover the
 * neck; odd dabs sit on the right-hand join, where the sphere's edges exceed l_max. */
static uint64_t runDumbbell(const DynTopoParams &p, int dabs, DynTopoStats *out = nullptr)
{
  Mesh *m = makeDumbbell();
  DynTopoStats sum;
  for (int d = 0; d < dabs; d++) {
    float3 center = d % 2 == 0 ? float3(0.0f) : float3(0.32f, 0.0f, 0.0f);
    float radius = d % 2 == 0 ? 0.3f : 0.12f;
    addStats(sum, runDyntopoRemesh(*m, center, radius, p, 77u + uint32_t(d)));
  }
  uint64_t h = hashResult(*m, sum);
  if (out) {
    *out = sum;
  }
  alloc::Delete(m);
  return h;
}

static uint64_t runPatch(const DynTopoParams &p, int dabs, DynTopoStats *out = nullptr)
{
  Mesh *m = makeBumpyPatch();
  DynTopoStats sum;
  for (int d = 0; d < dabs; d++) {
    addStats(sum, runDyntopoRemesh(*m, float3(0.0f), 0.3f, p, 91u + uint32_t(d)));
  }
  uint64_t h = hashResult(*m, sum);
  if (out) {
    *out = sum;
  }
  alloc::Delete(m);
  return h;
}

/** Hashes recorded from the engine before pinching existed (plan Phase 0a). With pinch_thin off,
 * both fixtures must still reproduce them exactly. */
static constexpr uint64_t kDumbbellBaseline = 0x22663566c97de5fbull;
static constexpr uint64_t kPatchBaseline = 0x852ed511b40866dcull;

static void gatePinchOffIdentity()
{
  DynTopoStats sd, sp;
  uint64_t hd = runDumbbell(dumbbellParams(), 4, &sd);
  uint64_t hp = runPatch(patchParams(), 4, &sp);
  printf("dumbbell: hash=0x%016llxull splits=%d collapses=%d flips=%d\n",
         (unsigned long long)hd,
         sd.splits,
         sd.collapses,
         sd.flips);
  printf("patch:    hash=0x%016llxull splits=%d collapses=%d flips=%d\n",
         (unsigned long long)hp,
         sp.splits,
         sp.collapses,
         sp.flips);
  test_assert(hd == kDumbbellBaseline);
  test_assert(hp == kPatchBaseline);
}

static void gateFixtures()
{
  Mesh *m = makeDumbbell();
  test_assert(closedManifold(*m, "dumbbell"));
  test_assert(signedVolume(*m) > 0.0f);
  alloc::Delete(m);

  m = makeBumpyPatch();
  test_assert(m->validateAndRepair() == 0);
  alloc::Delete(m);
}

static Mesh *makeTet(float s = 0.01f)
{
  Mesh *m = alloc::New<Mesh>("test_dyntopo_pinch tet");
  int v[4] = {m->make_vertex(float3(0.0f)),
              m->make_vertex(float3(s, 0.0f, 0.0f)),
              m->make_vertex(float3(0.0f, s, 0.0f)),
              m->make_vertex(float3(0.0f, 0.0f, s))};
  int faces[4][3] = {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {0, 3, 2}};
  for (auto &f : faces) {
    int t[3] = {v[f[0]], v[f[1]], v[f[2]]};
    m->make_face(std::span<int>(t, 3));
  }
  return m;
}

static void gateTetRefused()
{
  Mesh *m = makeTet();
  test_assert(closedManifold(*m, "tet"));
  test_assert(signedVolume(*m) > 0.0f);
  Vector<int> edges;
  for (int e : m->e) {
    edges.append(e);
  }
  for (int e : edges) {
    EdgeCollapseResult res;
    bool ok = bool(collapseEdge(*m, e, std::nullopt, 0.5f, &res, nullptr, true));
    test_assert(!ok);
    test_assert(res.refusal == CollapseRefusal::Tet);
  }
  test_assert(m->v.count == 4 && m->e.count == 6 && m->f.count == 4);
  test_assert(closedManifold(*m, "tet-after"));
  alloc::Delete(m);
}

/** Builds a capped 3-around prism tube of five rings along x. Ring k is vertices 3k .. 3k + 2. */
static Mesh *makePrism()
{
  Vector<RevRing> rings;
  for (int i = 0; i < 5; i++) {
    rings.append({-0.2f + 0.1f * float(i), 0.05f, 3});
  }
  return makeRevolution(rings, -0.3f, 0.3f);
}

static int weightsAudit(Mesh &m)
{
  DeformPool *pool = m.deformPoolOrNull();
  if (!pool) {
    return 0;
  }
  Vector<WeightSlot> roots;
  findVertWeights(m, "weights").collectRoots(roots);
  return pool->auditRefcounts(span<const WeightSlot>(roots.data(), roots.size()));
}

static void gatePinchPrism()
{
  Mesh *m = makePrism();
  test_assert(closedManifold(*m, "prism"));
  const int ring[3] = {6, 7, 8};

  m->v.attrs.ensure(AttrType::FLOAT, "userdata", true);
  auto *ud = m->v.attrs.find_attribute(AttrType::FLOAT, "userdata").get_data<float>();
  WeightsRef w = ensureVertWeights(*m, "weights");
  DeformWeight dw[1] = {{2, 0.5f}};
  for (int v : ring) {
    ud->materialize(v);
    (*ud)[v] = 10.0f + float(v);
    w.setRun(v, span<const DeformWeight>(dw, 1));
  }
  const int eab = m->find_edge(ring[0], ring[1]);
  int spoke = ELEM_NONE;
  for (int e : m->e_of_v(ring[0])) {
    int o = m->e.vs[e][0] == ring[0] ? m->e.vs[e][1] : m->e.vs[e][0];
    if (o >= 9 && o < 12) {
      spoke = e; // toward ring 3
      break;
    }
  }
  test_assert(spoke != ELEM_NONE);
  bnd::setEdgeFlag(m, bnd::EDGE_SEAM, eab, true);
  bnd::setEdgeFlag(m, bnd::EDGE_SEAM, spoke, true);

  int V0 = m->v.count, E0 = m->e.count, F0 = m->f.count;
  PinchResult res;
  test_assert(pinchSeparatingTriangle(*m, ring[0], ring[1], ring[2], &res));
  test_assert(res.refusal == PinchRefusal::None);
  test_assert(m->v.count == V0 + 3 && m->e.count == E0 + 3 && m->f.count == F0 + 2);
  test_assert(closedManifold(*m, "prism-pinched"));
  test_assert(signedVolume(*m) > 0.0f);

  Vector<Component> comps = components(*m);
  test_assert(comps.size() == 2);
  for (const Component &cp : comps) {
    test_assert(cp.euler() == 2);
  }

  for (int i = 0; i < 3; i++) {
    int orig = ring[i], cp = res.copies[i];
    test_assert(ud->safe_get(cp) == 10.0f + float(orig));
    test_assert(w.slot(cp) == w.slot(orig));
    test_assert((m->v.co[cp] - m->v.co[orig]).length() == 0.0f);
  }
  test_assert(weightsAudit(*m) == 0);

  BoolAttrView *seam = bnd::findBoolEdgeView(m, bnd::EDGE_SEAM);
  int twin = m->find_edge(res.copies[0], res.copies[1]);
  test_assert(twin != ELEM_NONE);
  test_assert((*seam)[eab] && (*seam)[twin]);
  test_assert(!m->e.freemap[spoke] && (*seam)[spoke]);
  int s0 = m->e.vs[spoke][0], s1 = m->e.vs[spoke][1];
  test_assert(s0 == ring[0] || s1 == ring[0] || s0 == res.copies[0] || s1 == res.copies[0]);
  alloc::Delete(m);
}

static void gatePinchRefusals()
{
  // A ring that is a face: the first pole-fan triangle.
  {
    Mesh *m = makePrism();
    int f = *m->f.begin();
    int c0 = m->l.c[m->f.l[f]];
    int va = m->c.v[c0], vb = m->c.v[m->c.next[c0]], vc = m->c.v[m->c.next[m->c.next[c0]]];
    int V0 = m->v.count, F0 = m->f.count;
    PinchResult res;
    test_assert(!pinchSeparatingTriangle(*m, va, vb, vc, &res));
    test_assert(res.refusal == PinchRefusal::IsFace);
    test_assert(m->v.count == V0 && m->f.count == F0);
    alloc::Delete(m);
  }
  // A ring through the patch boundary: the poked corner triangle grid[0], grid[1], grid[18].
  {
    Mesh *m = makeBumpyPatch();
    int V0 = m->v.count, F0 = m->f.count;
    PinchResult res;
    test_assert(!pinchSeparatingTriangle(*m, 0, 1, 18, &res));
    test_assert(res.refusal == PinchRefusal::NonManifold);
    test_assert(m->v.count == V0 && m->f.count == F0);
    alloc::Delete(m);
  }
  // A ring edge carrying a third face.
  {
    Mesh *m = makePrism();
    int flap = m->make_vertex(float3(0.0f, 0.3f, 0.0f));
    int t[3] = {6, 7, flap};
    m->make_face(std::span<int>(t, 3));
    int V0 = m->v.count, F0 = m->f.count;
    PinchResult res;
    test_assert(!pinchSeparatingTriangle(*m, 6, 7, 8, &res));
    test_assert(res.refusal == PinchRefusal::NonManifold);
    test_assert(m->v.count == V0 && m->f.count == F0);
    alloc::Delete(m);
  }
}

static void gatePinchHandle()
{
  Mesh *m = makeThinTorus();
  test_assert(closedManifold(*m, "torus"));
  test_assert(signedVolume(*m) > 0.0f);
  Vector<Component> before = components(*m);
  test_assert(before.size() == 1 && before[0].euler() == 0);

  PinchResult res;
  test_assert(pinchSeparatingTriangle(*m, 0, 1, 2, &res));
  test_assert(closedManifold(*m, "torus-pinched"));
  test_assert(signedVolume(*m) > 0.0f);
  Vector<Component> after = components(*m);
  test_assert(after.size() == 1 && after[0].euler() == 2);
  alloc::Delete(m);
}

/** Counts 3-cycles that are not faces and whose edges are all shorter than `len`, inside the
 * sphere (center, radius). These are the thin rings the pinch exists to remove. */
static int thinRings(Mesh &m, float3 center, float radius, float len)
{
  int n = 0;
  for (int e : m.e) {
    int a = std::min(m.e.vs[e][0], m.e.vs[e][1]), b = std::max(m.e.vs[e][0], m.e.vs[e][1]);
    if ((m.v.co[a] - center).length() > radius ||
        (m.v.co[a] - m.v.co[b]).length() >= len)
    {
      continue;
    }
    for (int e2 : m.e_of_v(a)) {
      int c = m.e.vs[e2][0] == a ? m.e.vs[e2][1] : m.e.vs[e2][0];
      if (c <= b) {
        continue;
      }
      int ebc = m.find_edge(b, c);
      if (ebc == ELEM_NONE || (m.v.co[a] - m.v.co[c]).length() >= len ||
          (m.v.co[b] - m.v.co[c]).length() >= len)
      {
        continue;
      }
      bool face = false;
      int c0 = m.e.c[e], cc = c0;
      do {
        face |= m.c.v[m.c.next[m.c.next[cc]]] == c;
        cc = m.c.radial_next[cc];
      } while (cc != c0);
      n += !face;
    }
  }
  return n;
}

static void gateDumbbellString(DynTopoRegion region)
{
  const char *tag = region == DynTopoRegion::Sphere ? "dumbbell-sphere" : "dumbbell-graded";
  Mesh *m = makeDumbbell();
  DynTopoParams p = dumbbellParams();
  p.region = region;
  p.pinch_thin = true;
  const int before = thinRings(*m, float3(0.0f), 0.3f, p.l_min);
  test_assert(before > 0);
  DynTopoStats sum;
  bool stalled = false;
  for (int d = 0; d < 6; d++) {
    DynTopoStats st = runDyntopoRemesh(*m, float3(0.0f), 0.3f, p, 300u + uint32_t(d));
    sum.pinches += st.pinches;
    sum.culled_faces += st.culled_faces;
    sum.trivial_dissolves += st.trivial_dissolves;
    sum.collapses += st.collapses;
    stalled |= st.stalled;
  }
  const int after = thinRings(*m, float3(0.0f), 0.3f, p.l_min);
  Vector<Component> comps = components(*m);
  printf("%s: thin rings %d -> %d, pinches=%d culled_faces=%d trivial=%d collapses=%d "
         "components=%d\n",
         tag,
         before,
         after,
         sum.pinches,
         sum.culled_faces,
         sum.trivial_dissolves,
         sum.collapses,
         int(comps.size()));
  test_assert(closedManifold(*m, tag));
  test_assert(sum.pinches > 0);
  test_assert(sum.culled_faces > 0);
  test_assert(after == 0);
  test_assert(comps.size() == 2);
  test_assert(!stalled);
  alloc::Delete(m);
}

static void gateTrivialSide()
{
  Mesh *m = makeBumpyPatch();
  int spikeV = ELEM_NONE;
  for (int v : m->v) {
    if (m->v.co[v][2] > 0.2f) {
      spikeV = v;
    }
  }
  test_assert(spikeV != ELEM_NONE);
  DynTopoParams p = patchParams();
  p.pinch_thin = true;
  DynTopoStats sum;
  for (int d = 0; d < 4; d++) {
    DynTopoStats st = runDyntopoRemesh(*m, float3(0.0f), 0.3f, p, 91u + uint32_t(d));
    sum.pinches += st.pinches;
    sum.culled_faces += st.culled_faces;
    sum.trivial_dissolves += st.trivial_dissolves;
    sum.collapses += st.collapses;
  }
  printf("patch-pinch: pinches=%d culled_faces=%d trivial=%d collapses=%d\n",
         sum.pinches,
         sum.culled_faces,
         sum.trivial_dissolves,
         sum.collapses);
  test_assert(m->validateAndRepair() == 0);
  test_assert(sum.pinches == 0);
  test_assert(sum.culled_faces == 0);
  test_assert(sum.trivial_dissolves > 0);
  test_assert(components(*m).size() == 1);
  // The spike's spokes are longer than the split threshold, so it is left standing.
  test_assert(!m->v.freemap[spikeV] && m->v.co[spikeV][2] > 0.29f);
  alloc::Delete(m);
}

/** Orients every triangle of a closed mesh so its normal points away from the x axis. The first
 * triangle is oriented along `cap_dir0` and the last along `cap_dir1`. Used for fixtures whose
 * winding is easier to fix afterwards than to get right while building. */
static Mesh *makeOrientedTris(const Vector<float3> &pts, const Vector<int> &tris, float3 cap_dir0,
                              float3 cap_dir1)
{
  Mesh *m = alloc::New<Mesh>("test_dyntopo_pinch oriented");
  Vector<int> ids;
  for (const float3 &p : pts) {
    ids.append(m->make_vertex(p));
  }
  int nt = int(tris.size()) / 3;
  for (int t = 0; t < nt; t++) {
    int v[3] = {ids[tris[t * 3]], ids[tris[t * 3 + 1]], ids[tris[t * 3 + 2]]};
    float3 a = m->v.co[v[0]], b = m->v.co[v[1]], c = m->v.co[v[2]];
    float3 n = (b - a).cross(c - a);
    float3 cen = (a + b + c) * (1.0f / 3.0f);
    float3 out = t == 0 ? cap_dir0 : t == nt - 1 ? cap_dir1 : float3(0.0f, cen[1], cen[2]);
    if (n.dot(out) < 0.0f) {
      std::swap(v[1], v[2]);
    }
    m->make_face(std::span<int>(v, 3));
  }
  return m;
}

/** Builds a Boerdijk-Coxeter helix: a 3-around tube whose rings twist, so the edge (i, i+1)
 * shares four neighbours between its endpoints, two of which close separating 3-rings. */
static Mesh *makeHelixTube(int n = 30, float r = 0.004f, float h = 0.004f)
{
  const float theta = std::acos(-2.0f / 3.0f);
  Vector<float3> pts;
  for (int i = 0; i < n; i++) {
    float a = float(i) * theta;
    pts.append(float3(float(i) * h, r * std::cos(a), r * std::sin(a)));
  }
  Vector<int> tris;
  auto add = [&](int a, int b, int c) {
    tris.append(a);
    tris.append(b);
    tris.append(c);
  };
  add(0, 1, 2);
  for (int i = 0; i + 3 < n; i++) {
    add(i, i + 1, i + 3);
    add(i, i + 2, i + 3);
  }
  add(n - 3, n - 2, n - 1);
  return makeOrientedTris(pts, tris, float3(-1.0f, 0.0f, 0.0f), float3(1.0f, 0.0f, 0.0f));
}

static void gateHelixRing()
{
  Mesh *m = makeHelixTube();
  test_assert(closedManifold(*m, "helix"));
  test_assert(signedVolume(*m) > 0.0f);
  const int i = 12;
  int e = m->find_edge(i, i + 1);
  EdgeCollapseResult res;
  test_assert(!bool(collapseEdge(*m, e, std::nullopt, 0.5f, &res, nullptr, true)));
  test_assert(res.refusal == CollapseRefusal::Link);
  test_assert(res.link_extra.size() == 2);
  // Both extras close a separating ring; the operator accepts either.
  for (int extra : res.link_extra) {
    Mesh *copy = makeHelixTube();
    PinchResult pres;
    test_assert(pinchSeparatingTriangle(*copy, i, i + 1, extra, &pres));
    test_assert(closedManifold(*copy, "helix-pinched"));
    test_assert(components(*copy).size() == 2);
    alloc::Delete(copy);
  }
  alloc::Delete(m);
}

/** A floating tetrahedron inside the dab: every collapse of it is refused as a tet, and the tet
 * refusal hands it to the cull. */
static void gateFloatingTetCulled()
{
  Mesh *m = makeTet(0.005f);
  DynTopoParams p = dumbbellParams();
  p.pinch_thin = true;
  DynTopoStats st = runDyntopoRemesh(*m, float3(0.0f), 0.3f, p, 500u);
  test_assert(st.culled_faces == 4);
  test_assert(m->f.count == 0 && m->v.count == 0 && m->e.count == 0);
  alloc::Delete(m);

  // With pinching off the tet is left alone, as before.
  m = makeTet(0.005f);
  p.pinch_thin = false;
  st = runDyntopoRemesh(*m, float3(0.0f), 0.3f, p, 500u);
  test_assert(m->f.count == 4);
  alloc::Delete(m);
}

/** Runs pinching over necks that start 6 or 8 around. The fold guard in collapseEdge refuses most
 * of the collapses that would take such a neck down to 3 around, so only the spots that do reach
 * 3 around are cut (plan: Contingency). This gate checks that the mesh stays a valid closed
 * manifold throughout, and that at least one cut happens. */
static void gateWideNeck(int neck_n)
{
  Mesh *m = makeDumbbell(0.004f, 0.04f, neck_n);
  test_assert(closedManifold(*m, "wide-neck"));
  DynTopoParams p = dumbbellParams();
  p.pinch_thin = true;
  DynTopoStats sum;
  for (int d = 0; d < 8; d++) {
    DynTopoStats st = runDyntopoRemesh(*m, float3(0.0f), 0.3f, p, 700u + uint32_t(d));
    sum.pinches += st.pinches;
    sum.culled_faces += st.culled_faces;
    sum.collapses += st.collapses;
  }
  Vector<Component> comps = components(*m);
  printf("neck %d-around: pinches=%d culled_faces=%d collapses=%d components=%d\n",
         neck_n,
         sum.pinches,
         sum.culled_faces,
         sum.collapses,
         int(comps.size()));
  test_assert(closedManifold(*m, "wide-neck-after"));
  test_assert(sum.pinches > 0);
  alloc::Delete(m);
}

/** A strand whose every edge carries a seam is pinned by feature preservation: no cut. */
static void gateFeatureRefusal()
{
  Mesh *m = makeDumbbell();
  for (int e : m->e) {
    float3 a = m->v.co[m->e.vs[e][0]], b = m->v.co[m->e.vs[e][1]];
    auto onNeck = [](float3 v) {
      return std::abs(v[0]) < 0.29f && float2(v[1], v[2]).length() < 0.01f;
    };
    if (onNeck(a) && onNeck(b)) {
      bnd::setEdgeFlag(m, bnd::EDGE_SEAM, e, true);
    }
  }
  bnd::recomputeDirty(m);
  DynTopoParams p = dumbbellParams();
  p.pinch_thin = true;
  p.preserve_features = true;
  DynTopoStats st = runDyntopoRemesh(*m, float3(0.0f), 0.3f, p, 9u);
  test_assert(st.pinches == 0);
  test_assert(thinRings(*m, float3(0.0f), 0.3f, p.l_min) > 0);
  test_assert(closedManifold(*m, "seamed-neck"));
  alloc::Delete(m);
}

/** Records every live vert position and every live face's verts, keyed by index, so a state can
 * be compared after undo or redo. */
struct MeshSnapshot {
  Vector<int> verts;
  Vector<float3> co;
  Vector<int> faces;
  Vector<int> face_verts;

  static MeshSnapshot take(Mesh &m)
  {
    MeshSnapshot s;
    for (int v : m.v) {
      s.verts.append(v);
      s.co.append(m.v.co[v]);
    }
    for (int f : m.f) {
      s.faces.append(f);
      int first = int(s.face_verts.size());
      int c0 = m.l.c[m.f.l[f]], c = c0;
      do {
        s.face_verts.append(m.c.v[c]);
        c = m.c.next[c];
      } while (c != c0);
      // Undo may restore a face starting from a different corner, so compare vert sets.
      std::sort(s.face_verts.begin() + first, s.face_verts.end());
    }
    return s;
  }

  bool operator==(const MeshSnapshot &b) const
  {
    if (verts.size() != b.verts.size() || faces.size() != b.faces.size() ||
        face_verts.size() != b.face_verts.size())
    {
      return false;
    }
    for (int i = 0; i < int(verts.size()); i++) {
      if (verts[i] != b.verts[i] || (co[i] - b.co[i]).length() > 1e-6f) {
        return false;
      }
    }
    for (int i = 0; i < int(faces.size()); i++) {
      if (faces[i] != b.faces[i]) {
        return false;
      }
    }
    for (int i = 0; i < int(face_verts.size()); i++) {
      if (face_verts[i] != b.face_verts[i]) {
        return false;
      }
    }
    return true;
  }
};

/** Chains the meshlog callbacks ahead of the tree's, in the order brush_executor uses. */
static mesh::MeshCallbacks combineCallbacks(mesh::MeshCallbacks *ml, mesh::MeshCallbacks *sp)
{
  mesh::MeshCallbacks cb = *ml;
  auto fc = cb.onFaceCreate, sfc = sp->onFaceCreate;
  cb.onFaceCreate = [fc, sfc](int f) {
    fc(f);
    sfc(f);
  };
  auto fk = cb.onFaceKill, sfk = sp->onFaceKill;
  cb.onFaceKill = [fk, sfk](int f) {
    fk(f);
    sfk(f);
  };
  auto vk = cb.onVertKill, svk = sp->onVertKill;
  cb.onVertKill = [vk, svk](int v) {
    vk(v);
    svk(v);
  };
  auto fch = cb.onFaceChange, sfch = sp->onFaceChange;
  cb.onFaceChange = [fch, sfch](int f) {
    fch(f);
    sfch(f);
  };
  auto ck = cb.onCornerKill, sck = sp->onCornerKill;
  cb.onCornerKill = [ck, sck](int c) {
    ck(c);
    sck(c);
  };
  return cb;
}

/** Pinches the dumbbell through the meshlog and a live spatial tree, one undo step per dab. The
 * tree must own every live face after each dab, and after every undo and redo. Undoing every step
 * must restore the original mesh exactly, including the culled pieces, and redoing them must
 * reproduce the pinched mesh. */
static void gatePinchUndo()
{
  using sculptcore::test::validateOwnership;
  Mesh *m = makeDumbbell();
  auto *tree = alloc::New<spatial::SpatialTree>("pinch undo tree", m);
  tree->leaf_limit = 64;
  tree->buildAll();
  test_assert(validateOwnership(tree, m, "pinch-build") == m->f.count);

  meshlog::MeshLog log;
  log.setActiveMesh(m);
  mesh::MeshCallbacks cb = combineCallbacks(log.callbacks(), tree->getSpatialCallbacks());

  DynTopoParams p = dumbbellParams();
  p.pinch_thin = true;
  const MeshSnapshot before = MeshSnapshot::take(*m);
  const int dabs = 6;
  DynTopoStats sum;
  for (int d = 0; d < dabs; d++) {
    log.beginStep(true);
    DynTopoStats st = runDyntopoRemesh(*m, float3(0.0f), 0.3f, p, 300u + uint32_t(d), &cb);
    log.endStep();
    sum.pinches += st.pinches;
    sum.culled_faces += st.culled_faces;
    tree->applyDeferredNodeSplit();
    tree->updateQueries();
    test_assert(validateOwnership(tree, m, "pinch-dab") == m->f.count);
  }
  const MeshSnapshot after = MeshSnapshot::take(*m);
  printf("pinch-undo: pinches=%d culled_faces=%d faces %d -> %d\n",
         sum.pinches,
         sum.culled_faces,
         int(before.faces.size()),
         int(after.faces.size()));
  test_assert(sum.pinches > 0 && sum.culled_faces > 0);

  for (int d = 0; d < dabs; d++) {
    log.undo(m, tree);
    tree->applyDeferredNodeSplit();
    tree->updateQueries();
    test_assert(validateOwnership(tree, m, "pinch-undo") == m->f.count);
  }
  test_assert(MeshSnapshot::take(*m) == before);
  test_assert(closedManifold(*m, "pinch-undone"));

  for (int d = 0; d < dabs; d++) {
    log.redo(m, tree);
    tree->applyDeferredNodeSplit();
    tree->updateQueries();
    test_assert(validateOwnership(tree, m, "pinch-redo") == m->f.count);
  }
  test_assert(MeshSnapshot::take(*m) == after);
  test_assert(closedManifold(*m, "pinch-redone"));

  alloc::Delete(tree);
  alloc::Delete(m);

  // A culled tet this small has faces under the tree's zero-area cutoff. Undo must re-own them
  // anyway, since none of them has an owned neighbour to anchor to.
  m = makeTet(0.0003f);
  tree = alloc::New<spatial::SpatialTree>("pinch undo tet tree", m);
  tree->buildAll();
  test_assert(validateOwnership(tree, m, "tet-build") == 4);
  meshlog::MeshLog tetLog;
  tetLog.setActiveMesh(m);
  cb = combineCallbacks(tetLog.callbacks(), tree->getSpatialCallbacks());
  tetLog.beginStep(true);
  DynTopoStats st = runDyntopoRemesh(*m, float3(0.0f), 0.3f, p, 501u, &cb);
  tetLog.endStep();
  test_assert(st.culled_faces == 4 && m->f.count == 0);
  tetLog.undo(m, tree);
  tree->applyDeferredNodeSplit();
  test_assert(m->f.count == 4);
  test_assert(validateOwnership(tree, m, "tet-undo") == 4);
  alloc::Delete(tree);
  alloc::Delete(m);
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  gateFixtures();
  gatePinchOffIdentity();
  gateTetRefused();
  gatePinchPrism();
  gatePinchRefusals();
  gatePinchHandle();
  gateDumbbellString(DynTopoRegion::Sphere);
  gateDumbbellString(DynTopoRegion::GradedRecursive);
  gateTrivialSide();
  gateHelixRing();
  gateFloatingTetCulled();
  gateWideNeck(6);
  gateWideNeck(8);
  gateFeatureRefusal();
  gatePinchUndo();

  return test_end();
}
