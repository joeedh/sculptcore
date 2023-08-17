#include "util/alloc.h"
#include "util/set.h"
#include "util/vector.h"

#include "mesh.h"

namespace sculptcore::mesh {
Mesh *createCube(int dimen, float size, float sphereFac)
{
  using namespace sculptcore::util;
  using namespace sculptcore::math;

  Vector<int> grid;

  int totpoint = dimen * dimen * 6;

  grid.resize(totpoint);

  Mesh *m = alloc::New<Mesh>("Mesh Cube");

  int axes[6][4] = {
      {0, 1, 2, -1}, //
      {0, 1, 2, 1},  //
      {1, 0, 2, -1}, //
      {1, 0, 2, 1},  //
      {0, 2, 1, -1}, //
      {1, 2, 0, -1},
  };

  float idimen = 1.0f / float(dimen);
  Map<int, int> vmap;

  for (int i = 0; i < 6; i++) {
    for (int j = 0; j < dimen; j++) {
      for (int k = 0; k < dimen; k++) {
        int ivec[3] = {j, k, i * totpoint};

        int x = ivec[axes[i][0]];
        int y = ivec[axes[i][1]];
        int z = ivec[axes[i][2]];

        float3 co;
        co[0] = (idimen * float(x)) - 0.5f;
        co[1] = (idimen * float(y)) - 0.5f;
        co[2] = (idimen * float(z)) - 0.5f;
        co *= size;

        int key = z * dimen * dimen + y * dimen + x;
        int v, *value = nullptr;

        if (vmap.add_uninitialized(key, &value)) {
          v = m->make_vertex(co);
          grid[key] = v;

          *value = v;
        } else {
          grid[key] = *value;
        }
      }
    }
  }

  Vector<int> vs;
  vs.resize(4);

  for (int i = 0; i < 6; i++) {
    for (int j = 0; j < dimen - 1; j++) {
      for (int k = 0; k < dimen - 1; k++) {
        int ivecs[4][3] = {
            {j, k, i * totpoint},         //
            {j, k + 1, i * totpoint},     //
            {j + 1, k + 1, i * totpoint}, //
            {j + 1, k, i * totpoint},     //
        };

        for (int l = 0; l < 4; l++) {
          int x = ivecs[l][axes[i][0]];
          int y = ivecs[l][axes[i][1]];
          int z = ivecs[l][axes[i][2]];

          vs[l] = vmap[z * dimen * dimen + y * dimen + x];
        }

        m->make_face(vs);
      }
    }
  }

  return m;
}
} // namespace sculptcore::mesh
