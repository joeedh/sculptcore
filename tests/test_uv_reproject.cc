// UV slide-reprojection (uv_reproject.h). A planar grid with uv = (x, y) makes
// barycentric re-interpolation exact: after a tangential vertex move the
// reprojected UV must equal the affine map at the new position. Also covers:
// normal-only motion (UV unchanged), a UV seam (per-wedge interpolation within
// each chart), a mesh with no UV layer (no-op), and a simultaneous two-vertex
// (Jacobi) move.
#include "test_util.h"

#include "litestl/util/alloc.h"
#include "mesh/attribute.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "mesh/uv_reproject.h"

#include <cmath>
#include <cstdio>
#include <span>

test_init;

using namespace sculptcore::mesh;
using namespace litestl::math;
using litestl::util::Vector;

/** Add a FLOAT2 corner layer tagged UV with uv = vertex (x, y) + per-face
 * chart offset `chartOffsetX(f)` (0 for a single continuous chart). */
template <typename OffsetFn>
static AttrData<float2> *assignPlanarUVs(Mesh &m, OffsetFn chartOffsetX)
{
  AttrRef &ref = m.c.attrs.ensure(AttrType::FLOAT2, "uv", /*materialize=*/true);
  ref.use = AttrUse::UV;
  auto *uv = ref.get_data<float2>();
  for (int c : m.c) {
    const int f = m.l.f[m.c.l[c]];
    const float3 co = m.v.co[m.c.v[c]];
    uv->materialize(c);
    (*uv)[c] = float2(co[0] + chartOffsetX(f), co[1]);
  }
  return uv;
}

/** All corner UVs at vertex @p v for faces where chartOffsetX == @p off must
 * equal @p expect (within eps). Returns the number of corners checked. */
static int checkCornerUVs(
    Mesh &m, AttrData<float2> *uv, int v, float offX, float2 expect, float eps = 1e-4f)
{
  int checked = 0;
  for (int c : m.c) {
    if (m.c.v[c] != v) {
      continue;
    }
    float2 t = uv->safe_get(c);
    t[0] -= offX;
    if ((t - expect).lengthSqr() > eps * eps) {
      fprintf(stderr,
              "corner %d uv (%f, %f) != expected (%f, %f)\n",
              c,
              double(t[0] + offX),
              double(t[1]),
              double(expect[0] + offX),
              double(expect[1]));
      return -1;
    }
    checked++;
  }
  return checked;
}

static void buildTwoQuads(Mesh &m, int verts[6])
{
  // v3 -- v4 -- v5
  // |  A  |  B  |
  // v0 -- v1 -- v2
  const float3 cos[6] = {
      {0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 1, 0}, {1, 1, 0}, {2, 1, 0}};
  for (int i = 0; i < 6; i++) {
    verts[i] = m.make_vertex(cos[i]);
  }
  int fa[4] = {verts[0], verts[1], verts[4], verts[3]};
  int fb[4] = {verts[1], verts[2], verts[5], verts[4]};
  m.make_face(std::span<int>(fa, 4));
  m.make_face(std::span<int>(fb, 4));
}

