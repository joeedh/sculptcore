#include "uv_reproject.h"

#include "attribute.h"
#include "attribute_enums.h"
#include "mesh.h"
#include "mesh_callbacks.h"
#include "mesh_iter.h"

#include "litestl/math/geom.h"
#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <limits>

namespace sculptcore::mesh::uvproj {

namespace {

using litestl::math::float2;
using litestl::math::float3;
using litestl::util::Map;
using litestl::util::Vector;

// Matches boundary.cc's computeUvChartBoundary discontinuity epsilon.
constexpr float UV_EPS2 = 1e-10f;
// Below this squared displacement a vertex is treated as unmoved.
constexpr float MOVE_EPS2 = 1e-20f;

/** Barycentric weights of @p p relative to triangle (a, b, c); (1,0,0) for a
 * degenerate triangle. */
float3 baryWeights(const float3 &p, const float3 &a, const float3 &b, const float3 &c)
{
  float3 v0 = b - a, v1 = c - a, v2 = p - a;
  float d00 = v0.dot(v0), d01 = v0.dot(v1), d11 = v1.dot(v1);
  float d20 = v2.dot(v0), d21 = v2.dot(v1);
  float denom = d00 * d11 - d01 * d01;
  if (std::fabs(denom) < 1e-20f) {
    return float3(1.0f, 0.0f, 0.0f);
  }
  float v = (d11 * d20 - d01 * d21) / denom;
  float w = (d00 * d21 - d01 * d20) / denom;
  return float3(1.0f - v - w, v, w);
}

/** Corners sitting at vertex @p v (one per incident face: the corner whose
 * outgoing edge lies in v's disk cycle). */
void cornersOfVert(MeshBase *m, int v, Vector<int, 16> &out)
{
  out.clear();
  int e0 = m->v.e[v];
  if (e0 == ELEM_NONE) {
    return;
  }
  for (int e : EdgeOfVertIter(m, v, e0)) {
    int c0 = m->e.c[e];
    if (c0 == ELEM_NONE) {
      continue; // wire edge
    }
    int c = c0;
    do {
      if (m->c.v[c] == v) {
        out.append(c);
      }
      c = m->c.radial_next[c];
    } while (c != c0 && c != ELEM_NONE);
  }
}

} // namespace

int reprojectVertUVs(MeshBase *m,
                     std::span<const int> verts,
                     std::span<const float3> oldCo,
                     MeshCallbacks *cb)
{
  // The UV corner layers (there may be several; each keeps its own wedges).
  Vector<AttrData<float2> *, 4> layers;
  for (AttrRef &attr : m->c.attrs.attrs) {
    if (attr.type == AttrType::FLOAT2 && attr.data &&
        (static_cast<int>(attr.use) & static_cast<int>(AttrUse::UV)) != 0)
    {
      layers.append(static_cast<AttrData<float2> *>(attr.data));
    }
  }
  if (layers.size() == 0 || verts.empty()) {
    return 0;
  }

  Map<int, float3> oldPos;
  for (size_t i = 0; i < verts.size() && i < oldCo.size(); i++) {
    oldPos.insert(verts[i], oldCo[i]);
  }
  auto oldOf = [&](int v) -> float3 {
    float3 *p = oldPos.lookup_ptr(v);
    return p ? *p : m->v.co[v];
  };

  // Deferred writes: (layer, corner, uv). Reads below must only ever see the
  // pre-move UVs — a neighbor's fan interpolates this vertex's corners.
  Vector<int> wLayer, wCorner;
  Vector<float2> wUv;

  Vector<int, 16> corners;
  Vector<int, 16> wedge;
  Vector<int, 16> used;

  for (int vi = 0; vi < int(verts.size()); vi++) {
    const int v = verts[vi];
    if (v < 0 || v >= int(m->v.capacity()) || m->v.freemap[v]) {
      continue;
    }
    const float3 newP = m->v.co[v];
    const float3 oldP = oldOf(v);
    if ((newP - oldP).lengthSqr() < MOVE_EPS2) {
      continue;
    }
    cornersOfVert(m, v, corners);
    if (corners.size() == 0) {
      continue;
    }

    for (int li = 0; li < int(layers.size()); li++) {
      AttrData<float2> *uv = layers[li];

      used.clear();
      used.resize(corners.size());
      for (int i = 0; i < int(corners.size()); i++) {
        used[i] = 0;
      }

      for (int i = 0; i < int(corners.size()); i++) {
        if (used[i]) {
          continue;
        }
        // Wedge: every corner sharing this one's old UV at v.
        const float2 uvAtV = uv->safe_get(corners[i]);
        wedge.clear();
        for (int j = i; j < int(corners.size()); j++) {
          if (!used[j] && (uv->safe_get(corners[j]) - uvAtV).lengthSqr() <= UV_EPS2) {
            used[j] = 1;
            wedge.append(corners[j]);
          }
        }

        // Closest old corner triangle (c, next, prev) of the wedge's faces.
        float bestD2 = std::numeric_limits<float>::max();
        float2 bestUv(0.0f, 0.0f);
        bool found = false;
        for (int c : wedge) {
          const int cn = m->c.next[c], cp = m->c.prev[c];
          if (cn == ELEM_NONE || cp == ELEM_NONE || cn == c || cp == c) {
            continue;
          }
          const float3 a = oldP;
          const float3 b = oldOf(m->c.v[cn]);
          const float3 d = oldOf(m->c.v[cp]);
          const float3 cpnt = litestl::math::closestPointOnTri(newP, a, b, d);
          const float d2 = (cpnt - newP).lengthSqr();
          if (d2 >= bestD2) {
            continue;
          }
          const float3 w = baryWeights(cpnt, a, b, d);
          bestD2 = d2;
          bestUv = uv->safe_get(c) * w[0] + uv->safe_get(cn) * w[1] +
                   uv->safe_get(cp) * w[2];
          found = true;
        }
        if (!found) {
          continue; // degenerate wedge — keep the old UVs
        }
        for (int c : wedge) {
          if ((uv->safe_get(c) - bestUv).lengthSqr() > 0.0f) {
            wLayer.append(li);
            wCorner.append(c);
            wUv.append(bestUv);
          }
        }
      }
    }
  }

  for (int i = 0; i < int(wCorner.size()); i++) {
    if (cb && cb->onCornerChange) {
      cb->onCornerChange(wCorner[i]); // capture the pre-state for undo
    }
    AttrData<float2> *uv = layers[wLayer[i]];
    uv->materialize(wCorner[i]);
    (*uv)[wCorner[i]] = wUv[i];
  }
  return int(wCorner.size());
}

} // namespace sculptcore::mesh::uvproj
