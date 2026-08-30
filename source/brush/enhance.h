#pragma once

#include "mesh/mesh.h"
#include "mesh/mesh_topo_cache.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <utility>

/**
 * Enhance-details brush (difference-of-smooths, band-pass).
 *
 * Amplifies surface relief by adding a high-/band-pass of the position field
 * along a smoothed normal. Two ring-walk low-passes are built per vertex:
 *   S_inner = mean position within `inner` rings   (the sharp reference)
 *   S_outer = mean position within `rings`  rings   (the base surface)
 * and the per-vertex enhance vector is
 *   E = dot(S_inner - S_outer, n̂) * n̂          (n̂ = smoothed normal)
 * `inner == 0` makes S_inner the raw vertex → classic unsharp (high-pass, keeps
 * mesh noise); `inner >= 1` is a difference-of-smooths band-pass that rejects
 * both per-vertex noise and coarse form (the default). The kernel then applies
 * `v.co += E * strength`.
 *
 * E is computed host-side (the N-ring BFS can't live in a 1-ring kernel) and
 * cached per stroke in the `.brush.enhance.disp` TEMP attr, keyed by strokeGen
 * via `.brush.enhance.gen` — the cavity/feature-align pattern. CPU-only, like
 * feature-align.
 */
namespace sculptcore::brush {
using litestl::math::float3;

inline constexpr const char *ENHANCE_DISP_ATTR = ".brush.enhance.disp";
inline constexpr const char *ENHANCE_GEN_ATTR = ".brush.enhance.gen";

struct EnhanceParams {
  // Outer smoothing depth (N) — the low-pass cutoff / feature scale.
  int rings = 4;
  // Inner smoothing depth (m): 0 = classic unsharp (high-pass), >=1 = difference-
  // of-smooths band-pass (default; rejects per-vertex noise). Clamped to < rings.
  int inner = 1;
};

/** Reusable BFS scratch: a monotonic-token visited set sized to the vertex
 * capacity plus two frontier buffers, so the region fill allocates once. */
struct EnhanceScratch {
  litestl::util::Vector<uint32_t> visitStamp;
  uint32_t token = 0;
  litestl::util::Vector<int> frontier;
  litestl::util::Vector<int> nextFrontier;

  void ensure(int vertCap)
  {
    if (int(visitStamp.size()) < vertCap) {
      visitStamp.resize(vertCap);
      for (int i = 0; i < vertCap; i++) {
        visitStamp[i] = 0;
      }
    }
  }
};

/**
 * Per-vertex enhance displacement (difference-of-smooths, projected on the
 * smoothed normal). BFS-walks the ring1 CSR out to `rings`, averaging position
 * over the inner and outer neighborhoods (both include the center) and the
 * normal over the outer one. Requires `m->topo_cache.ring1` current. Returns 0
 * for degenerate neighborhoods.
 */
inline float3
computeEnhanceDisp(mesh::Mesh *m, int v0, const EnhanceParams &p, EnhanceScratch &scr)
{
  const mesh::VertNbrCSR &csr = m->topo_cache.ring1;
  if (int(csr.offsets.size()) <= v0 + 1) {
    return float3{0, 0, 0};
  }

  int inner = p.inner < 0 ? 0 : p.inner;
  int outer = p.rings < 1 ? 1 : p.rings;
  if (inner > outer) {
    inner = outer;
  }

  scr.ensure(m->v.count);
  scr.token++;
  const uint32_t tok = scr.token;

  float3 innerSum{0, 0, 0}, outerSum{0, 0, 0}, normalSum{0, 0, 0};
  int innerCount = 0, outerCount = 0;

  auto visit = [&](int idx, int depth) {
    const float3 co = m->v.co[idx];
    outerSum += co;
    normalSum += m->v.no[idx];
    outerCount++;
    if (depth <= inner) {
      innerSum += co;
      innerCount++;
    }
  };

  scr.frontier.clear();
  scr.visitStamp[v0] = tok;
  scr.frontier.append(v0);
  visit(v0, 0);

  for (int depth = 1; depth <= outer; depth++) {
    scr.nextFrontier.clear();
    for (int u : scr.frontier) {
      uint32_t off = csr.offsets[u];
      uint32_t end = csr.offsets[u + 1];
      for (uint32_t k = off; k < end; k++) {
        int nb = csr.nbr_verts[k];
        if (scr.visitStamp[nb] == tok) {
          continue;
        }
        scr.visitStamp[nb] = tok;
        scr.nextFrontier.append(nb);
        visit(nb, depth);
      }
    }
    std::swap(scr.frontier, scr.nextFrontier);
    if (scr.frontier.size() == 0) {
      break;
    }
  }

  if (innerCount == 0 || outerCount == 0) {
    return float3{0, 0, 0};
  }

  float3 sInner = innerSum / float(innerCount);
  float3 sOuter = outerSum / float(outerCount);

  float3 nrm = normalSum;
  float nlen = nrm.length();
  nrm = nlen > 1e-6f ? nrm / nlen : m->v.no[v0];

  float3 detail = sInner - sOuter;
  return nrm * detail.dot(nrm);
}

/**
 * Fill `.brush.enhance.disp` for `verts` (the dab region) with the per-vertex
 * enhance vector, once per stroke (gated by `.brush.enhance.gen == strokeGen`).
 * Ensures the two TEMP attrs and the ring1 CSR. Call from the executor before
 * the ENHANCE kernel, while topology is live.
 */
inline void updateEnhanceRegion(mesh::Mesh &m,
                                const litestl::util::Vector<int> &verts,
                                const EnhanceParams &params,
                                uint32_t strokeGen)
{
  m.topo_cache.ensureRing1(m);

  mesh::AttrRef &dispRef =
      m.v.attrs.ensure(mesh::AttrType::FLOAT3, ENHANCE_DISP_ATTR, false);
  dispRef.flag |= mesh::AttrFlag::TEMP | mesh::AttrFlag::NOCOPY;
  mesh::AttrRef &genRef = m.v.attrs.ensure(mesh::AttrType::INT, ENHANCE_GEN_ATTR, false);
  genRef.flag |= mesh::AttrFlag::TEMP | mesh::AttrFlag::NOCOPY;
  auto *disp = static_cast<mesh::AttrData<float3> *>(dispRef.data);
  auto *gen = static_cast<mesh::AttrData<int> *>(genRef.data);

  EnhanceScratch scr;
  for (int v : verts) {
    gen->materialize(v);
    disp->materialize(v);
    if (strokeGen == 0 || (*gen)[v] != int(strokeGen)) {
      (*disp)[v] = computeEnhanceDisp(&m, v, params, scr);
      (*gen)[v] = int(strokeGen);
    }
  }
}

} // namespace sculptcore::brush
