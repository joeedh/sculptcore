#include "state_dump.h"

#include "scene.h"

#include "litestl/util/vector.h"
#include "mesh/mesh.h"
#include "spatial/node.h"
#include "spatial/spatial.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace sculptcore::debug_app::state_dump {

using litestl::math::float3;

namespace {

void writeFloat3(std::FILE *f, const float3 &v)
{
  std::fprintf(f, "[%g,%g,%g]", v[0], v[1], v[2]);
}

void dumpMesh(std::FILE *f, mesh::Mesh *m, bool &first)
{
  if (!first) {
    std::fputs(",\n", f);
  }
  first = false;

  float3 mn{0, 0, 0}, mx{0, 0, 0};
  if (m->v.count > 0) {
    m->calcAABB(mn, mx);
  }

  // Order-independent coordinate fingerprint. `co_sum` catches net
  // translation/displacement; `co_sqsum` catches symmetric deformation
  // that leaves the centroid put. Together with the aabb this is a
  // sensitive-but-fp-tolerant golden signature for brush output —
  // accumulated in double so the reduction itself doesn't lose bits.
  double sx = 0.0, sy = 0.0, sz = 0.0, sq = 0.0;
  for (int i = 0; i < m->v.count; i++) {
    float3 co = m->v.co[i];
    sx += co[0];
    sy += co[1];
    sz += co[2];
    sq += double(co[0]) * co[0] + double(co[1]) * co[1] + double(co[2]) * co[2];
  }

  std::fprintf(f, "  \"mesh\": {\n");
  std::fprintf(f, "    \"verts\": %d,\n", m->v.count);
  std::fprintf(f, "    \"edges\": %d,\n", m->e.count);
  std::fprintf(f, "    \"corners\": %d,\n", m->c.count);
  std::fprintf(f, "    \"faces\": %d,\n", m->f.count);
  std::fprintf(f, "    \"aabb_min\": ");
  writeFloat3(f, mn);
  std::fputs(",\n", f);
  std::fprintf(f, "    \"aabb_max\": ");
  writeFloat3(f, mx);
  std::fputs(",\n", f);
  std::fprintf(f, "    \"co_sum\": [%.9g,%.9g,%.9g],\n", sx, sy, sz);
  std::fprintf(f, "    \"co_sqsum\": %.9g", sq);

  // Order-independent fingerprint of user float attribute layers (e.g. a paint
  // brush's "color"). Skips builtins (positions/normals/select and any
  // '.'-prefixed internal layer) so meshes with no user attrs dump byte-for-byte
  // as before — keeping existing brush goldens stable.
  std::string attrsJson;
  for (mesh::AttrRef &a : m->v.attrs.attrs) {
    const char *nm = a.name.c_str();
    if (nm[0] == '.' || std::strcmp(nm, "positions") == 0 ||
        std::strcmp(nm, "normals") == 0 || std::strcmp(nm, "select") == 0) {
      continue;
    }
    int comps = 0;
    switch (a.type) {
    case mesh::AttrType::FLOAT:  comps = 1; break;
    case mesh::AttrType::FLOAT2: comps = 2; break;
    case mesh::AttrType::FLOAT3: comps = 3; break;
    case mesh::AttrType::FLOAT4: comps = 4; break;
    default: continue;
    }
    double sum = 0.0, sqs = 0.0;
    for (int i = 0; i < m->v.count; i++) {
      float c[4] = {0, 0, 0, 0};
      switch (a.type) {
      case mesh::AttrType::FLOAT:
        c[0] = static_cast<mesh::AttrData<float> *>(a.data)->safe_get(i);
        break;
      case mesh::AttrType::FLOAT2: {
        auto v = static_cast<mesh::AttrData<litestl::math::float2> *>(a.data)->safe_get(i);
        c[0] = v[0]; c[1] = v[1];
        break;
      }
      case mesh::AttrType::FLOAT3: {
        auto v = static_cast<mesh::AttrData<float3> *>(a.data)->safe_get(i);
        c[0] = v[0]; c[1] = v[1]; c[2] = v[2];
        break;
      }
      case mesh::AttrType::FLOAT4: {
        auto v = static_cast<mesh::AttrData<litestl::math::float4> *>(a.data)->safe_get(i);
        c[0] = v[0]; c[1] = v[1]; c[2] = v[2]; c[3] = v[3];
        break;
      }
      default: break;
      }
      for (int k = 0; k < comps; k++) {
        sum += c[k];
        sqs += double(c[k]) * c[k];
      }
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s\"%s\":{\"sum\":%.9g,\"sqsum\":%.9g}",
                  attrsJson.empty() ? "" : ",", nm, sum, sqs);
    attrsJson += buf;
  }
  if (!attrsJson.empty()) {
    std::fprintf(f, ",\n    \"attrs\": {%s}", attrsJson.c_str());
  }

  // Order-independent fingerprint of user FACE attribute layers (e.g. the
  // polygroup brush's int "group"). Mirrors the vertex-attr block above but
  // iterates faces and handles INT (the poly-group id). Skips builtins so
  // meshes with no user face attrs dump byte-for-byte as before.
  std::string fattrsJson;
  for (mesh::AttrRef &a : m->f.attrs.attrs) {
    const char *nm = a.name.c_str();
    if (nm[0] == '.') {
      continue;
    }
    double sum = 0.0, sqs = 0.0;
    bool handled = true;
    for (int i = 0; i < m->f.count; i++) {
      double v = 0.0;
      switch (a.type) {
      case mesh::AttrType::INT:
        v = double(static_cast<mesh::AttrData<int> *>(a.data)->safe_get(i));
        break;
      case mesh::AttrType::FLOAT:
        v = double(static_cast<mesh::AttrData<float> *>(a.data)->safe_get(i));
        break;
      default:
        handled = false;
        break;
      }
      if (!handled) {
        break;
      }
      sum += v;
      sqs += v * v;
    }
    if (!handled) {
      continue;
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s\"%s\":{\"sum\":%.9g,\"sqsum\":%.9g}",
                  fattrsJson.empty() ? "" : ",", nm, sum, sqs);
    fattrsJson += buf;
  }
  if (!fattrsJson.empty()) {
    std::fprintf(f, ",\n    \"face_attrs\": {%s}", fattrsJson.c_str());
  }
  std::fputs("\n  }", f);
}

void dumpSpatial(std::FILE *f, spatial::SpatialTree *tree, bool &first)
{
  if (!first) {
    std::fputs(",\n", f);
  }
  first = false;

  auto leaves = tree->leaves();
  std::fprintf(f, "  \"spatial\": {\n");
  std::fprintf(f, "    \"leaf_limit\": %d,\n", tree->leaf_limit);
  std::fprintf(f, "    \"depth_limit\": %d,\n", tree->depth_limit);
  std::fprintf(f, "    \"leaf_count\": %d,\n", int(leaves.size()));
  std::fprintf(f, "    \"leaves\": [");
  for (size_t i = 0; i < leaves.size(); i++) {
    spatial::SpatialNode *n = leaves[i];
    if (i > 0) {
      std::fputs(",", f);
    }
    std::fputs("\n      {", f);
    std::fprintf(f, "\"id\":%d,\"depth\":%d,\"flag\":%d",
                 n->id, n->depth, int(n->flag));
    std::fputs(",\"aabb_min\":", f);
    writeFloat3(f, n->aabb.min);
    std::fputs(",\"aabb_max\":", f);
    writeFloat3(f, n->aabb.max);
    if (n->data) {
      int uv = int(n->data->unique_verts.size());
      int tris = int(n->data->tris.size());
      std::fprintf(f, ",\"unique_verts\":%d,\"tris\":%d", uv, tris);
    }
    std::fputs("}", f);
  }
  std::fputs("\n    ]\n  }", f);
}

void dumpBrush(std::FILE *f, brush::Brush &b, bool &first)
{
  if (!first) {
    std::fputs(",\n", f);
  }
  first = false;
  std::fprintf(f, "  \"brush\": {\n");
  std::fprintf(f, "    \"radius\": %g,\n", b.radius);
  std::fprintf(f, "    \"strength\": %g,\n", b.strength);
  std::fprintf(f, "    \"invert\": %s\n", b.invert ? "true" : "false");
  std::fputs("  }", f);
}

} // namespace

bool writeJSON(Scene &scene, const char *path, const Options &opts)
{
  std::FILE *f = std::fopen(path, "wb");
  if (!f) {
    return false;
  }
  std::fputs("{\n", f);
  bool first = true;
  if (opts.mesh && scene.mesh) {
    dumpMesh(f, scene.mesh, first);
  }
  if (opts.spatial && scene.tree) {
    dumpSpatial(f, scene.tree, first);
  }
  if (opts.brush) {
    dumpBrush(f, scene.brush, first);
  }
  std::fputs("\n}\n", f);
  std::fclose(f);
  return true;
}

} // namespace sculptcore::debug_app::state_dump
