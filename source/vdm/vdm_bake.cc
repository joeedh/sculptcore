#include "vdm_bake.h"

#include "displace/frames.h"
#include "mesh/attribute.h"
#include "mesh/mesh.h"
#include "mesh/mesh_proxy.h"

#include "litestl/util/set.h"
#include "litestl/util/vector.h"

namespace sculptcore::vdm {

using litestl::math::float2;
using litestl::util::Set;
using litestl::util::Vector;
using mesh::AttrData;
using mesh::AttrRef;
using mesh::AttrType;
using mesh::Mesh;

namespace {
constexpr float EPS = 1e-9f;

inline float3 safeNormalize(const float3 &v)
{
  float l = v.length();
  return l > EPS ? v * (1.0f / l) : float3(0.0f, 0.0f, 0.0f);
}
} // namespace

VdmBakeStats applyToVerts(Mesh &m, VdmStore &store, bool clearStore)
{
  VdmBakeStats stats;

  bool ptex = store.params.backend == VdmBackend::PTEX;
  AttrData<float2> *uv = nullptr;
  AttrData<int> *ptexGrid = nullptr;
  AttrData<float2> *ptexUv = nullptr;
  if (ptex) {
    AttrRef gref = m.c.attrs.find_attribute(AttrType::INT, PTEX_GRID_ATTR);
    AttrRef uref = m.c.attrs.find_attribute(AttrType::FLOAT2, PTEX_UV_ATTR);
    ptexGrid = gref.exists() ? static_cast<AttrData<int> *>(gref.data) : nullptr;
    ptexUv = uref.exists() ? static_cast<AttrData<float2> *>(uref.data) : nullptr;
    if (!ptexGrid || !ptexUv) {
      return stats;
    }
  } else {
    uv = findUvCornerLayer(m);
    if (!uv) {
      return stats;
    }
  }
  AttrRef nref =
      m.v.attrs.find_attribute(AttrType::FLOAT3, displace::FRAME_NORMAL_ATTR);
  AttrRef tref =
      m.v.attrs.find_attribute(AttrType::FLOAT3, displace::FRAME_TANGENT_ATTR);
  if (!nref.exists() || !tref.exists()) {
    return stats;
  }
  AttrData<float3> *fnormal = static_cast<AttrData<float3> *>(nref.data);
  AttrData<float3> *ftangent = static_cast<AttrData<float3> *>(tref.data);

  // Per-corner params, per-vert displacement: walk face corners and take the
  // first corner seen for each vert (grid-border verts have corners in
  // several grids; the skirt sync makes either sample agree to C0).
  Set<int> visited;
  for (int f : m.f) {
    mesh::FaceProxy face(&m, f);
    for (auto list : face.lists()) {
      for (auto c : list) {
        int v = c.v();
        if (visited.contains(v)) {
          continue;
        }
        visited.add(v);

        int grid = 0;
        float2 p;
        if (ptex) {
          grid = ptexGrid->safe_get(c.i);
          if (grid < 0) {
            continue;
          }
          p = ptexUv->safe_get(c.i);
        } else {
          p = uv->safe_get(c.i);
        }
        float3 D = store.sample(grid, p[0], p[1]);
        if (D.length() < EPS) {
          continue;
        }
        float3 n = safeNormalize(fnormal->safe_get(v));
        float3 t = ftangent->safe_get(v);
        t = safeNormalize(t - n * n.dot(t));
        if (n.length() < EPS || t.length() < EPS) {
          continue;
        }
        float3 b = n.cross(t);
        m.v.co[v] = m.v.co[v] + t * D[0] + b * D[1] + n * D[2];
        stats.vertsMoved++;
      }
    }
  }

  if (clearStore) {
    stats.tilesCleared = store.tileCount();
    store.clearTiles();
  }
  if (stats.vertsMoved > 0) {
    m.recalc_normals();
  }
  return stats;
}

} // namespace sculptcore::vdm
