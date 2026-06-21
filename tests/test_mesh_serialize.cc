#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"
#include "mesh/attribute.h"
#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "mesh/mesh_serialize.h"

#include <span>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

test_init;

#define TASSERT(expr)                                                                    \
  do {                                                                                   \
    if (!(expr)) {                                                                        \
      retval = 1;                                                                        \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                  \
      fflush(stderr);                                                                    \
    }                                                                                    \
  } while (0)

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace litestl;
using namespace litestl::util;
using litestl::math::float3;

namespace {

/* --- Mesh integrity (mirrors test_mesh_reorder.cc::validateMesh) --- */
bool validateMesh(Mesh &m, const char *tag)
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
      int next = m.e.disk[ec][side * 2 + 1];
      int prev = m.e.disk[ec][side * 2];
      int side_n = m.e.vs[next][0] == vi ? 0 : 1;
      int side_p = m.e.vs[prev][0] == vi ? 0 : 1;
      if (m.e.disk[next][side_n * 2] != ec || m.e.disk[prev][side_p * 2 + 1] != ec) {
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
        if (!((ev0 == v_here && ev1 == v_next) || (ev1 == v_here && ev0 == v_next))) {
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

uint64_t posHash(const float3 &p)
{
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < 3; i++) {
    uint32_t bits;
    float f = p[i];
    std::memcpy(&bits, &f, sizeof(bits));
    h ^= uint64_t(bits);
    h *= 1099511628211ull;
  }
  return h;
}

Vector<int64_t> geomEdgeSignature(Mesh &m)
{
  Vector<int64_t> sig;
  for (int ei : m.e) {
    uint64_t a = posHash(m.v.co[m.e.vs[ei][0]]);
    uint64_t b = posHash(m.v.co[m.e.vs[ei][1]]);
    uint64_t lo = a < b ? a : b;
    uint64_t hi = a < b ? b : a;
    sig.append(int64_t(lo * 1099511628211ull ^ hi));
  }
  std::sort(sig.begin(), sig.end());
  return sig;
}

bool sigEqual(const Vector<int64_t> &a, const Vector<int64_t> &b)
{
  if (a.size() != b.size()) return false;
  for (int i = 0; i < int(a.size()); i++) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

/* Deterministic, position-derived attribute values: because positions
 * round-trip bit-exactly, recomputing the expected value from the loaded
 * position reproduces the stored bits exactly — no epsilon needed. */
float vFloat(const float3 &c) { return c[0] * 131.0f + c[1] * 17.0f + c[2] * 3.0f; }
int vInt(const float3 &c) { return int(c[0] * 1000.0f) + int(c[1] * 100.0f) * 7; }
float3 vF3(const float3 &c) { return float3(c[1], c[0], c[0] + c[1]); }
bool vBool(const float3 &c) { return c[0] > 0.5f; }
bool vSelect(const float3 &c) { return (c[0] + c[1]) > 1.0f; }

/* Sum of a face's vertex positions (no division → exact float reproduction). */
float3 faceVertSum(Mesh &m, int fi)
{
  float3 sum(0.0f);
  int li = m.f.l[fi];
  while (li != ELEM_NONE) {
    int c0 = m.l.c[li];
    int cc = c0;
    do {
      sum = sum + m.v.co[m.c.v[cc]];
      cc = m.c.next[cc];
    } while (cc != c0);
    li = m.l.next[li];
  }
  return sum;
}

int fInt(const float3 &s) { return int(s[0] * 128.0f) ^ int(s[1] * 64.0f); }

void build_grid(Mesh &m, int N)
{
  Vector<int> v;
  v.resize((N + 1) * (N + 1));
  auto vat = [&](int i, int j) -> int & { return v[j * (N + 1) + i]; };

  for (int j = 0; j <= N; j++) {
    for (int i = 0; i <= N; i++) {
      vat(i, j) = m.make_vertex(float3(float(i) / float(N), float(j) / float(N), 0.0f));
    }
  }
  for (int j = 0; j < N; j++) {
    for (int i = 0; i < N; i++) {
      int v0 = vat(i, j), v1 = vat(i + 1, j), v2 = vat(i + 1, j + 1), v3 = vat(i, j + 1);
      if (m.find_edge(v0, v1) == ELEM_NONE) m.make_edge(v0, v1);
      if (m.find_edge(v1, v2) == ELEM_NONE) m.make_edge(v1, v2);
      if (m.find_edge(v2, v3) == ELEM_NONE) m.make_edge(v2, v3);
      if (m.find_edge(v3, v0) == ELEM_NONE) m.make_edge(v3, v0);
      int verts[4] = {v0, v1, v2, v3};
      m.make_face(std::span<int>(verts, 4));
    }
  }
}

/* Add custom + sparse attrs and write deterministic values to every live elem. */
void populate_attrs(Mesh &m)
{
  m.v.attrs.ensure(AttrType::FLOAT, "vfloat");
  m.v.attrs.ensure(AttrType::INT, "vint");
  m.v.attrs.ensure(AttrType::FLOAT3, "vf3");
  m.v.attrs.ensure(AttrType::FLOAT4, "vf4");
  m.v.attrs.ensure(AttrType::BOOL, "vbool");
  m.v.attrs.ensure(AttrType::INT, "sparse_unset"); /* left at page default */
  m.f.attrs.ensure(AttrType::INT, "fint");

  auto vfloat = m.v.attrs.find_attribute(AttrType::FLOAT, "vfloat");
  auto vint = m.v.attrs.find_attribute(AttrType::INT, "vint");
  auto vf3 = m.v.attrs.find_attribute(AttrType::FLOAT3, "vf3");
  auto vf4 = m.v.attrs.find_attribute(AttrType::FLOAT4, "vf4");
  auto vbool = m.v.attrs.find_attribute(AttrType::BOOL, "vbool");
  auto fint = m.f.attrs.find_attribute(AttrType::INT, "fint");

  for (int vi : m.v) {
    float3 c = m.v.co[vi];
    vfloat.get_data<float>()->materialize(vi);
    (*vfloat.get_data<float>())[vi] = vFloat(c);
    vint.get_data<int>()->materialize(vi);
    (*vint.get_data<int>())[vi] = vInt(c);
    vf3.get_data<float3>()->materialize(vi);
    (*vf3.get_data<float3>())[vi] = vF3(c);
    vf4.get_data<math::float4>()->materialize(vi);
    (*vf4.get_data<math::float4>())[vi] =
        math::float4(c[0], c[1], c[2], vFloat(c));
    static_cast<BoolAttrView *>(vbool.data)->set(vi, vBool(c));
    static_cast<BoolAttrView *>(
        m.v.attrs.find_attribute(AttrType::BOOL, "select").data)
        ->set(vi, vSelect(c));
  }

  for (int fi : m.f) {
    fint.get_data<int>()->materialize(fi);
    (*fint.get_data<int>())[fi] = fInt(faceVertSum(m, fi));
  }
}

/* Every index in [0,count) is live and every index in [count,capacity) free —
 * the dense layout serialization guarantees. */
bool isDense(ElemData &ed, const char *tag, const char *dom)
{
  int cap = int(ed.capacity());
  for (int i = 0; i < cap; i++) {
    bool free = ed.freemap[i];
    bool shouldBeFree = i >= ed.count;
    if (free != shouldBeFree) {
      fprintf(stderr, "[%s] %s slot %d free=%d but count=%d (not dense)\n", tag, dom, i,
              int(free), ed.count);
      return false;
    }
  }
  return true;
}

bool roundTrip(Mesh &src, Mesh &dst, const char *tag)
{
  std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
  if (!serial::writeMesh(src, ss)) {
    fprintf(stderr, "[%s] writeMesh failed\n", tag);
    return false;
  }
  std::string data = ss.str();
  std::stringstream rs(data, std::ios::in | std::ios::out | std::ios::binary);
  if (!serial::readMesh(dst, rs)) {
    fprintf(stderr, "[%s] readMesh failed\n", tag);
    return false;
  }
  return true;
}

void check_attrs(Mesh &m, const char *tag)
{
  auto vfloat = m.v.attrs.find_attribute(AttrType::FLOAT, "vfloat");
  auto vint = m.v.attrs.find_attribute(AttrType::INT, "vint");
  auto vf3 = m.v.attrs.find_attribute(AttrType::FLOAT3, "vf3");
  auto vf4 = m.v.attrs.find_attribute(AttrType::FLOAT4, "vf4");
  auto vbool = m.v.attrs.find_attribute(AttrType::BOOL, "vbool");
  auto vsel = m.v.attrs.find_attribute(AttrType::BOOL, "select");
  auto sparse = m.v.attrs.find_attribute(AttrType::INT, "sparse_unset");
  auto fint = m.f.attrs.find_attribute(AttrType::INT, "fint");

  TASSERT(vfloat.exists() && vint.exists() && vf3.exists() && vbool.exists());
  TASSERT(vf4.exists());
  TASSERT(vsel.exists() && sparse.exists() && fint.exists());

  for (int vi : m.v) {
    float3 c = m.v.co[vi];
    TASSERT(vfloat.get_data<float>()->safe_get(vi) == vFloat(c));
    TASSERT(vint.get_data<int>()->safe_get(vi) == vInt(c));
    float3 e3 = vF3(c);
    float3 g3 = vf3.get_data<float3>()->safe_get(vi);
    TASSERT(g3[0] == e3[0] && g3[1] == e3[1] && g3[2] == e3[2]);
    math::float4 e4(c[0], c[1], c[2], vFloat(c));
    math::float4 g4 = vf4.get_data<math::float4>()->safe_get(vi);
    TASSERT(g4[0] == e4[0] && g4[1] == e4[1] && g4[2] == e4[2] && g4[3] == e4[3]);
    TASSERT(static_cast<BoolAttrView *>(vbool.data)->get(vi) == vBool(c));
    TASSERT(static_cast<BoolAttrView *>(vsel.data)->get(vi) == vSelect(c));
    TASSERT(sparse.get_data<int>()->safe_get(vi) == 0);
  }

  for (int fi : m.f) {
    TASSERT(fint.get_data<int>()->safe_get(fi) == fInt(faceVertSum(m, fi)));
  }
}

void test_full_roundtrip(int N)
{
  char tag[64];
  snprintf(tag, sizeof(tag), "full-N%d", N);

  Mesh m;
  build_grid(m, N);

  /* Isolated verts (no edges) plus a couple killed to make vertex holes. */
  int iso[4];
  for (int k = 0; k < 4; k++) {
    iso[k] = m.make_vertex(float3(2.0f + k * 0.5f, -1.0f, 0.5f * k));
  }
  m.kill_vertex(iso[1]);
  m.kill_vertex(iso[3]);

  /* Face + edge holes: killing an edge cascades to its faces. */
  int someFace = *m.f.begin();
  m.kill_face(someFace);
  int someEdge = *m.e.begin();
  m.kill_edge(someEdge);

  TASSERT(validateMesh(m, tag));
  populate_attrs(m);

  int vc = m.v.count, ec = m.e.count, cc = m.c.count, lc = m.l.count, fc = m.f.count;
  Vector<int64_t> origSig = geomEdgeSignature(m);

  Mesh m2;
  if (!roundTrip(m, m2, tag)) {
    retval = 1;
    return;
  }

  TASSERT(m2.v.count == vc);
  TASSERT(m2.e.count == ec);
  TASSERT(m2.c.count == cc);
  TASSERT(m2.l.count == lc);
  TASSERT(m2.f.count == fc);

  TASSERT(validateMesh(m2, tag));
  TASSERT(sigEqual(origSig, geomEdgeSignature(m2)));

  TASSERT(isDense(m2.v, tag, "vert"));
  TASSERT(isDense(m2.e, tag, "edge"));
  TASSERT(isDense(m2.c, tag, "corner"));
  TASSERT(isDense(m2.l, tag, "list"));
  TASSERT(isDense(m2.f, tag, "face"));

  check_attrs(m2, tag);
}

/* A frozen-topology mesh is the post-sculpt-stroke state: the live TOPO link
 * columns (.edge.vs.disk, .vert.e, …) are freed. writeMesh must thaw first;
 * before that fix this case hung reading the freed disk column. Custom attrs
 * (incl. the FLOAT4 from populate_attrs) must survive the freeze→serialize. */
void test_frozen_roundtrip(int N)
{
  char tag[64];
  snprintf(tag, sizeof(tag), "frozen-N%d", N);

  Mesh m;
  build_grid(m, N);
  populate_attrs(m);

  int vc = m.v.count, ec = m.e.count, cc = m.c.count, lc = m.l.count, fc = m.f.count;
  Vector<int64_t> origSig = geomEdgeSignature(m);

  m.freezeTopo();
  TASSERT(m.topo_frozen);

  Mesh m2;
  if (!roundTrip(m, m2, tag)) {
    retval = 1;
    return;
  }

  TASSERT(m2.v.count == vc && m2.e.count == ec && m2.c.count == cc);
  TASSERT(m2.l.count == lc && m2.f.count == fc);
  TASSERT(validateMesh(m2, tag));
  TASSERT(sigEqual(origSig, geomEdgeSignature(m2)));
  check_attrs(m2, tag);
}

void test_empty()
{
  Mesh m, m2;
  if (!roundTrip(m, m2, "empty")) {
    retval = 1;
    return;
  }
  TASSERT(m2.v.count == 0 && m2.e.count == 0 && m2.f.count == 0);
  TASSERT(validateMesh(m2, "empty"));
}

void test_verts_only()
{
  Mesh m;
  for (int i = 0; i < 32; i++) {
    m.make_vertex(float3(float(i), float(i) * 0.5f, 1.0f));
  }
  int e0 = m.make_edge(0, 1);
  (void)e0;
  m.make_edge(2, 3);

  Mesh m2;
  if (!roundTrip(m, m2, "verts-only")) {
    retval = 1;
    return;
  }
  TASSERT(m2.v.count == m.v.count);
  TASSERT(m2.e.count == m.e.count);
  TASSERT(m2.f.count == 0);
  TASSERT(validateMesh(m2, "verts-only"));
}

/* B2 (audit): an attribute's category (AttrUse) must survive save/load — the
 * Wave 2b active-attr bridge depends on it. Before the fix only name/type/flag
 * were serialized, so categories reverted to NONE on load. */
void test_attr_use_roundtrip()
{
  Mesh m;
  build_grid(m, 4);
  AttrRef &cref = m.v.attrs.ensure(AttrType::FLOAT4, "color", /*materialize=*/true);
  cref.use = AttrUse::COLOR;
  AttrRef &gref = m.f.attrs.ensure(AttrType::INT, "group", /*materialize=*/true);
  gref.use = AttrUse::POLYGROUP;

  Mesh m2;
  if (!roundTrip(m, m2, "attr-use")) {
    retval = 1;
    return;
  }
  AttrRef lc = m2.v.attrs.find_attribute(AttrType::FLOAT4, "color");
  AttrRef lg = m2.f.attrs.find_attribute(AttrType::INT, "group");
  TASSERT(lc.exists() && lg.exists());
  TASSERT(int(lc.use) == int(AttrUse::COLOR));
  TASSERT(int(lg.use) == int(AttrUse::POLYGROUP));
}

/* Regression: the derived boundary classification (EDGE_POLYGROUP / VERT_CLASS)
 * is TEMP and not serialized, and boundaryDirty defaults false — so a loaded
 * mesh keeps the source flags (group/seam/sharp) but no recomputed classes.
 * readMesh must markAllDirty so the overlay / smooth brush rebuild it on first
 * use; without it face-set borders are invisible until the user repaints. */
void test_boundary_roundtrip()
{
  namespace bnd = sculptcore::mesh::boundary;
  Mesh m;
  int v0 = m.make_vertex(float3(0, 0, 0));
  int v1 = m.make_vertex(float3(1, 0, 0));
  int v2 = m.make_vertex(float3(2, 0, 0));
  int v3 = m.make_vertex(float3(0, 1, 0));
  int v4 = m.make_vertex(float3(1, 1, 0));
  int v5 = m.make_vertex(float3(2, 1, 0));
  auto edge = [&](int a, int b) {
    if (m.find_edge(a, b) == ELEM_NONE) m.make_edge(a, b);
  };
  edge(v0, v1); edge(v1, v4); edge(v4, v3); edge(v3, v0);
  edge(v1, v2); edge(v2, v5); edge(v5, v4);
  int fa[4] = {v0, v1, v4, v3};
  int fb[4] = {v1, v2, v5, v4};
  int faceA = m.make_face(std::span<int>(fa, 4));
  int faceB = m.make_face(std::span<int>(fb, 4));
  AttrRef &gref = m.f.attrs.ensure(AttrType::INT, bnd::FACE_GROUP, /*materialize=*/true);
  AttrData<int> *g = gref.get_data<int>();
  (*g)[faceA] = 0;
  (*g)[faceB] = 1;

  Mesh m2;
  if (!roundTrip(m, m2, "boundary")) {
    retval = 1;
    return;
  }

  // The fix: a freshly loaded mesh must be boundary-dirty so derived state rebuilds.
  TASSERT(m2.boundaryDirty);

  bnd::recomputeDirty(&m2);
  int eShared = m2.find_edge(v1, v4);
  TASSERT(eShared != ELEM_NONE);
  TASSERT(bnd::edgeFlag(&m2, bnd::EDGE_POLYGROUP, eShared) == true);
  TASSERT((bnd::vertClass(&m2, v1) & bnd::BC_POLYGROUP) != 0);
  TASSERT((bnd::vertClass(&m2, v4) & bnd::BC_POLYGROUP) != 0);
  TASSERT((bnd::vertClass(&m2, v0) & bnd::BC_POLYGROUP) == 0);
}

/* T4 / C2 (audit): detachAttr parks a layer (data preserved) for undo and
 * reattachAttr restores it intact — the undo primitive behind RemoveAttrOp /
 * GenerateUVOp. */
void test_detach_reattach()
{
  Mesh m;
  build_grid(m, 4);
  AttrRef &uref = m.v.attrs.ensure(AttrType::INT, "udata", /*materialize=*/true);
  for (int vi : m.v) {
    uref.get_data<int>()->materialize(vi);
    (*uref.get_data<int>())[vi] = vInt(m.v.co[vi]);
  }
  // Find its index in the vertex group.
  int idx = -1;
  for (int i = 0; i < int(m.v.attrs.attrs.size()); i++) {
    if (m.v.attrs.attrs[i].name == util::string("udata")) idx = i;
  }
  TASSERT(idx >= 0);

  int stashId = m.detachAttr(/*domain=*/1, idx);
  TASSERT(stashId >= 0);
  TASSERT(m.v.attrs.has(AttrType::INT, "udata") == false); // gone from the group

  int newIdx = m.reattachAttr(stashId);
  TASSERT(newIdx >= 0);
  TASSERT(m.v.attrs.has(AttrType::INT, "udata") == true); // back in the group
  AttrRef back = m.v.attrs.find_attribute(AttrType::INT, "udata");
  for (int vi : m.v) {
    TASSERT(back.get_data<int>()->safe_get(vi) == vInt(m.v.co[vi])); // data preserved
  }
}

/* Crash-repro guard (immediateTODOs #11): some old .wproj files were saved before
 * the spatial node-ownership attrs were flagged TEMP|NOINTERP|NOCOPY, so they
 * persisted on disk with stale flags and stale leaf ids. On load, dyntopo's
 * interpAttrs (skips only TOPO|NOINTERP) would interpolate a parent's leaf id
 * onto new geometry → a stale index that mis-partitions a freshly built tree and
 * crashes. readMesh must re-assert the mandatory flags so such a file loads inert
 * and TEMP-dropped from the next save. We simulate the bad save by creating the
 * attrs with NO flags (plain ensure), which also lets writeMesh emit them (TEMP
 * would otherwise skip them). */
void test_nonpersistent_flag_repair()
{
  const char *tag = "flag-repair";
  Mesh m;
  build_grid(m, 4);

  /* Stale spatial ownership attrs as a pre-flag writer left them: plain INT,
   * no TEMP/NOINTERP/NOCOPY, filled with out-of-range leaf ids. */
  AttrRef &vnode = m.v.attrs.ensure(AttrType::INT, ".spatial.v.node", true);
  for (int vi : m.v) {
    vnode.get_data<int>()->materialize(vi);
    (*vnode.get_data<int>())[vi] = 99999; /* nonexistent leaf */
  }
  AttrRef &fnode = m.f.attrs.ensure(AttrType::INT, ".spatial.f.node", true);
  for (int fi : m.f) {
    fnode.get_data<int>()->materialize(fi);
    (*fnode.get_data<int>())[fi] = 99999;
  }
  /* A derived boundary layer persisted without TEMP. */
  m.e.attrs.ensure(AttrType::BOOL, ".boundary.edge.polygroup", true);

  TASSERT(!bool(vnode.flag & AttrFlag::TEMP)); /* the bad-save precondition */

  Mesh m2;
  if (!roundTrip(m, m2, tag)) {
    retval = 1;
    return;
  }

  AttrRef lv = m2.v.attrs.find_attribute(AttrType::INT, ".spatial.v.node");
  AttrRef lf = m2.f.attrs.find_attribute(AttrType::INT, ".spatial.f.node");
  AttrRef lpg = m2.e.attrs.find_attribute(AttrType::BOOL, ".boundary.edge.polygroup");
  TASSERT(lv.exists() && lf.exists() && lpg.exists());

  /* The repair: mandatory flags re-asserted regardless of the file's stale flag. */
  for (AttrRef *r : {&lv, &lf}) {
    TASSERT(bool(r->flag & AttrFlag::TEMP));
    TASSERT(bool(r->flag & AttrFlag::NOINTERP)); /* the crash-preventing bit */
    TASSERT(bool(r->flag & AttrFlag::NOCOPY));
  }
  TASSERT(bool(lpg.flag & AttrFlag::TEMP));
  printf("  [%s] spatial node attrs re-flagged TEMP|NOINTERP|NOCOPY on load\n", tag);
}

} // namespace

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  for (int N : {1, 4, 8, 16}) {
    test_full_roundtrip(N);
  }
  test_frozen_roundtrip(8);
  test_empty();
  test_verts_only();
  test_attr_use_roundtrip();
  test_boundary_roundtrip();
  test_detach_reattach();
  test_nonpersistent_flag_repair();

  printf("mesh_serialize test done (retval=%d)\n", retval);
  return retval;
}
