#include "litestl/util/alloc.h"
#include "litestl/util/rand.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"

using namespace litestl;

#include "mesh.h"

namespace sculptcore::mesh {
Mesh *createCube(int dimen, float size, float sphereFac)
{
  using namespace litestl::util;
  using namespace litestl::math;
  Random rnd;

  int totpoint = dimen * dimen * dimen * 6;

  Vector<int> grid;
  grid.resize(totpoint);
  memset(static_cast<void *>(grid.data()), 0, totpoint * sizeof(int));

  Mesh *m = alloc::New<Mesh>("Mesh Cube");

  int axes[6][4] = {
      {0, 1, 2, 0},         //
      {0, 1, 2, dimen - 1}, //
      {1, 2, 0, 0},         //
      {1, 2, 0, dimen - 1}, //
      {2, 0, 1, 0},         //
      {2, 0, 1, dimen - 1},
  };

  float idimen = 1.0f / float(std::max(dimen, 2) - 1);

  for (int i = 0; i < 6; i++) {
    for (int j = 0; j < dimen; j++) {
      for (int k = 0; k < dimen; k++) {
        int xyz[3];
        xyz[axes[i][0]] = j;
        xyz[axes[i][1]] = k;
        xyz[axes[i][2]] = axes[i][3];

        int x = xyz[0];
        int y = xyz[1];
        int z = xyz[2];

        int key = z * dimen * dimen + y * dimen + x;
        int vi;

        if (key >= grid.size() || key < 0) {
          printf("error!! %d (%d %d %d)\n", key, x, y, z);
          continue;
        }

        if (grid[key] == 0) {
          float3 co;
          co[0] = (idimen * float(x)) - 0.5f;
          co[1] = (idimen * float(y)) - 0.5f;
          co[2] = (idimen * float(z)) - 0.5f;

          float3 sphereCo = co.normalized() * 0.5f;
          co += (sphereCo - co) * sphereFac;

          co *= size;
          vi = m->make_vertex(co);
          grid[key] = vi + 1;
        }
      }
    }
  }

  Vector<int> vs;
  vs.resize(4);

  for (int i = 0; i < 6; i++) {
    for (int j = 0; j < dimen - 1; j++) {
      for (int k = 0; k < dimen - 1; k++) {
        int quad[4][2] = {
            {j, k},
            {j, k + 1},
            {j + 1, k + 1},
            {j + 1, k},
        };

        for (int l = 0; l < 4; l++) {
          int xyz[3];
          xyz[axes[i][0]] = quad[l][0];
          xyz[axes[i][1]] = quad[l][1];
          xyz[axes[i][2]] = axes[i][3];

          int x = xyz[0];
          int y = xyz[1];
          int z = xyz[2];

          int key = z * dimen * dimen + y * dimen + x;
          if (grid[key] == 0) {
            printf("error! %d %d %d %d %d\n", key, x, y, z, dimen);
            continue;
          }

          vs[l] = grid[key] - 1;
        }

        if (i % 2 == 1) {
          vs.reverse();
        }
        m->make_face(vs);
      }
    }
  }

  return m;
}

extern "C" Mesh *Mesh_createCube(int dimen, float size, float sphereFac)
{
  return createCube(dimen, size, sphereFac);
}

} // namespace sculptcore::mesh
