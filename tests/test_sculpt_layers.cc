/* Sculpt-layer compositor units (displacementAndSubSurf plan, F1 gate):
 * composition math (DELTA layers, weight/enable/frozen semantics), the
 * LayerEditScope incremental bracket, settings serialization (mesh format v3),
 * and layer/position consistency under an attribute-interpolating edge split. */
#include "test_util.h"

#include "displace/compositor.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/mesh_serialize.h"
#include "mesh/utils/edge_split.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>
#include <sstream>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace litestl;
using namespace litestl::math;
using litestl::util::Vector;

static bool near3(const float3 &a, const float3 &b, float eps = 1e-5f)
{
  return std::fabs(a[0] - b[0]) < eps && std::fabs(a[1] - b[1]) < eps &&
         std::fabs(a[2] - b[2]) < eps;
}

/* n×n grid of triangulated quads on z=0. */
static void buildGrid(Mesh &m, int n)
{
  Vector<int> grid;
  grid.resize(n * n);
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      float fx = float(x) / float(n - 1) - 0.5f;
      float fy = float(y) / float(n - 1) - 0.5f;
      grid[y * n + x] = m.make_vertex(float3(fx, fy, 0.0f));
    }
  }
  for (int y = 0; y < n - 1; y++) {
    for (int x = 0; x < n - 1; x++) {
      int a = grid[y * n + x], b = grid[y * n + x + 1];
      int c = grid[(y + 1) * n + x + 1], d = grid[(y + 1) * n + x];
      int t0[3] = {a, b, c}, t1[3] = {a, c, d};
      m.make_face(std::span<int>(t0, 3));
      m.make_face(std::span<int>(t1, 3));
    }
  }
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  Mesh *m = alloc::New<Mesh>("test mesh");
  buildGrid(*m, 8);

  Vector<float3> orig;
  orig.resize(m->v.count);
  for (int v : m->v) {
    orig[v] = m->v.co[v];
  }

  // --- category + settings rows ---
  int la = m->addSculptLayerNamed("layerA");
  int lb = m->addSculptLayerNamed("layerB");
  test_assert(la == 0 && lb == 1);
  test_assert(m->sculptLayerCount() == 2);
  test_assert(m->findSculptLayer(string("layerA")) == 0);
  test_assert(m->sculptLayerAttrIndex(0) >= 0);
  {
    AttrRef ref = m->v.attrs.find_attribute(AttrType::FLOAT3, string("layerA"));
    test_assert(ref.exists());
    test_assert(bool(ref.use & AttrUse::SCULPT_LAYER));
  }

  Vector<int> allVerts;
  for (int v : m->v) {
    allVerts.append(v);
  }
  std::span<const int> vspan(allVerts.data(), allVerts.size());

  auto layerData = [&](const char *name) {
    AttrRef ref = m->v.attrs.find_attribute(AttrType::FLOAT3, string(name));
    return ref.get_data<float3>();
  };

  // --- LayerEditScope folds delta writes into evaluated positions ---
  const float3 dA(0.0f, 0.0f, 1.0f);
  {
    displace::LayerEditScope scope;
    test_assert(scope.begin(*m, string("layerA"), vspan));
    AttrData<float3> *da = layerData("layerA");
    for (int v : m->v) {
      (*da)[v] = dA;
    }
    scope.end();
  }
  for (int v : m->v) {
    test_assert(near3(m->v.co[v], orig[v] + dA));
  }

  // Half-weight layer B: contribution scales by the weight at edit time.
  displace::setLayerWeight(*m, lb, 0.5f);
  const float3 dB(1.0f, 0.0f, 0.0f);
  {
    displace::LayerEditScope scope;
    test_assert(scope.begin(*m, string("layerB"), vspan));
    AttrData<float3> *db = layerData("layerB");
    for (int v : m->v) {
      (*db)[v] = dB;
    }
    scope.end();
  }
  const float3 both = dA + dB * 0.5f;
  for (int v : m->v) {
    test_assert(near3(m->v.co[v], orig[v] + both));
  }

  // --- enable/weight round-trips ---
  displace::setLayerEnabled(*m, la, false);
  for (int v : m->v) {
    test_assert(near3(m->v.co[v], orig[v] + dB * 0.5f));
  }
  displace::setLayerEnabled(*m, la, true);
  for (int v : m->v) {
    test_assert(near3(m->v.co[v], orig[v] + both));
  }
  displace::setLayerWeight(*m, lb, 1.0f);
  for (int v : m->v) {
    test_assert(near3(m->v.co[v], orig[v] + dA + dB));
  }
  displace::setLayerWeight(*m, lb, 0.5f);

  // --- frozen: writes are reverted, positions untouched ---
  displace::setLayerFrozen(*m, la, true);
  {
    displace::LayerEditScope scope;
    test_assert(scope.begin(*m, string("layerA"), vspan));
    AttrData<float3> *da = layerData("layerA");
    for (int v : m->v) {
      (*da)[v] = float3(9.0f, 9.0f, 9.0f);
    }
    scope.end();
  }
  {
    AttrData<float3> *da = layerData("layerA");
    for (int v : m->v) {
      test_assert(near3((*da)[v], dA));
      test_assert(near3(m->v.co[v], orig[v] + both));
    }
  }
  displace::setLayerFrozen(*m, la, false);

  // --- serialization round-trip (v3 settings table + tagged column) ---
  {
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    test_assert(serial::writeMesh(*m, ss));
    Mesh *m2 = alloc::New<Mesh>("test mesh 2");
    test_assert(serial::readMesh(*m2, ss));
    test_assert(m2->sculptLayerCount() == 2);
    test_assert(m2->findSculptLayer(string("layerA")) == 0);
    test_assert(m2->findSculptLayer(string("layerB")) == 1);
    test_assert(std::fabs(m2->sculptLayers[1].weight - 0.5f) < 1e-6f);
    test_assert(m2->sculptLayers[0].enabled && m2->sculptLayers[1].enabled);
    AttrRef ref = m2->v.attrs.find_attribute(AttrType::FLOAT3, string("layerA"));
    test_assert(ref.exists());
    test_assert(bool(ref.use & AttrUse::SCULPT_LAYER));
    AttrData<float3> *da = ref.get_data<float3>();
    int checked = 0;
    for (int v : m2->v) {
      test_assert(near3((*da)[v], dA));
      checked++;
    }
    test_assert(checked == int(allVerts.size()));
    alloc::Delete(m2);
  }

  // --- attribute interp keeps co == implicit base + Σ w·d across a split ---
  {
    int edge = *m->e.begin();
    int v0 = m->e.vs[edge][0], v1 = m->e.vs[edge][1];
    float3 exCo = (m->v.co[v0] + m->v.co[v1]) * 0.5f;

    EdgeSplitResult res;
    splitEdge(*m, edge, &res);
    int nv = res.new_vert;
    test_assert(nv >= 0);

    test_assert(near3(m->v.co[nv], exCo));
    AttrData<float3> *da = layerData("layerA");
    AttrData<float3> *db = layerData("layerB");
    test_assert(near3(da->safe_get(nv), dA));
    test_assert(near3(db->safe_get(nv), dB));
    // Composition invariant on the new vert: co − Σ w·d == lerp of the
    // endpoints' implicit bases (all columns lerped by the same interp).
    float3 base0 = m->v.co[v0] - dA - dB * 0.5f;
    float3 base1 = m->v.co[v1] - dA - dB * 0.5f;
    float3 baseN = m->v.co[nv] - da->safe_get(nv) - db->safe_get(nv) * 0.5f;
    test_assert(near3(baseN, (base0 + base1) * 0.5f));
  }

  // --- removeLayer subtracts the contribution and drops the column ---
  displace::removeLayer(*m, lb);
  test_assert(m->sculptLayerCount() == 1);
  test_assert(!m->v.attrs.find_attribute(AttrType::FLOAT3, string("layerB")).exists());

  alloc::Delete(m);
  return retval;
}
