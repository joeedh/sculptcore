#include "mesh/attribute.h"
#include "mesh/mesh.h"
#include "test_util.h"
#include "util/alloc.h"
#include "util/vector.h"
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
      mesh.v.select.set(v1, true);
    }
    
    mesh.v.co[v1][0] = 1.0f;
    mesh.v.co[v1][1] = -1.0f;
  }

  return test_end();
}
