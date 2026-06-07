#include <cmath>

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

static constexpr double SC_PI = 3.14159265358979323846;

Mesh *makeGrid(int nx, int ny, float size)
{
  using namespace litestl::util;
  using namespace litestl::math;

  nx = std::max(nx, 2);
  ny = std::max(ny, 2);

  Mesh *m = alloc::New<Mesh>("Mesh Grid");

  Vector<int> verts;
  verts.resize(nx * ny);

  float inx = 1.0f / float(nx - 1);
  float iny = 1.0f / float(ny - 1);

  for (int i = 0; i < nx; i++) {
    for (int j = 0; j < ny; j++) {
      float3 co;
      co[0] = (float(i) * inx - 0.5f) * size;
      co[1] = (float(j) * iny - 0.5f) * size;
      co[2] = 0.0f;
      verts[i * ny + j] = m->make_vertex(co);
    }
  }

  Vector<int> vs;
  for (int i = 0; i < nx - 1; i++) {
    for (int j = 0; j < ny - 1; j++) {
      vs.clear();
      vs.append(verts[i * ny + j]);
      vs.append(verts[(i + 1) * ny + j]);
      vs.append(verts[(i + 1) * ny + (j + 1)]);
      vs.append(verts[i * ny + (j + 1)]);
      m->make_face(vs);
    }
  }

  m->recalc_normals();
  return m;
}

Mesh *makeCylinder(int radialSegs, int heightSegs, float radius, float height,
                   bool capped)
{
  using namespace litestl::util;
  using namespace litestl::math;

  int R = std::max(radialSegs, 3);
  int H = std::max(heightSegs, 1);

  Mesh *m = alloc::New<Mesh>("Mesh Cylinder");

  Vector<int> ring;
  ring.resize((H + 1) * R);

  float h2 = height * 0.5f;
  for (int k = 0; k <= H; k++) {
    float z = -h2 + height * (float(k) / float(H));
    for (int s = 0; s < R; s++) {
      float a = 2.0f * float(SC_PI) * (float(s) / float(R));
      float3 co;
      co[0] = radius * std::cos(a);
      co[1] = radius * std::sin(a);
      co[2] = z;
      ring[k * R + s] = m->make_vertex(co);
    }
  }

  Vector<int> vs;
  for (int k = 0; k < H; k++) {
    for (int s = 0; s < R; s++) {
      int s2 = (s + 1) % R;
      vs.clear();
      vs.append(ring[k * R + s]);
      vs.append(ring[k * R + s2]);
      vs.append(ring[(k + 1) * R + s2]);
      vs.append(ring[(k + 1) * R + s]);
      m->make_face(vs);
    }
  }

  if (capped) {
    float3 cbco{0.0f, 0.0f, -h2};
    float3 ctco{0.0f, 0.0f, h2};
    int cb = m->make_vertex(cbco);
    int ct = m->make_vertex(ctco);

    for (int s = 0; s < R; s++) {
      int s2 = (s + 1) % R;
      // bottom (z=-h2): outward -z
      vs.clear();
      vs.append(cb);
      vs.append(ring[s2]);
      vs.append(ring[s]);
      m->make_face(vs);
      // top (z=+h2): outward +z
      vs.clear();
      vs.append(ct);
      vs.append(ring[H * R + s]);
      vs.append(ring[H * R + s2]);
      m->make_face(vs);
    }
  }

  m->recalc_normals();
  return m;
}

