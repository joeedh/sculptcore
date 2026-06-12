#pragma once

// Header-only OBJ import/export, shared by the remesh CLI / debug app and the
// tests. OBJ is a fixture/interchange format here, not a production serialization
// path (that's mesh_serialize.h), so it stays a header-only utility alongside
// mesh_validate.h / triangulate.h. Parses `v` / `f` lines; `vt` / `vn` / groups
// / materials are ignored.

#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"

#include <cstdio>
#include <cstdlib>
#include <span>

namespace sculptcore::mesh {

/* Import an OBJ as a Mesh (caller owns the returned pointer; null on open
 * failure / empty). keepNgons=true makes each face one make_face() (quads and
 * n-gons preserved — what the debug app loads); keepNgons=false fan-triangulates
 * every face on import (the remesh solve / legacy test path). */
static inline Mesh *loadObj(const char *path, bool keepNgons)
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
      if (idx.size() < 3)
        continue;
      if (keepNgons) {
        m->make_face(std::span<int>(idx.data(), idx.size()));
      } else {
        for (int i = 1; i + 1 < int(idx.size()); i++) {
          int tri[3] = {idx[0], idx[i], idx[i + 1]};
          m->make_face(std::span<int>(tri, 3));
        }
      }
    }
  }
  std::fclose(fp);
  m->recalc_normals();
  return m;
}

/* Export a Mesh to OBJ. Native quads/n-gons are emitted as-is (one `f` line per
 * face, 1-based dense vertex remap over the live verts). Returns false if the
 * file can't be opened. */
static inline bool writeObj(Mesh &m, const char *path)
{
  using litestl::math::float3;
  using litestl::util::Vector;

  std::FILE *fp = std::fopen(path, "wb");
  if (!fp) {
    return false;
  }

  Vector<int> omap; // vert handle -> 1-based OBJ index (only live verts filled)
  omap.resize(int(m.v.capacity()));
  int next = 1;
  for (int v : m.v) {
    float3 co = m.v.co[v];
    std::fprintf(fp, "v %.9g %.9g %.9g\n", co[0], co[1], co[2]);
    omap[v] = next++;
  }
  for (int f : m.f) {
    std::fputc('f', fp);
    int li = m.f.l[f], c0 = m.l.c[li], cc = c0;
    do {
      std::fprintf(fp, " %d", omap[m.c.v[cc]]);
      cc = m.c.next[cc];
    } while (cc != c0);
    std::fputc('\n', fp);
  }
  std::fclose(fp);
  return true;
}

} // namespace sculptcore::mesh
