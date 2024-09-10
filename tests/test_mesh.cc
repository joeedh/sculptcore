#include "test_util.h"

#include "mesh/attribute.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"

#include "litestl/math/vector.h"

#include <cstdio>

test_init;

int main()
{
  using namespace sculptcore;
  using namespace litestl::math;
  using namespace litestl;
  using namespace sculptcore::mesh;

  {
    Mesh *cube = createCube(256);

    alloc::Delete<Mesh>(cube);


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

    int f = mesh.make_face(util::Vector({v1, v2, v3}));

    printf("face:\n");
    FaceProxy face(&mesh, f);
    for (auto list : face.lists()) {
      for (auto corner : list) {
        printf("%d\n", corner.v().i);
      }
    }

    EdgeProxy edge1(&mesh, e1);
    VertProxy vert1(&mesh, v1);

    vert1.e() = edge1;

    CornerProxy c(&mesh, 0);
    c = 0;
    c.next() = 2; 

    printf("edges:\n");
    auto iter = vert1.edges();
    auto end = iter.end();
    iter = iter.begin();

    while (iter != end) {
      printf("  %d\n", (*iter).i);
      ++iter;
    }

    printf("edges:\n");
    for (EdgeProxy e : vert1.edges()) {
      printf("  %d\n", e.i);
    }
  }

  return test_end();
}
