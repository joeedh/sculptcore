/* VdmStore core (displacementAndSubSurf plan, V1 gate): texel write/sample
 * round-trips (bilinear correctness, zero outside allocation), magnitude-bound
 * pyramid vs brute force, the self-inverse tile-delta undo bracket, the lz4
 * container serialization round-trip, and the per-face bound export that
 * feeds F2's setFaceDisplacementBounds. */
#include "test_util.h"

#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "vdm/vdm_store.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace sculptcore::vdm;
using namespace litestl;
using namespace litestl::math;
using litestl::util::Vector;

static bool near3(const float3 &a, const float3 &b, float eps = 1e-6f)
{
  return std::fabs(a[0] - b[0]) < eps && std::fabs(a[1] - b[1]) < eps &&
         std::fabs(a[2] - b[2]) < eps;
}

/* Deterministic LCG (the cross_field.cc generator). */
static uint32_t lcg(uint32_t &s)
{
  s = s * 1664525u + 1013904223u;
  return s;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  VdmStoreParams params;
  params.tile_size = 16;
  params.resolution = 256;

  // --- write/sample round-trip + bilinear ---
  {
    VdmStore store(params);
    const float3 val(0.5f, -1.0f, 2.0f);
    store.writeTexel(10, 12, val);
    test_assert(store.tileCount() == 1);
    test_assert(near3(store.texel(10, 12), val));
    test_assert(near3(store.texel(11, 12), float3(0, 0, 0)));

    // Sampling exactly at the texel center returns the texel.
    float u = (10.0f + 0.5f) / 256.0f;
    float v = (12.0f + 0.5f) / 256.0f;
    test_assert(near3(store.sample(0, u, v), val));

    // Midpoint between two texel centers = their average.
    store.writeTexel(11, 12, float3(1.0f, 1.0f, 0.0f));
    float um = (11.0f) / 256.0f;
    float3 expect = (val + float3(1.0f, 1.0f, 0.0f)) * 0.5f;
    test_assert(near3(store.sample(0, um, v), expect));

    // Far outside any allocated tile: zero (and no allocation happened).
    int before = store.tileCount();
    test_assert(near3(store.sample(0, 0.9f, 0.9f), float3(0, 0, 0)));
    test_assert(store.tileCount() == before);

    // addTexel accumulates.
    store.addTexel(10, 12, val);
    test_assert(near3(store.texel(10, 12), val * 2.0f));
  }

  // --- bound pyramid vs brute force ---
  {
    VdmStore store(params);
    uint32_t seed = 12345u;
    float brute = 0.0f;
    for (int i = 0; i < 500; i++) {
      int x = int(lcg(seed) % 200u);
      int y = int(lcg(seed) % 200u);
      float3 d(float(lcg(seed) % 1000u) / 500.0f - 1.0f,
               float(lcg(seed) % 1000u) / 500.0f - 1.0f,
               float(lcg(seed) % 1000u) / 500.0f - 1.0f);
      store.writeTexel(x, y, d);
      // writeTexel overwrites, so track the *final* value per coord by
      // re-reading everything at the end instead of accumulating here.
    }
    // Brute force over every allocated texel.
    store.foreachTile([&](const VdmTile &t) {
      for (const float3 &d : t.texels) {
        float l = d.length();
        brute = l > brute ? l : brute;
      }
    });
    float pyr = store.maxBound();
    fprintf(stderr, "bounds: brute=%f pyramid=%f\n", brute, pyr);
    test_assert(std::fabs(brute - pyr) < 1e-6f);

    // Rect queries: the full store rect equals the global bound; a rect
    // covering only empty tiles is zero; conservativeness on sub-rects.
    test_assert(std::fabs(store.maxBoundInUvRect(0.0f, 0.0f, 1.0f, 1.0f) - pyr) < 1e-6f);
    test_assert(store.maxBoundInUvRect(0.9f, 0.9f, 0.99f, 0.99f) == 0.0f);
    float sub = store.maxBoundInUvRect(0.0f, 0.0f, 0.25f, 0.25f);
    test_assert(sub <= pyr + 1e-6f);
  }

  // --- tile-delta undo bracket (self-inverse swap) ---
  {
    VdmStore store(params);
    const float3 pre(1.0f, 0.0f, 0.0f);
    store.writeTexel(5, 5, pre); // pre-existing tile, outside the bracket

    store.beginDelta();
    store.writeTexel(5, 5, float3(9.0f, 9.0f, 9.0f)); // modify existing tile
    store.writeTexel(100, 100, float3(2.0f, 0.0f, 1.0f)); // creates a new tile
    VdmDelta *delta = store.endDelta();
    test_assert(delta != nullptr);
    test_assert(store.tileCount() == 2);

    // Undo: back to the pre-bracket state (new tile gone, old texel restored).
    store.applyDelta(*delta);
    test_assert(store.tileCount() == 1);
    test_assert(near3(store.texel(5, 5), pre));
    test_assert(near3(store.texel(100, 100), float3(0, 0, 0)));

    // Redo: the same blob restores the post-bracket state.
    store.applyDelta(*delta);
    test_assert(store.tileCount() == 2);
    test_assert(near3(store.texel(5, 5), float3(9.0f, 9.0f, 9.0f)));
    test_assert(near3(store.texel(100, 100), float3(2.0f, 0.0f, 1.0f)));

    // And undo again — a full round-trip.
    store.applyDelta(*delta);
    test_assert(store.tileCount() == 1);
    test_assert(near3(store.texel(5, 5), pre));
    alloc::Delete(delta);

    // An empty bracket yields no delta.
    store.beginDelta();
    test_assert(store.endDelta() == nullptr);
  }

  // --- serialization round-trip ---
  {
    VdmStore store(params);
    uint32_t seed = 999u;
    for (int i = 0; i < 300; i++) {
      int x = int(lcg(seed) % 300u);
      int y = int(lcg(seed) % 300u);
      store.writeTexel(
          x, y, float3(float(lcg(seed) % 100u), float(lcg(seed) % 100u), -1.0f));
    }
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    test_assert(store.write(ss));

    VdmStore loaded;
    test_assert(loaded.read(ss));
    test_assert(loaded.params.tile_size == params.tile_size);
    test_assert(loaded.params.resolution == params.resolution);
    test_assert(loaded.tileCount() == store.tileCount());
    int mismatches = 0;
    store.foreachTile([&](const VdmTile &t) {
      const VdmTile *lt = loaded.findTile(t.tx, t.ty);
      if (!lt) {
        mismatches++;
        return;
      }
      for (int i = 0; i < int(t.texels.size()); i++) {
        // Bit-exact: the container stores raw floats.
        if (std::memcmp(&t.texels[i], &lt->texels[i], sizeof(float3)) != 0) {
          mismatches++;
          return;
        }
      }
    });
    fprintf(stderr, "serialize: tiles=%d mismatches=%d\n", store.tileCount(), mismatches);
    test_assert(mismatches == 0);
    test_assert(std::fabs(loaded.maxBound() - store.maxBound()) < 1e-6f);
  }

  // --- per-face bound export (the F2 feed) ---
  {
    // Two quads: face A owns UV [0, 0.5]², face B owns [0.5, 1]×[0, 0.5] —
    // tile-aligned at tile_size 16 / resolution 256 (0.5 = 8 tiles).
    Mesh *m = alloc::New<Mesh>("vdm test mesh");
    int v0 = m->make_vertex(float3(0, 0, 0));
    int v1 = m->make_vertex(float3(1, 0, 0));
    int v2 = m->make_vertex(float3(1, 1, 0));
    int v3 = m->make_vertex(float3(0, 1, 0));
    int v4 = m->make_vertex(float3(2, 0, 0));
    int v5 = m->make_vertex(float3(2, 1, 0));
    int qa[4] = {v0, v1, v2, v3};
    int qb[4] = {v1, v4, v5, v2};
    int fa = m->make_face(std::span<int>(qa, 4));
    int fb = m->make_face(std::span<int>(qb, 4));

    AttrRef &uvRef = m->c.attrs.ensure(AttrType::FLOAT2, "uv", /*materialize=*/true);
    uvRef.use = AttrUse::UV;
    auto *uv = static_cast<AttrData<float2> *>(uvRef.data);
    auto setUv = [&](int f, float2 base) {
      float2 offs[4] = {{0.0f, 0.0f}, {0.5f, 0.0f}, {0.5f, 0.5f}, {0.0f, 0.5f}};
      int k = 0;
      FaceProxy face(m, f);
      for (auto list : face.lists()) {
        for (auto c : list) {
          (*uv)[c.i] = base + offs[k % 4];
          k++;
        }
      }
    };
    setUv(fa, float2(0.0f, 0.0f));
    setUv(fb, float2(0.5f, 0.0f));

    VdmStore store(params);
    // Write only inside face A's UV region ([0,0.5)² in texels: [0,128)²).
    store.writeTexel(40, 40, float3(0.0f, 0.0f, 3.0f));

    int faces[2] = {fa, fb};
    Vector<float> bounds;
    exportFaceBounds(store, *m, std::span<const int>(faces, 2), bounds);
    fprintf(stderr, "face bounds: A=%f B=%f\n", bounds[0], bounds[1]);
    test_assert(std::fabs(bounds[0] - 3.0f) < 1e-6f);
    test_assert(bounds[1] == 0.0f);

    alloc::Delete(m);
  }

  /* Skip test_end(): mesh attr name strings stay live in the alloc tracker
   * (mirrors the other mesh-using tests). */
  return retval;
}
