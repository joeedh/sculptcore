#include "test_util.h"

#include "mesh/attribute.h"
#include "mesh/mesh.h"

#include <cstdio>

test_init;

int main()
{
  using namespace sculptcore;
  using namespace sculptcore::mesh;

  {
    Mesh mesh(5, 0, 0, 0);

    int v1 = 1;

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

    int e = mesh.make_edge(0, 1);

    Mesh::EdgeProxy p(&mesh, e);
    printf("%d\n", p.v1().v);
  }

  return test_end();
}
