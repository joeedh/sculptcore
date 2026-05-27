#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"
#include "mesh/attribute.h"
#include "mesh/mesh.h"
#include "mesh/mesh_serialize.h"

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
  m.v.attrs.ensure(AttrType::BOOL, "vbool");
  m.v.attrs.ensure(AttrType::INT, "sparse_unset"); /* left at page default */
  m.f.attrs.ensure(AttrType::INT, "fint");

  auto vfloat = m.v.attrs.find_attribute(AttrType::FLOAT, "vfloat");
  auto vint = m.v.attrs.find_attribute(AttrType::INT, "vint");
  auto vf3 = m.v.attrs.find_attribute(AttrType::FLOAT3, "vf3");
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
  auto vbool = m.v.attrs.find_attribute(AttrType::BOOL, "vbool");
  auto vsel = m.v.attrs.find_attribute(AttrType::BOOL, "select");
  auto sparse = m.v.attrs.find_attribute(AttrType::INT, "sparse_unset");
  auto fint = m.f.attrs.find_attribute(AttrType::INT, "fint");

  TASSERT(vfloat.exists() && vint.exists() && vf3.exists() && vbool.exists());
  TASSERT(vsel.exists() && sparse.exists() && fint.exists());

  for (int vi : m.v) {
    float3 c = m.v.co[vi];
    TASSERT(vfloat.get_data<float>()->safe_get(vi) == vFloat(c));
    TASSERT(vint.get_data<int>()->safe_get(vi) == vInt(c));
    float3 e3 = vF3(c);
    float3 g3 = vf3.get_data<float3>()->safe_get(vi);
    TASSERT(g3[0] == e3[0] && g3[1] == e3[1] && g3[2] == e3[2]);
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

} // namespace

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  for (int N : {1, 4, 8, 16}) {
    test_full_roundtrip(N);
  }
  test_empty();
  test_verts_only();

  printf("mesh_serialize test done (retval=%d)\n", retval);
  return retval;
}
