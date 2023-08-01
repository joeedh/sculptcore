#include "test_util.h"

#include "mesh/attribute.h"
#include "mesh/mesh.h"

#include "math/vector.h"

#include <cstdio>

test_init;

int main()
{
  using namespace sculptcore;
  using namespace sculptcore::math;
  using namespace sculptcore::mesh;

  {
    Mesh mesh;

    int v1 = mesh.make_vertex(float3(0.0, 0.1, 0.2));
    int v2 = mesh.make_vertex(float3(0.1, 0.3, 0.4));
    int v3 = mesh.make_vertex(float3(0.7, 0.5, 0.3));

    mesh.v.select.set(v1, true);

    if (mesh.v.select[v1]) {
      mesh.v.select.set(v1, false);
    }
    
    test_assert(!mesh.v.select[v1]);

    mesh.v.co[v1][0] = 1.0f;
    mesh.v.co[v1][1] = -1.0f;

    for (int v : mesh.v) {
      mesh.v.co[v] += math::float3(1.0f);
    }

    int e1 = mesh.make_edge(v1, v2);
    int e2 = mesh.make_edge(v2, v3);
    int e3 = mesh.make_edge(v3, v1);

    //int f = mesh.make_face(util::Vector({v1, v2, v3}));
    EdgeProxy edge1(&mesh, e1);
    VertProxy vert1(&mesh, v1);

    vert1.e() = edge1;
    
    edge1 = vert1.e();

    printf("%d\n", edge1.v1().v);
  }

  return test_end();
}
