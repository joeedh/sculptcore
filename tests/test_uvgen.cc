// Wave 7: simple UV generation from seams. Two coplanar quads sharing edge
// v1-v4. With no seam they're one connected chart; seaming the shared edge
// splits them into two. In both cases every packed per-corner UV must land in
// [0,1].
#include "test_util.h"

#include "mesh/attribute.h"
#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "mesh/uvgen.h"

#include <cstdio>
#include <span>

test_init;

using namespace sculptcore::mesh;
using namespace litestl::math;
namespace bnd = sculptcore::mesh::boundary;

static void buildTwoQuads(Mesh &m, int &v1o, int &v4o)
{
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
  m.make_face(std::span<int>(fa, 4));
  m.make_face(std::span<int>(fb, 4));
  v1o = v1;
  v4o = v4;
}

static bool uvsInUnit(Mesh &m, const char *name)
{
  if (!m.c.attrs.has(AttrType::FLOAT2, name)) return false;
  AttrRef ref = m.c.attrs.find_attribute(AttrType::FLOAT2, name);
  auto *uv = ref.get_data<float2>();
  for (int c = 0; c < m.c.count; c++) {
    float2 t = (*uv)[c];
    if (t[0] < -1e-4f || t[0] > 1.0001f || t[1] < -1e-4f || t[1] > 1.0001f) {
      return false;
    }
  }
  return true;
}

int main()
{
  {
    // No seam: the two quads form one connected chart.
    Mesh m;
    int v1, v4;
    buildTwoQuads(m, v1, v4);
    int charts = generateUVFromSeams(&m, "uv");
    fprintf(stderr, "uvgen no-seam charts=%d corners=%d\n", charts, m.c.count);
    test_assert(charts == 1);
    test_assert(uvsInUnit(m, "uv"));
  }
  {
    // Seam the shared edge: two charts.
    Mesh m;
    int v1, v4;
    buildTwoQuads(m, v1, v4);
    int eShared = m.find_edge(v1, v4);
    test_assert(eShared != ELEM_NONE);
    bnd::setEdgeFlag(&m, bnd::EDGE_SEAM, eShared, true);
    int charts = generateUVFromSeams(&m, "uv");
    fprintf(stderr, "uvgen seam charts=%d\n", charts);
    test_assert(charts == 2);
    test_assert(uvsInUnit(m, "uv"));
  }
  return test_end();
}
