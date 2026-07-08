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

  // ================= V2: implicit active edit layer =================
  // All values below are small dyadic rationals, so every ± is exact in fp
  // and the gates can assert bit-exact equality (n=5 grid → x/4 coords).
  {
    Mesh *m3 = alloc::New<Mesh>("v2 mesh");
    buildGrid(*m3, 5);

    auto exact3 = [](const float3 &a, const float3 &b) {
      return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
    };
    auto saveCo = [&](Vector<float3> &out) {
      out.resize(m3->v.count);
      for (int v : m3->v) {
        out[v] = m3->v.co[v];
      }
    };
    auto col = [&](const char *name) -> AttrData<float3> * {
      AttrRef ref = m3->v.attrs.find_attribute(AttrType::FLOAT3, string(name));
      return ref.exists() ? ref.get_data<float3>() : nullptr;
    };
    auto hasRest = [&]() {
      return m3->v.attrs.find_attribute(AttrType::FLOAT3, string(SCULPT_LAYER_REST_ATTR))
          .exists();
    };

    Vector<float3> rest0;
    saveCo(rest0);

    // --- activation snapshots rest; sculpt-sim + fold derives the delta ---
    int lp = m3->addSculptLayerNamed("v2p");
    test_assert(displace::setActiveEditLayer(*m3, lp) == lp);
    test_assert(m3->activeEditLayer == lp && m3->sculptLayerEditTarget() == lp);
    test_assert(hasRest());

    const float3 sP(0.0f, 0.0f, 0.5f);
    for (int v : m3->v) {
      m3->v.co[v] += sP; // sculpt-sim: direct co write, no kernel involved
    }
    displace::foldActiveLayer(*m3);
    AttrData<float3> *dp = col("v2p");
    for (int v : m3->v) {
      test_assert(exact3(dp->safe_get(v), sP));
    }
    // fold idempotence: double fold == single fold, bit-exact
    displace::foldActiveLayer(*m3);
    for (int v : m3->v) {
      test_assert(exact3(dp->safe_get(v), sP));
    }

    // --- region fold vs whole-mesh fold equivalence ---
    Vector<int> sub;
    for (int v : m3->v) {
      if (int(sub.size()) < 5) {
        sub.append(v);
      }
    }
    const float3 sSub(0.125f, 0.0f, 0.0f);
    for (int v : sub) {
      m3->v.co[v] += sSub;
    }
    displace::foldActiveLayer(*m3, std::span<const int>(sub.data(), sub.size()));
    Vector<float3> snap;
    snap.resize(m3->v.count);
    for (int v : m3->v) {
      snap[v] = dp->safe_get(v);
    }
    displace::foldActiveLayer(*m3);
    for (int v : m3->v) {
      test_assert(exact3(dp->safe_get(v), snap[v]));
    }

    // --- deactivation folds + drops rest; weight 0 returns bit-exact rest ---
    test_assert(displace::setActiveEditLayer(*m3, -1) == -1);
    test_assert(m3->activeEditLayer == -1);
    test_assert(!hasRest());
    Vector<float3> at1;
    saveCo(at1);
    displace::setLayerWeight(*m3, lp, 0.0f);
    for (int v : m3->v) {
      test_assert(exact3(m3->v.co[v], rest0[v]));
    }
    displace::setLayerWeight(*m3, lp, 1.0f); // 1→0→1 round-trip is bit-stable
    for (int v : m3->v) {
      test_assert(exact3(m3->v.co[v], at1[v]));
    }

    // --- activating a half-weight layer with prior content pins weight 1 ---
    int lq = m3->addSculptLayerNamed("v2q");
    const float3 dQ(0.25f, 0.0f, 0.0f);
    {
      Vector<int> all;
      for (int v : m3->v) {
        all.append(v);
      }
      displace::LayerEditScope scope;
      test_assert(
          scope.begin(*m3, string("v2q"), std::span<const int>(all.data(), all.size())));
      AttrData<float3> *dq = col("v2q");
      for (int v : m3->v) {
        (*dq)[v] = dQ;
      }
      scope.end();
    }
    displace::setLayerWeight(*m3, lq, 0.5f);
    test_assert(displace::setActiveEditLayer(*m3, lq) == lq);
    test_assert(m3->sculptLayers[lq].weight == 1.0f);
    for (int v : m3->v) {
      test_assert(exact3(m3->v.co[v], at1[v] + dQ));
    }

    // --- mutating another layer while targeted mirrors into rest ---
    displace::setLayerWeight(*m3, lp, 0.5f); // lp is NOT the target
    for (int v : m3->v) {
      m3->v.co[v] += float3(0.0f, 0.25f, 0.0f); // more sculpting into lq
    }
    displace::setActiveEditLayer(*m3, -1);
    displace::setLayerWeight(*m3, lq, 0.0f);
    for (int v : m3->v) {
      // back to the ADJUSTED rest: rest0 + the re-weighted lp contribution
      test_assert(exact3(m3->v.co[v], rest0[v] + dp->safe_get(v) * 0.5f));
    }
    displace::setLayerWeight(*m3, lq, 1.0f);

    // --- frozen layers cannot be the target ---
    displace::setLayerFrozen(*m3, lp, true);
    test_assert(displace::setActiveEditLayer(*m3, lp) == -1);
    test_assert(m3->activeEditLayer == -1);
    displace::setLayerFrozen(*m3, lp, false);

    // --- activating a disabled layer enables it (weight pinned too) ---
    displace::setLayerEnabled(*m3, lp, false);
    Vector<float3> coNoP;
    saveCo(coNoP);
    test_assert(displace::setActiveEditLayer(*m3, lp) == lp);
    test_assert(m3->sculptLayerEnabled(lp) == 1);
    test_assert(m3->sculptLayers[lp].weight == 1.0f);
    for (int v : m3->v) {
      test_assert(exact3(m3->v.co[v], coNoP[v] + dp->safe_get(v)));
    }

    // --- mutating the target itself ends the edit first ---
    displace::setLayerWeight(*m3, lp, 0.25f);
    test_assert(m3->activeEditLayer == -1 && !hasRest());
    test_assert(m3->sculptLayers[lp].weight == 0.25f);
    test_assert(displace::setActiveEditLayer(*m3, lp) == lp);
    displace::setLayerFrozen(*m3, lp, true); // freezing the target clears it
    test_assert(m3->activeEditLayer == -1 && m3->sculptLayerFrozen(lp) == 1);
    displace::setLayerFrozen(*m3, lp, false);

    // --- removing a layer below the target shifts the target index ---
    test_assert(displace::setActiveEditLayer(*m3, lq) == lq);
    displace::removeLayer(*m3, lp);
    test_assert(m3->activeEditLayer == 0);
    test_assert(m3->sculptLayers[0].name == string("v2q"));
    test_assert(hasRest());

    // --- serialization folds the live target; rest never round-trips ---
    const float3 sTail(0.0f, 0.0f, 0.0625f);
    for (int v : m3->v) {
      m3->v.co[v] += sTail;
    }
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    test_assert(serial::writeMesh(*m3, ss));
    test_assert(m3->activeEditLayer == 0); // fold is undo/edit-transparent
    Mesh *m4 = alloc::New<Mesh>("v2 mesh rt");
    test_assert(serial::readMesh(*m4, ss));
    test_assert(m4->activeEditLayer == -1); // the edit target is runtime state
    test_assert(
        !m4->v.attrs.find_attribute(AttrType::FLOAT3, string(SCULPT_LAYER_REST_ATTR))
             .exists());
    AttrData<float3> *dq3 = col("v2q");
    AttrRef q4 = m4->v.attrs.find_attribute(AttrType::FLOAT3, string("v2q"));
    test_assert(q4.exists());
    AttrData<float3> *dq4 = q4.get_data<float3>();
    int nChecked = 0;
    for (int v : m4->v) {
      test_assert(exact3(dq4->safe_get(v), dq3->safe_get(v)));
      nChecked++;
    }
    test_assert(nChecked == int(m3->v.count));

    alloc::Delete(m4);
    alloc::Delete(m3);
  }

  return retval;
}