int main()
{
  {
    // Interior tangential slide on a 3x3 planar grid: exact affine result.
    Mesh *m = makeGrid(3, 3, 2.0f);
    auto *uv = assignPlanarUVs(*m, [](int) { return 0.0f; });
    const int center = 4; // (i=1, j=1) -> (0, 0, 0)
    test_assert((m->v.co[center] - float3(0, 0, 0)).lengthSqr() < 1e-12f);

    const float3 oldP = m->v.co[center];
    m->v.co[center] = float3(0.25f, 0.1f, 0.0f);
    int changed = uvproj::reprojectVertUVs(
        m, std::span<const int>(&center, 1), std::span<const float3>(&oldP, 1));
    fprintf(stderr, "grid slide changed=%d\n", changed);
    test_assert(changed == 4); // the center vert has 4 corners
    test_assert(checkCornerUVs(*m, uv, center, 0.0f, float2(0.25f, 0.1f)) == 4);
    litestl::alloc::Delete<Mesh>(m);
  }
  {
    // Moving the vertex only along the normal leaves the closest point on the
    // old fan at the old position, so the UV must not change.
    Mesh *m = makeGrid(3, 3, 2.0f);
    auto *uv = assignPlanarUVs(*m, [](int) { return 0.0f; });
    const int center = 4;
    const float3 oldP = m->v.co[center];
    m->v.co[center] = float3(0.0f, 0.0f, 0.5f);
    uvproj::reprojectVertUVs(
        m, std::span<const int>(&center, 1), std::span<const float3>(&oldP, 1));
    test_assert(checkCornerUVs(*m, uv, center, 0.0f, float2(0.0f, 0.0f)) == 4);
    litestl::alloc::Delete<Mesh>(m);
  }
  {
    // UV seam between the two quads (chart B shifted +5 in u): the shared vert
    // has two wedges; each must re-interpolate within its own chart.
    Mesh m;
    int verts[6];
    buildTwoQuads(m, verts);
    auto *uv = assignPlanarUVs(m, [](int f) { return f == 0 ? 0.0f : 5.0f; });
    const int v4 = verts[4]; // (1, 1, 0), shared by both quads
    const float3 oldP = m.v.co[v4];
    m.v.co[v4] = float3(1.0f, 0.7f, 0.0f); // slide along the seam
    int changed = uvproj::reprojectVertUVs(
        &m, std::span<const int>(&v4, 1), std::span<const float3>(&oldP, 1));
    fprintf(stderr, "seam slide changed=%d\n", changed);
    test_assert(changed == 2); // one corner per chart
    // Check each chart's corner against its own offset.
    for (int c : m.c) {
      if (m.c.v[c] != v4) {
        continue;
      }
      const int f = m.l.f[m.c.l[c]];
      const float offX = f == 0 ? 0.0f : 5.0f;
      const float2 t = uv->safe_get(c);
      test_assert(std::fabs(t[0] - (1.0f + offX)) < 1e-4f);
      test_assert(std::fabs(t[1] - 0.7f) < 1e-4f);
    }
  }
  {
    // No UV layer: no-op, zero corners changed.
    Mesh m;
    int verts[6];
    buildTwoQuads(m, verts);
    const int v4 = verts[4];
    const float3 oldP = m.v.co[v4];
    m.v.co[v4] = float3(1.0f, 0.7f, 0.0f);
    test_assert(uvproj::reprojectVertUVs(&m,
                                         std::span<const int>(&v4, 1),
                                         std::span<const float3>(&oldP, 1)) == 0);
  }
  {
    // Simultaneous (Jacobi) move of two adjacent verts: each reprojects
    // against the other's OLD position, so the affine result stays exact.
    Mesh *m = makeGrid(3, 3, 2.0f);
    auto *uv = assignPlanarUVs(*m, [](int) { return 0.0f; });
    const int vs[2] = {4, 7}; // (0,0,0) and (1,0,0)
    test_assert((m->v.co[7] - float3(1, 0, 0)).lengthSqr() < 1e-12f);
    const float3 olds[2] = {m->v.co[4], m->v.co[7]};
    m->v.co[4] = float3(0.2f, 0.05f, 0.0f);
    m->v.co[7] = float3(0.9f, -0.1f, 0.0f);
    uvproj::reprojectVertUVs(
        m, std::span<const int>(vs, 2), std::span<const float3>(olds, 2));
    test_assert(checkCornerUVs(*m, uv, 4, 0.0f, float2(0.2f, 0.05f)) == 4);
    test_assert(checkCornerUVs(*m, uv, 7, 0.0f, float2(0.9f, -0.1f)) == 2);
    litestl::alloc::Delete<Mesh>(m);
  }
  return test_end();
}
