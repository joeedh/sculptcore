#pragma once

// Tests-only OBJ importer. OBJ is a fixture format here, not a production path,
// so this lives in tests/ rather than source/mesh/c-api/. Parses `v` / `f`
// lines and fan-triangulates any n-gon face on import, so the remesh pipeline
// always receives a clean triangle mesh regardless of the .obj's face types.
// vt / vn / groups / materials are ignored.

#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"

#include <cstdio>
#include <cstdlib>
#include <span>

namespace sculptcore::mesh {

static inline Mesh *loadObj(const char *path)
{
  std::FILE *fp = std::fopen(path, "rb");
  if (!fp) {
    return nullptr;
  }

  using litestl::math::float3;
  using litestl::util::Vector;

  Mesh *m = litestl::alloc::New<Mesh>("Mesh OBJ");
  Vector<int> verts; // obj 1-based vertex index -> mesh vert handle

  char line[2048];
  while (std::fgets(line, sizeof(line), fp)) {
    if (line[0] == 'v' && (line[1] == ' ' || line[1] == '\t')) {
      float x = 0.0f, y = 0.0f, z = 0.0f;
      std::sscanf(line + 2, "%f %f %f", &x, &y, &z);
      verts.append(m->make_vertex(float3(x, y, z)));
    } else if (line[0] == 'f' && (line[1] == ' ' || line[1] == '\t')) {
      Vector<int, 16> idx;
      const char *p = line + 1;
      while (*p) {
        while (*p == ' ' || *p == '\t')
          p++;
        if (*p == 0 || *p == '\n' || *p == '\r')
          break;
        // Each token is v, v/vt, v/vt/vn or v//vn — take the leading integer.
        int vi = std::atoi(p);
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
          p++;
        if (vi == 0)
          continue;
        if (vi < 0)
          vi = int(verts.size()) + vi + 1; // OBJ relative (negative) index
        if (vi < 1 || vi > int(verts.size()))
          continue;
        idx.append(verts[vi - 1]);
      }
      for (int i = 1; i + 1 < int(idx.size()); i++) {
        int tri[3] = {idx[0], idx[i], idx[i + 1]};
        m->make_face(std::span<int>(tri, 3));
      }
    }
  }
  std::fclose(fp);
  m->recalc_normals();
  return m;
}

} // namespace sculptcore::mesh