Mesh *makeTorus(int majorSegs, int minorSegs, float majorRadius,
                float minorRadius)
{
  using namespace litestl::util;
  using namespace litestl::math;

  int U = std::max(majorSegs, 3);
  int V = std::max(minorSegs, 3);

  Mesh *m = alloc::New<Mesh>("Mesh Torus");

  Vector<int> grid;
  grid.resize(U * V);

  for (int u = 0; u < U; u++) {
    float th = 2.0f * float(SC_PI) * (float(u) / float(U));
    for (int v = 0; v < V; v++) {
      float ph = 2.0f * float(SC_PI) * (float(v) / float(V));
      float r = majorRadius + minorRadius * std::cos(ph);
      float3 co;
      co[0] = r * std::cos(th);
      co[1] = r * std::sin(th);
      co[2] = minorRadius * std::sin(ph);
      grid[u * V + v] = m->make_vertex(co);
    }
  }

  Vector<int> vs;
  for (int u = 0; u < U; u++) {
    int u2 = (u + 1) % U;
    for (int v = 0; v < V; v++) {
      int v2 = (v + 1) % V;
      vs.clear();
      vs.append(grid[u * V + v]);
      vs.append(grid[u2 * V + v]);
      vs.append(grid[u2 * V + v2]);
      vs.append(grid[u * V + v2]);
      m->make_face(vs);
    }
  }

  m->recalc_normals();
  return m;
}

Mesh *makeUVSphere(int rings, int segs, float radius)
{
  using namespace litestl::util;
  using namespace litestl::math;

  rings = std::max(rings, 2);
  segs = std::max(segs, 3);

  Mesh *m = alloc::New<Mesh>("Mesh UVSphere");

  float3 topco{0.0f, 0.0f, radius};
  float3 botco{0.0f, 0.0f, -radius};
  int top = m->make_vertex(topco);
  int bot = m->make_vertex(botco);

  // Interior latitude rings i in [1, rings-1], `segs` verts each.
  int nrows = rings - 1;
  Vector<int> grid;
  grid.resize(nrows * segs);

  for (int i = 1; i < rings; i++) {
    float th = float(SC_PI) * (float(i) / float(rings));
    float st = std::sin(th), ct = std::cos(th);
    for (int j = 0; j < segs; j++) {
      float ph = 2.0f * float(SC_PI) * (float(j) / float(segs));
      float3 co;
      co[0] = radius * st * std::cos(ph);
      co[1] = radius * st * std::sin(ph);
      co[2] = radius * ct;
      grid[(i - 1) * segs + j] = m->make_vertex(co);
    }
  }

  Vector<int> vs;

  // Top triangle fan.
  for (int j = 0; j < segs; j++) {
    int j2 = (j + 1) % segs;
    vs.clear();
    vs.append(top);
    vs.append(grid[j]);
    vs.append(grid[j2]);
    m->make_face(vs);
  }

  // Quad bands between interior rings.
  for (int i = 1; i <= rings - 2; i++) {
    for (int j = 0; j < segs; j++) {
      int j2 = (j + 1) % segs;
      vs.clear();
      vs.append(grid[(i - 1) * segs + j]);
      vs.append(grid[i * segs + j]);
      vs.append(grid[i * segs + j2]);
      vs.append(grid[(i - 1) * segs + j2]);
      m->make_face(vs);
    }
  }

  // Bottom triangle fan.
  int last = nrows - 1;
  for (int j = 0; j < segs; j++) {
    int j2 = (j + 1) % segs;
    vs.clear();
    vs.append(bot);
    vs.append(grid[last * segs + j2]);
    vs.append(grid[last * segs + j]);
    m->make_face(vs);
  }

  m->recalc_normals();
  return m;
}

extern "C" Mesh *Mesh_createCube(int dimen, float size, float sphereFac)
{
  return createCube(dimen, size, sphereFac);
}

extern "C" Mesh *Mesh_makeGrid(int nx, int ny, float size)
{
  return makeGrid(nx, ny, size);
}

extern "C" Mesh *Mesh_makeCylinder(int radialSegs, int heightSegs, float radius,
                                   float height, int capped)
{
  return makeCylinder(radialSegs, heightSegs, radius, height, capped != 0);
}

extern "C" Mesh *Mesh_makeTorus(int majorSegs, int minorSegs, float majorRadius,
                                float minorRadius)
{
  return makeTorus(majorSegs, minorSegs, majorRadius, minorRadius);
}

extern "C" Mesh *Mesh_makeUVSphere(int rings, int segs, float radius)
{
  return makeUVSphere(rings, segs, radius);
}

extern "C" void Mesh_free(Mesh *m)
{
  litestl::alloc::Delete<Mesh>(m);
}

} // namespace sculptcore::mesh
