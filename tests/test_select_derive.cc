#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"
#include "mesh/ops/subdivide.h"
#include "mesh/utils/select_derive.h"

#include <cstdio>

test_init;

#define TASSERT(expr)                                                                    \
  do {                                                                                   \
    if (!(expr)) {                                                                       \
      retval = 1;                                                                        \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                  \
      fflush(stderr);                                                                    \
    }                                                                                    \
  } while (0)

using namespace sculptcore;
using namespace sculptcore::mesh;
using litestl::math::float3;
using litestl::util::Set;
using litestl::util::Vector;

namespace {

/* 2x1 quad strip: verts 0..5, faces f0 (v0 v1 v4 v3), f1 (v1 v2 v5 v4).
 *
 *   3 --- 4 --- 5
 *   | f0  | f1  |
 *   0 --- 1 --- 2
 */
struct Strip {
  int v[6];
  int f[2];
};

Strip make_strip(Mesh &m)
{
  Strip s;
  s.v[0] = m.make_vertex(float3(0, 0, 0));
  s.v[1] = m.make_vertex(float3(1, 0, 0));
  s.v[2] = m.make_vertex(float3(2, 0, 0));
  s.v[3] = m.make_vertex(float3(0, 1, 0));
  s.v[4] = m.make_vertex(float3(1, 1, 0));
  s.v[5] = m.make_vertex(float3(2, 1, 0));
  {
    int q0[4] = {s.v[0], s.v[1], s.v[4], s.v[3]};
    s.f[0] = m.make_face(std::span<int>(q0, 4));
  }
  {
    int q1[4] = {s.v[1], s.v[2], s.v[5], s.v[4]};
    s.f[1] = m.make_face(std::span<int>(q1, 4));
  }
  return s;
}

void clear_selection(Mesh &m)
{
  auto *vs = m.v.select.get_data();
  auto *es = m.e.select.get_data();
  auto *fs = m.f.select.get_data();
  for (int v : m.v) {
    vs->set(v, false);
  }
  for (int e : m.e) {
    es->set(e, false);
  }
  for (int f : m.f) {
    fs->set(f, false);
  }
}

int meshEdgeCount(Mesh &m)
{
  int n = 0;
  for (int e : m.e) {
    (void)e;
    n++;
  }
  return n;
}

} // namespace

int main()
{
  /* derive rules over a 2-quad strip */
  {
    Mesh m;
    Strip s = make_strip(m);
    auto *vs = m.v.select.get_data();
    auto *fs = m.f.select.get_data();

    /* All 4 corners of f0 vert-selected -> f0 derived (All), f1 only under Any. */
    clear_selection(m);
    vs->set(s.v[0], true);
    vs->set(s.v[1], true);
    vs->set(s.v[4], true);
    vs->set(s.v[3], true);

    Set<int> faces = deriveFaceSelection(m, DeriveRule::All);
    TASSERT(faces.size() == 1);
    TASSERT(faces.contains(s.f[0]));

    Set<int> facesAny = deriveFaceSelection(m, DeriveRule::Any);
    TASSERT(facesAny.size() == 2); /* f1 touches v1/v4 */
    TASSERT(facesAny.contains(s.f[1]));

    /* Edge derivation (All): both endpoints selected -> f0's 4 edges + nothing
     * else (v1-v4 is shared, both selected, so it counts once). */
    Set<int> edges = deriveEdgeSelection(m, DeriveRule::All);
    TASSERT(edges.size() == 4);

    /* Vert derivation from a face selection. */
    clear_selection(m);
    fs->set(s.f[1], true);
    Set<int> verts = deriveVertSelection(m);
    TASSERT(verts.size() == 4);
    TASSERT(verts.contains(s.v[1]) && verts.contains(s.v[2]) && verts.contains(s.v[5]) &&
            verts.contains(s.v[4]));

    /* Any-rule edge derivation from the face selection: f1's edges. */
    Set<int> edgesAny = deriveEdgeSelection(m, DeriveRule::Any);
    TASSERT(edgesAny.size() == 4);
  }

  /* resolve: prefer-op-domain vs union */
  {
    Mesh m;
    Strip s = make_strip(m);
    auto *vs = m.v.select.get_data();
    auto *fs = m.f.select.get_data();

    /* Explicit f1 + a full vert-selection of f0. */
    clear_selection(m);
    fs->set(s.f[1], true);
    vs->set(s.v[0], true);
    vs->set(s.v[1], true);
    vs->set(s.v[4], true);
    vs->set(s.v[3], true);

    Set<int> prefer = resolveFaceSelection(m, true);
    TASSERT(prefer.size() == 1);
    TASSERT(prefer.contains(s.f[1])); /* explicit wins outright */

    Set<int> merged = resolveFaceSelection(m, false);
    TASSERT(merged.size() == 2); /* union of explicit + derived */

    /* Empty face domain -> derivation fills it regardless of the flag. */
    clear_selection(m);
    vs->set(s.v[0], true);
    vs->set(s.v[1], true);
    vs->set(s.v[4], true);
    vs->set(s.v[3], true);
    Set<int> derivedOnly = resolveFaceSelection(m, true);
    TASSERT(derivedOnly.size() == 1);
    TASSERT(derivedOnly.contains(s.f[0]));
  }

  /* subdivide consumes a vert-only selection through the resolve seam */
  {
    Mesh m;
    Strip s = make_strip(m);
    auto *vs = m.v.select.get_data();

    clear_selection(m);
    vs->set(s.v[0], true);
    vs->set(s.v[1], true);
    vs->set(s.v[4], true);
    vs->set(s.v[3], true);

    int edgesBefore = meshEdgeCount(m);
    Vector<int> outVerts;
    ops::subdivideEdges(m, nullptr, 1, outVerts, true);
    TASSERT(outVerts.size() == 4);           /* one cut vert per f0 edge */
    TASSERT(meshEdgeCount(m) > edgesBefore); /* topology actually changed */
  }

  /* Linking mesh pulls in static-init allocations that aren't tagged
   * PermanentGuard; skip test_end()'s leak check like test_meshlog_topo. */
  return retval;
}
