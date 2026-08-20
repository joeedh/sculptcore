/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Derived per-grid-element attributes for a multires stack (subdiv/grid_attrs.h):
 * the host-capability storage policy, Blender's ptex-bilinear rule for generic
 * layers, the face-varying rule for UV maps, and per-grid face-set colors.
 *
 * The fixtures are planar quad grids, where both rules have a closed form: a
 * uniform planar lattice is a Catmull-Clark fixed point (face point = centroid,
 * edge point = midpoint, vertex point = itself, limit point = itself), so a
 * subdivided sample must land exactly on the bilinear lattice point that the
 * cage's corner values define. That makes the check independent of the code
 * under test rather than a snapshot of it. */

#include "test_util.h"

#include "mesh/attribute.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "brush/brush.h"
#include "brush/cage_smooth.h"
#include "subdiv/c-api/grid_channel_c_api.h"
#include "subdiv/grid_attrs.h"
#include "subdiv/grid_domain.h"
#include "mesh/mesh_proxy.h"
#include "subdiv/multires.h"
#include "subdiv/subdiv.h"
#include "vdm/vdm_store.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>
#include <cstring>

test_init;

/* The undo seam (subdiv/c-api/subdiv_c_api.cc): no header declares these, the
 * hosts reach them through the DLL's C interface. */
extern "C" {
uint8_t *Multires_serializeStore(sculptcore::subdiv::Multires *mr, int *out_size);
int Multires_restoreStore(sculptcore::subdiv::Multires *mr, const uint8_t *data, int size);
uint64_t Multires_maskGeneration(sculptcore::subdiv::Multires *mr);
void freeMeshBuffer(uint8_t *buf);
}

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace litestl;
using namespace litestl::math;
using litestl::util::Vector;
using subdiv::GridAttrStorage;
using subdiv::Multires;
using subdiv::UvSmooth;

/* An n x n quad grid in the XY plane, one unit per cell. */
static Mesh *makeGrid(int n)
{
  Mesh *m = alloc::New<Mesh>("attr grid");
  Vector<int> verts;
  for (int y = 0; y <= n; y++) {
    for (int x = 0; x <= n; x++) {
      verts.append(m->make_vertex(float3(float(x), float(y), 0.0f)));
    }
  }
  const int w = n + 1;
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      int q[4] = {verts[y * w + x],
                  verts[y * w + x + 1],
                  verts[(y + 1) * w + x + 1],
                  verts[(y + 1) * w + x]};
      m->make_face(std::span<int>(q, 4));
    }
  }
  return m;
}

/* uv = vertex xy, optionally offset per chart so a seam exists. */
static void assignUVs(Mesh *m, float seamAtY)
{
  AttrRef &ref = m->c.attrs.ensure(AttrType::FLOAT2, "uv", /*materialize=*/true);
  ref.use = AttrUse::UV;
  auto *uv = ref.get_data<float2>();
  for (int c : m->c) {
    const int f = m->l.f[m->c.l[c]];
    float cy = 0.0f;
    int cnt = 0;
    const int c0 = m->l.c[m->f.l[f]];
    int cc = c0;
    do {
      cy += m->v.co[m->c.v[cc]][1];
      cnt++;
      cc = m->c.next[cc];
    } while (cc != c0);
    cy /= float(cnt);
    const float3 &co = m->v.co[m->c.v[c]];
    (*uv)[c] = float2(co[0] + (cy > seamAtY ? 10.0f : 0.0f), co[1]);
  }
}

/* The cage vert of grid `g`'s (0,0) sample, plus the next/previous corner verts
 * — the grid's own axes (subdiv.h). */
struct TestCorner {
  int face, v0, vNext, vPrev, vOpp, size;
};

static void cornerRefs(Mesh &cage, Vector<TestCorner> &out)
{
  for (int fi : cage.f) {
    const int c0 = cage.l.c[cage.f.l[fi]];
    int size = 0, cc = c0;
    do {
      size++;
      cc = cage.c.next[cc];
    } while (cc != c0);
    cc = c0;
    do {
      const int cn = cage.c.next[cc];
      int cp = cc;
      while (cage.c.next[cp] != cc) {
        cp = cage.c.next[cp];
      }
      const int co = cage.c.next[cn];
      out.append(TestCorner{
          fi, cage.c.v[cc], cage.c.v[cn], cage.c.v[cp], cage.c.v[co], size});
      cc = cn;
    } while (cc != c0);
  }
}

/* The bilinear value at grid sample (u,v) of a quad face, from the four cage
 * corner values in the grid's own axes: A + fu/2·(B-A) + fv/2·(D-A) bilinearly
 * over the whole quad, which for a quad reduces to the standard patch. */
static float2 expectedQuadUv(
    float2 A, float2 B, float2 D, float2 C, float fu, float fv)
{
  const float su = fu * 0.5f, sv = fv * 0.5f;
  const float w0 = (1.0f - su) * (1.0f - sv);
  const float w1 = su * (1.0f - sv);
  const float w2 = su * sv;
  const float w3 = (1.0f - su) * sv;
  return A * w0 + B * w1 + C * w2 + D * w3;
}

static void gateStoragePolicy()
{
  Mesh *cage = makeGrid(2);
  Multires mr;
  mr.init(*cage, 1);
  auto &ga = mr.gridAttrs();

  ga.declareHostAttr("mask", AttrType::FLOAT);

  test_assert(ga.storageFor("mask", AttrType::FLOAT, AttrFlag::NONE) ==
              GridAttrStorage::Host);
  /* Declared by name AND type: a float3 "mask" is not the host's mask. */
  test_assert(ga.storageFor("mask", AttrType::FLOAT3, AttrFlag::NONE) ==
              GridAttrStorage::Derived);
  test_assert(ga.storageFor("color", AttrType::FLOAT4, AttrFlag::NONE) ==
              GridAttrStorage::Derived);
  /* Scratch the host never stores stays writable per grid element. */
  test_assert(ga.storageFor("_tmp", AttrType::FLOAT4, AttrFlag::TEMP) ==
              GridAttrStorage::Temp);
  test_assert(ga.storageFor("mask", AttrType::FLOAT, AttrFlag::TEMP) ==
              GridAttrStorage::Temp);

  ga.clearHostAttrs();
  test_assert(ga.storageFor("mask", AttrType::FLOAT, AttrFlag::NONE) ==
              GridAttrStorage::Derived);

  alloc::Delete(cage);
}

/* A point-domain float3 layer holding the vertex position: on a planar grid the
 * bilinear rule must reproduce the lattice point exactly. */
static void gateBilinearPoint(int level)
{
  Mesh *cage = makeGrid(3);
  {
    AttrRef &ref = cage->v.attrs.ensure(AttrType::FLOAT3, "pos", /*materialize=*/true);
    auto *d = ref.get_data<float3>();
    for (int v : cage->v) {
      (*d)[v] = cage->v.co[v];
    }
  }
  Multires mr;
  mr.init(*cage, level);

  int comps = 0;
  const float *s = mr.gridAttrs().samples(level, "pos", &comps);
  test_assert(s != nullptr);
  test_assert(comps == 3);
  if (!s || comps != 3) {
    alloc::Delete(cage);
    return;
  }

  Vector<TestCorner> refs;
  cornerRefs(*cage, refs);
  test_assert(int(refs.size()) == mr.refiner.gridCount());

  const int S = mr.refiner.levels[level - 1].gridSide;
  const int w = S + 1;
  float worst = 0.0f;
  for (int g = 0; g < mr.refiner.gridCount(); g++) {
    const TestCorner &r = refs[g];
    const float3 A = cage->v.co[r.v0], B = cage->v.co[r.vNext];
    const float3 D = cage->v.co[r.vPrev], C = cage->v.co[r.vOpp];
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        const float su = float(u) / float(S) * 0.5f;
        const float sv = float(v) / float(S) * 0.5f;
        const float3 want = A * ((1.0f - su) * (1.0f - sv)) + B * (su * (1.0f - sv)) +
                            C * (su * sv) + D * ((1.0f - su) * sv);
        const float *got = s + (size_t(g) * (w * w) + size_t(v) * w + u) * 3;
        for (int k = 0; k < 3; k++) {
          worst = std::max(worst, std::fabs(got[k] - want[k]));
        }
      }
    }
  }
  test_assert(worst < 1e-5f);
  if (worst >= 1e-5f) {
    fprintf(stderr, "bilinear point (level %d): worst %g\n", level, worst);
  }
  alloc::Delete(cage);
}

/* UVs go through the face-varying path. On a planar UV layout the fvar limit
 * surface is the same uniform lattice, so the same closed form applies — and
 * every sample must stay inside its own chart, i.e. the seam does not blend. */
static void gateFaceVaryingUv(UvSmooth mode, bool withSeam)
{
  Mesh *cage = makeGrid(4);
  assignUVs(cage, withSeam ? 2.0f : 1e9f);

  Multires mr;
  mr.init(*cage, 2);
  mr.gridAttrs().setUvSmooth(mode);

  const int level = 2;
  const float2 *uv = mr.gridAttrs().uvSamples(level);
  test_assert(uv != nullptr);
  if (!uv) {
    alloc::Delete(cage);
    return;
  }

  AttrRef ref = cage->c.attrs.find_attribute(AttrType::FLOAT2, "uv");
  auto *cuv = ref.get_data<float2>();

  Vector<TestCorner> refs;
  cornerRefs(*cage, refs);
  /* Corner ids per grid, in the same walk cornerRefs uses. */
  Vector<int> gridCorner;
  for (int fi : cage->f) {
    const int c0 = cage->l.c[cage->f.l[fi]];
    int cc = c0;
    do {
      gridCorner.append(cc);
      cc = cage->c.next[cc];
    } while (cc != c0);
  }

  const int S = mr.refiner.levels[level - 1].gridSide;
  const int w = S + 1;
  float worst = 0.0f;
  for (int g = 0; g < mr.refiner.gridCount(); g++) {
    const int cc = gridCorner[g];
    const int cn = cage->c.next[cc];
    const int co = cage->c.next[cn];
    int cp = cc;
    while (cage->c.next[cp] != cc) {
      cp = cage->c.next[cp];
    }
    const float2 A = (*cuv)[cc], B = (*cuv)[cn], C = (*cuv)[co], D = (*cuv)[cp];
    for (int v = 0; v < w; v++) {
      for (int u = 0; u < w; u++) {
        const float2 want =
            expectedQuadUv(A, B, D, C, float(u) / float(S), float(v) / float(S));
        const float2 &got = uv[size_t(g) * (w * w) + size_t(v) * w + u];
        worst = std::max(worst, std::fabs(got[0] - want[0]));
        worst = std::max(worst, std::fabs(got[1] - want[1]));
      }
    }
  }
  test_assert(worst < 1e-4f);
  if (worst >= 1e-4f) {
    fprintf(stderr,
            "fvar uv (mode %d, seam %d): worst %g\n",
            int(mode),
            int(withSeam),
            worst);
  }
  alloc::Delete(cage);
}

/* The planar gates above are deliberately a Catmull-Clark fixed point, so a
 * regression that quietly routed UVs down the bilinear path would still pass
 * them. Distort one interior vertex's UV and the two rules must disagree:
 * SMOOTH_ALL is smooth everywhere, NONE is FVAR_LINEAR_ALL. */
static void gateFaceVaryingIsNotBilinear()
{
  Mesh *cage = makeGrid(4);
  assignUVs(cage, 1e9f);
  {
    /* Pull every corner sitting on the middle vertex. */
    auto *uv = cage->c.attrs.find_attribute(AttrType::FLOAT2, "uv").get_data<float2>();
    for (int c : cage->c) {
      const float3 &co = cage->v.co[cage->c.v[c]];
      if (co[0] == 2.0f && co[1] == 2.0f) {
        (*uv)[c] = float2(2.6f, 2.4f);
      }
    }
  }

  Multires mr;
  mr.init(*cage, 2);
  const int level = 2;
  const int S = mr.refiner.levels[level - 1].gridSide;
  const int n = mr.refiner.gridCount() * (S + 1) * (S + 1);

  mr.gridAttrs().setUvSmooth(UvSmooth::None);
  Vector<float2> linear;
  {
    const float2 *uv = mr.gridAttrs().uvSamples(level);
    test_assert(uv != nullptr);
    if (!uv) {
      alloc::Delete(cage);
      return;
    }
    for (int i = 0; i < n; i++) {
      linear.append(uv[i]);
    }
  }

  mr.gridAttrs().setUvSmooth(UvSmooth::SmoothAll);
  const float2 *smooth = mr.gridAttrs().uvSamples(level);
  test_assert(smooth != nullptr);
  if (!smooth) {
    alloc::Delete(cage);
    return;
  }
  float biggest = 0.0f;
  for (int i = 0; i < n; i++) {
    biggest = std::max(biggest, std::fabs(smooth[i][0] - linear[i][0]));
    biggest = std::max(biggest, std::fabs(smooth[i][1] - linear[i][1]));
  }
  test_assert(biggest > 0.01f);
  if (biggest <= 0.01f) {
    fprintf(stderr, "fvar collapsed onto bilinear: max delta %g\n", biggest);
  }

  alloc::Delete(cage);
}

/* Face sets are per grid (a grid is one cage corner), and the default group
 * must stay white so the host's face-set overlay shows nothing. */
static void gateFaceSetColors()
{
  Mesh *cage = makeGrid(2);
  {
    AttrRef &ref = cage->f.attrs.ensure(AttrType::INT, "group", /*materialize=*/true);
    auto *d = ref.get_data<int>();
    int i = 0;
    for (int f : cage->f) {
      (*d)[f] = (i++ < 2) ? cage->default_group_id : 7;
    }
  }
  Multires mr;
  mr.init(*cage, 1);

  const float3 *cols = mr.gridAttrs().gridFaceSetColors();
  test_assert(cols != nullptr);
  if (!cols) {
    alloc::Delete(cage);
    return;
  }
  int white = 0, colored = 0;
  for (int g = 0; g < mr.refiner.gridCount(); g++) {
    const bool isWhite = cols[g][0] == 1.0f && cols[g][1] == 1.0f && cols[g][2] == 1.0f;
    (isWhite ? white : colored)++;
    /* Every channel of a hashed colour stays in the readable band. */
    if (!isWhite) {
      for (int k = 0; k < 3; k++) {
        test_assert(cols[g][k] >= 0.25f && cols[g][k] <= 0.96f);
      }
    }
  }
  test_assert(white == 8);   /* two default-group quads */
  test_assert(colored == 8); /* two group-7 quads */

  alloc::Delete(cage);
}

/* A materialized level mesh must carry the derived cage attributes, because
 * that mesh is what the host draws whenever a mesh-path tool is active — and it
 * must carry them as its own UV map, distinct from the internal ptex atlas. */
static void gateSlotAttrs()
{
  Mesh *cage = makeGrid(2);
  assignUVs(cage, -1.0f);
  {
    AttrRef &ref = cage->f.attrs.ensure(AttrType::INT, "group", /*materialize=*/true);
    auto *d = ref.get_data<int>();
    for (int f : cage->f) {
      (*d)[f] = 7;
    }
  }
  {
    AttrRef &ref = cage->v.attrs.ensure(AttrType::FLOAT4, "color", /*materialize=*/true);
    ref.use = AttrUse::COLOR;
    auto *d = ref.get_data<float4>();
    for (int v : cage->v) {
      (*d)[v] = float4(cage->v.co[v][0], 0.0f, 0.0f, 1.0f);
    }
  }

  Multires mr;
  mr.init(*cage, 1);
  subdiv::MultiresSlot *slot = mr.setActiveLevel(1);
  test_assert(slot && slot->mesh);
  if (!slot || !slot->mesh) {
    alloc::Delete(cage);
    return;
  }
  Mesh &m = *slot->mesh;

  /* The atlas is present and untagged; the UV map is present and tagged. */
  AttrRef atlas = m.c.attrs.find_attribute(AttrType::FLOAT2, vdm::PTEX_ATLAS_ATTR);
  test_assert(atlas.exists() && int(atlas.use & AttrUse::UV) == 0);
  AttrRef uvRef = m.c.attrs.find_attribute(AttrType::FLOAT2, "uv");
  test_assert(uvRef.exists() && int(uvRef.use & AttrUse::UV) != 0);
  auto *uv = uvRef.get_data<float2>();

  const float2 *samples = mr.gridAttrs().uvSamples(1);
  test_assert(samples != nullptr);
  if (!samples) {
    alloc::Delete(cage);
    return;
  }

  /* Every corner reads its own lattice sample — the face-major walk again, so a
   * corner-order or grid-order drift between the two shows up here. */
  subdiv::SubdivLevel &lvl = mr.refiner.levels[0];
  const int S = lvl.gridSide, w = S + 1;
  static const int du[4] = {0, 1, 1, 0};
  static const int dv[4] = {0, 0, 1, 1};
  int f = 0;
  for (int g = 0; g < mr.refiner.gridCount(); g++) {
    const int *gv = &lvl.gridVerts[g * w * w];
    for (int v = 0; v < S; v++) {
      for (int u = 0; u < S; u++, f++) {
        mesh::FaceProxy face(&m, f);
        for (auto list : face.lists()) {
          for (auto c : list) {
            int j = 0;
            while (j < 4 && gv[(v + dv[j]) * w + (u + du[j])] != c.v()) {
              j++;
            }
            test_assert(j < 4);
            const float2 want = samples[g * w * w + (v + dv[j]) * w + (u + du[j])];
            test_assert((*uv)[c.i][0] == want[0] && (*uv)[c.i][1] == want[1]);
          }
        }
      }
    }
  }

  /* Colors ride the vertex domain, face sets the face domain. */
  AttrRef colRef = m.v.attrs.find_attribute(AttrType::FLOAT4, "color");
  test_assert(colRef.exists());
  const float4 *colSamples = mr.gridAttrs().colorSamples(1);
  test_assert(colSamples != nullptr);
  if (colSamples) {
    auto *col = colRef.get_data<float4>();
    for (int g = 0; g < mr.refiner.gridCount(); g++) {
      const int *gv = &lvl.gridVerts[g * w * w];
      for (int i = 0; i < w * w; i++) {
        test_assert((*col)[gv[i]][0] == colSamples[g * w * w + i][0]);
      }
    }
  }
  AttrRef grpRef = m.f.attrs.find_attribute(AttrType::INT, "group");
  test_assert(grpRef.exists());
  {
    auto *grp = grpRef.get_data<int>();
    for (int fi : m.f) {
      test_assert((*grp)[fi] == 7);
    }
  }

  alloc::Delete(cage);
}

/* A cage edit must drop the derived layer, not serve the stale one. */
static void gateInvalidation()
{
  Mesh *cage = makeGrid(2);
  {
    AttrRef &ref = cage->v.attrs.ensure(AttrType::FLOAT, "h", /*materialize=*/true);
    auto *d = ref.get_data<float>();
    for (int v : cage->v) {
      (*d)[v] = 1.0f;
    }
  }
  Multires mr;
  mr.init(*cage, 1);

  int comps = 0;
  const float *s = mr.gridAttrs().samples(1, "h", &comps);
  test_assert(s != nullptr && comps == 1 && s[0] == 1.0f);

  const uint64_t gen = mr.gridAttrs().generation();
  {
    auto *d = cage->v.attrs.find_attribute(AttrType::FLOAT, "h").get_data<float>();
    for (int v : cage->v) {
      (*d)[v] = 3.0f;
    }
  }
  mr.gridAttrs().invalidate("h");
  test_assert(mr.gridAttrs().generation() != gen);

  s = mr.gridAttrs().samples(1, "h", &comps);
  test_assert(s != nullptr && s[0] == 3.0f);

  /* An unknown attribute is a null answer, not a crash. */
  test_assert(mr.gridAttrs().samples(1, "nope", &comps) == nullptr);
  test_assert(comps == 0);

  alloc::Delete(cage);
}

/* The return route (Multires::scatterFaceIntToCage): a mesh-path face-set edit
 * lands on the SLOT's derived copy, and only this pushes it back onto the cage
 * that actually persists. Blender's rule is per base face and unweighted, so a
 * couple of painted cells claim their whole cage face — and the face's other
 * cells must come back uniform, or the next scatter would read them as a fresh
 * disagreement and revert what was just painted. */
static void gateCageScatter()
{
  Mesh *cage = makeGrid(2);
  cage->default_group_id = 1;
  {
    AttrRef &ref = cage->f.attrs.ensure(AttrType::INT, "group", /*materialize=*/true);
    auto *d = ref.get_data<int>();
    for (int f : cage->f) {
      (*d)[f] = 1;
    }
  }
  Multires mr;
  mr.init(*cage, 2);
  subdiv::MultiresSlot *slot = mr.setActiveLevel(2);
  test_assert(slot && slot->mesh);
  if (!slot || !slot->mesh) {
    alloc::Delete(cage);
    return;
  }
  Mesh &m = *slot->mesh;
  const int S = mr.refiner.levels[1].gridSide, cells = S * S;
  test_assert(m.f.count == mr.refiner.gridCount() * cells);

  /* Tint the whole cage before the edit, so the patch path (not a fresh build)
   * is what has to produce the new colours. */
  const float3 *cols = mr.gridAttrs().gridFaceSetColors();
  test_assert(cols != nullptr && cols[0][0] == 1.0f && cols[0][1] == 1.0f);
  const uint64_t gen = mr.gridAttrs().generation();

  /* Two cells of grid 0 only: a partial paint of cage face 0. */
  auto *sgrp = m.f.attrs.find_attribute(AttrType::INT, "group").get_data<int>();
  (*sgrp)[0] = 7;
  (*sgrp)[2] = 7;

  Vector<int> touched;
  test_assert(mr.scatterFaceIntToCage(2, "group", touched) == 1);

  /* All four grids of cage face 0, and nothing else. */
  test_assert(touched.size() == 4);
  for (int i = 0; i < 4; i++) {
    test_assert(touched[i] == i);
  }
  auto *cgrp = cage->f.attrs.find_attribute(AttrType::INT, "group").get_data<int>();
  int painted = 0;
  for (int f : cage->f) {
    if ((*cgrp)[f] == 7) {
      painted++;
    }
    else {
      test_assert((*cgrp)[f] == 1);
    }
  }
  test_assert(painted == 1);

  /* The slot came back uniform over the claimed face. */
  for (int f = 0; f < m.f.count; f++) {
    test_assert((*sgrp)[f] == (f < 4 * cells ? 7 : 1));
  }

  /* Only the touched grids re-tinted, and the generation did NOT move — a bump
   * would send the draw source through markAllData() and refill everything. */
  test_assert(mr.gridAttrs().generation() == gen);
  cols = mr.gridAttrs().gridFaceSetColors();
  test_assert(cols != nullptr);
  for (int g = 0; g < 4; g++) {
    test_assert(!(cols[g][0] == 1.0f && cols[g][1] == 1.0f && cols[g][2] == 1.0f));
  }
  test_assert(cols[4][0] == 1.0f && cols[4][1] == 1.0f && cols[4][2] == 1.0f);

  /* Idempotent: nothing disagrees any more, so a second pass proposes nothing.
   * (Without the re-stamp above this reverts cage face 0 to 1.) */
  test_assert(mr.scatterFaceIntToCage(2, "group", touched) == 0);
  test_assert(touched.size() == 0);
  test_assert((*cgrp)[0] == 7);

  alloc::Delete(cage);
}

/* Vec has no operator==, and these are exact copies, not a numeric result. */
static bool sameColor(const float4 &a, const float4 &b)
{
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/** Euclidean RGBA distance -- the "moved toward the mean" metric below. */
static float colorDist(const float4 &a, const float4 &b)
{
  float d = 0.0f;
  for (int i = 0; i < 4; i++) {
    d += (a[i] - b[i]) * (a[i] - b[i]);
  }
  return std::sqrt(d);
}

/* CS2's engine cage-dab entry (brush/cage_smooth.h): a colour-smoothing dab on
 * a multires level runs the generated mesh kernel over the cage colour layer
 * directly -- falloff in limit space, neighbours the cage 1-ring, masking from
 * the store's mask channel -- and its epilogue re-derives the touched grids so
 * the host's trailing scatter proposes nothing. */
static void gateCageColorSmooth()
{
  using subdiv::GridLevelDomain;

  Mesh *cage = makeGrid(2);
  {
    AttrRef &ref = cage->v.attrs.ensure(AttrType::FLOAT4, "color", /*materialize=*/true);
    ref.use = AttrUse::COLOR;
    auto *d = ref.get_data<float4>();
    for (int v : cage->v) {
      const float t = float(v + 1) / 16.0f;
      (*d)[v] = float4(t, 1.0f - t, 0.5f, 1.0f);
    }
    /* The linear ramp is a fixed point of ring averaging at the centre vert;
     * black it out so the smooth has somewhere to go. */
    (*d)[4] = float4(0.0f, 0.0f, 0.0f, 1.0f);
  }
  /* Lift one boundary corner: its cage and limit positions now disagree,
   * which is the discriminator for the space the kernel's falloff reads. */
  cage->v.co[0][2] = 1.5f;

  Multires mr;
  mr.init(*cage, 2);
  const int level = 2, grids = mr.refiner.gridCount();
  subdiv::MultiresSlot *slot = mr.setActiveLevel(level);
  test_assert(slot && slot->mesh && slot->tree);
  if (!slot || !slot->mesh || !slot->tree) {
    alloc::Delete(cage);
    return;
  }
  const subdiv::SubdivLevel &lvl = mr.refiner.levels[level - 1];
  const int w = lvl.gridSide + 1;

  Vector<int> gridVert;
  test_assert(mr.gridCageVerts(gridVert) && int(gridVert.size()) == grids);
  auto gridOf = [&](int v) {
    for (int g = 0; g < grids; g++) {
      if (gridVert[g] == v) {
        return g;
      }
    }
    return -1;
  };
  const int gC = gridOf(4), g0 = gridOf(0), g1 = gridOf(1);
  test_assert(gC >= 0 && g0 >= 0 && g1 >= 0);

  /* Mask one ring vert through the host's channel -- the route a session's
   * paint masking actually arrives by. */
  GridLevelDomain *dom = mr.gridDomain(level);
  test_assert(dom != nullptr);
  dom->mask[dom->gridVerts(g1)[0]] = 1.0f;
  dom->flushMaskToStore();

  auto *cageCol = cage->v.attrs.find_attribute(AttrType::FLOAT4, "color").get_data<float4>();
  test_assert(cageCol != nullptr);
  float4 before[9];
  for (int v = 0; v < 9; v++) {
    before[v] = (*cageCol)[v];
  }
  float4 ringAvg(0.0f, 0.0f, 0.0f, 0.0f);
  for (int v : {1, 3, 5, 7}) {
    for (int i = 0; i < 4; i++) {
      ringAvg[i] += 0.25f * before[v][i];
    }
  }

  const float3 centre = slot->mesh->v.co[lvl.gridVerts[size_t(gC) * w * w]];
  const float3 limit0 = slot->mesh->v.co[lvl.gridVerts[size_t(g0) * w * w]];

  const uint64_t gen = mr.gridAttrs().generation();
  const uint64_t cgen = mr.gridAttrs().cageGeneration();

  brush::Brush b;
  b.radius = 2.0f;
  b.strength = 0.35f;
  b.writeProps();
  const int tool = int(brush::SculptBrushes::COLORSMOOTH);

  {
    brush::CageSmoothSession s(&mr, level, &b);
    test_assert(s.begin("color"));

    const float dabA[7] = {centre[0], centre[1], centre[2], 0.0f, 0.0f, 1.0f, 2.0f};
    test_assert(s.dabBatch(tool, 1, dabA, 0.35f, false, 1.0f, false, nullptr, 0) > 0);

    /* The centre moved toward its cage 1-ring's mean -- the four edge
     * midpoints, not the grid lattice. */
    test_assert(!sameColor((*cageCol)[4], before[4]));
    test_assert(colorDist((*cageCol)[4], ringAvg) < colorDist(before[4], ringAvg));
    /* Verts 1 and 7 sit at the same lattice distance: the unmasked one
     * changed, the masked one held bit-still. */
    test_assert(!sameColor((*cageCol)[7], before[7]));
    test_assert(sameColor((*cageCol)[1], before[1]));

    /* A dab centred on the lifted corner's limit position, with a radius
     * shorter than the cage-to-limit gap, reaches the vert only if falloff
     * read the swapped-in limit snapshot rather than cage->v.co. */
    float gap = 0.0f;
    for (int i = 0; i < 3; i++) {
      gap += (cage->v.co[0][i] - limit0[i]) * (cage->v.co[0][i] - limit0[i]);
    }
    gap = std::sqrt(gap);
    test_assert(gap > 0.2f);
    const float4 corner0 = (*cageCol)[0];
    const float dabB[7] = {limit0[0], limit0[1], limit0[2], 0.0f, 0.0f, 1.0f, 0.5f * gap};
    test_assert(s.dabBatch(tool, 1, dabB, 0.5f, false, 1.0f, false, nullptr, 0) > 0);
    test_assert(!sameColor((*cageCol)[0], corner0));

    s.end();
  }

  /* The epilogue left every grid's (0, 0) sample a bit-exact copy of its cage
   * vert, so the host's trailing scatter is a no-op instead of an overwrite of
   * the kernel's fresh cage values with stale slot ones. */
  const float4 *samples = mr.gridAttrs().colorSamples(level);
  test_assert(samples != nullptr);
  if (samples) {
    for (int g = 0; g < grids; g++) {
      test_assert(sameColor(samples[size_t(g) * w * w], (*cageCol)[gridVert[g]]));
    }
  }
  Vector<int> tg;
  test_assert(mr.scatterVertFloat4ToCage(level, "color", nullptr, 0, tg) == 0);

  /* Kernel dabs are cage edits: no derived rebuild happened, and the cage
   * stamp is what tells every other level to re-derive on the way in. */
  test_assert(mr.gridAttrs().generation() == gen);
  test_assert(mr.gridAttrs().cageGeneration() > cgen);

  printf("cage colour smooth: %d grids\n", grids);
  alloc::Delete(cage);
}


/* B2a's vertex-domain twin of gateCageScatter. Blender has no multires
 * attribute domain, so painted colour has to land somewhere in the object's own
 * data or die with the session; the cage is that somewhere. The write is exact
 * restriction rather than a transpose: sample (0, 0) of a grid IS its corner's
 * cage vert, at weight one. */
static void gateCageVertScatter()
{
  Mesh *cage = makeGrid(2);
  {
    AttrRef &ref = cage->v.attrs.ensure(AttrType::FLOAT4, "color", /*materialize=*/true);
    ref.use = AttrUse::COLOR;
    auto *d = ref.get_data<float4>();
    for (int v : cage->v) {
      const float t = float(v + 1) / 16.0f;
      (*d)[v] = float4(t, 1.0f - t, 0.5f, 1.0f);
    }
  }
  Multires mr;
  mr.init(*cage, 2);

  const int level = 2, grids = mr.refiner.gridCount();
  subdiv::MultiresSlot *slot = mr.setActiveLevel(level);
  test_assert(slot && slot->mesh);
  if (!slot || !slot->mesh) {
    alloc::Delete(cage);
    return;
  }
  Mesh &m = *slot->mesh;
  const subdiv::SubdivLevel &lvl = mr.refiner.levels[level - 1];
  const int S = lvl.gridSide, w = S + 1;

  Vector<int> gridVert;
  test_assert(mr.gridCageVerts(gridVert));
  test_assert(int(gridVert.size()) == grids && grids == 16);

  /* A cage vert appears once per incident face: on a 2x2 quad grid that is four
   * corners once, four edge verts twice and the centre four times. */
  int seen[9] = {0};
  for (int g = 0; g < grids; g++) {
    test_assert(gridVert[g] >= 0 && gridVert[g] < 9);
    seen[gridVert[g]]++;
  }
  int once = 0, twice = 0, quad = 0;
  for (int i = 0; i < 9; i++) {
    once += seen[i] == 1;
    twice += seen[i] == 2;
    quad += seen[i] == 4;
  }
  test_assert(once == 4 && twice == 4 && quad == 1);

  /* The correspondence itself, checked against the rule that derives a level
   * rather than against the walk that produced the pairing. Blender's ptex
   * rule is bilinear within the grid, so sample (0, 0) is one-hot on the
   * grid's own corner: the derived sample there must be a bit-exact copy of
   * the cage value, which is what makes the write-back restriction and not a
   * least-squares fit. (Positions are no oracle here -- they follow the
   * Catmull-Clark limit stencil, which pulls a valence-one boundary corner
   * inward.) */
  {
    const float4 *samples = mr.gridAttrs().colorSamples(level);
    test_assert(samples != nullptr);
    auto *cageCol = cage->v.attrs.find_attribute(AttrType::FLOAT4, "color").get_data<float4>();
    test_assert(cageCol != nullptr);
    if (samples && cageCol) {
      for (int g = 0; g < grids; g++) {
        test_assert(sameColor(samples[g * w * w], (*cageCol)[gridVert[g]]));
      }
    }
  }

  const int col = Multires_gridChannelEnsure(&mr,
                                             "col",
                                             4,
                                             int(subdiv::GridElemDomain::Vertex),
                                             int(AttrType::FLOAT4),
                                             /*persist=*/1,
                                             int(subdiv::GridLevelRule::Authored));
  test_assert(col > 0);
  const int perGrid = Multires_gridChannelGridFloats(&mr, level, col);
  test_assert(perGrid == w * w * 4);
  Vector<float> buf;
  buf.resize(perGrid * grids);
  for (int i = 0; i < int(buf.size()); i++) {
    buf[i] = 1.0f;
  }
  const float4 C(0.25f, 0.5f, 0.75f, 1.0f);
  for (int k = 0; k < 4; k++) {
    buf[k] = C[k];
  }
  test_assert(Multires_gridChannelWrite(&mr, level, col, 0, grids, buf.data(), buf.size()) ==
              perGrid * grids);

  /* The scatter now reports the grids it re-derived (C1); this gate reads only
   * its return value, so the list is scratch. */
  Vector<int> tg;
  auto scatterVert = [&](const char *n) {
    tg.clear();
    return mr.scatterVertFloat4ToCage(level, n, nullptr, 0, tg);
  };

  /* One grid's corner disagrees with white, so exactly one cage vert changes --
   * and the other eight reading white is what proves the first-ever layer was
   * flooded with the host's default colour instead of the attribute system's
   * zero. */
  test_assert(scatterVert("col") == 1);
  AttrRef cref = cage->v.attrs.find_attribute(AttrType::FLOAT4, "col");
  test_assert(cref.exists());
  auto *cdata = cref.get_data<float4>();
  for (int v : cage->v) {
    const float4 want = v == gridVert[0] ? C : float4(1.0f, 1.0f, 1.0f, 1.0f);
    test_assert(sameColor((*cdata)[v], want));
  }

  /* Now paint every grid, with replicas of one cage vert agreeing -- which is
   * what a dab actually leaves behind, since gridAttrScatter writes through
   * every occurrence of a boundary sample. */
  for (int g = 0; g < grids; g++) {
    const float t = float(gridVert[g] + 1) / 16.0f;
    float *dst = &buf[g * perGrid];
    dst[0] = t;
    dst[1] = 1.0f - t;
    dst[2] = 0.5f;
    dst[3] = 1.0f;
  }
  test_assert(Multires_gridChannelWrite(&mr, level, col, 0, grids, buf.data(), buf.size()) ==
              perGrid * grids);
  test_assert(scatterVert("col") == 9);
  for (int v : cage->v) {
    const float t = float(v + 1) / 16.0f;
    test_assert(sameColor((*cdata)[v], float4(t, 1.0f - t, 0.5f, 1.0f)));
  }

  /* Idempotent: the replicas already agree, and the re-derive that follows the
   * write leaves the corners equal to the cage they were just written to, so
   * the second pass finds no disagreement to resolve. */
  test_assert(scatterVert("col") == 0);

  /* No store channel by that name: the materialized level's own column is the
   * fallback source, which is what a mesh-path stroke leaves behind. */
  {
    AttrRef &ref = m.v.attrs.ensure(AttrType::FLOAT4, "slotcol", /*materialize=*/true);
    auto *d = ref.get_data<float4>();
    for (int v : m.v) {
      (*d)[v] = float4(1.0f, 1.0f, 1.0f, 1.0f);
    }
    (*d)[lvl.gridVerts[0]] = float4(0.0f, 0.25f, 0.5f, 1.0f);
  }
  test_assert(scatterVert("slotcol") == 1);
  AttrRef sref = cage->v.attrs.find_attribute(AttrType::FLOAT4, "slotcol");
  test_assert(sref.exists());
  {
    auto *d = sref.get_data<float4>();
    for (int v : cage->v) {
      const float4 want = v == gridVert[0] ? float4(0.0f, 0.25f, 0.5f, 1.0f) :
                                             float4(1.0f, 1.0f, 1.0f, 1.0f);
      test_assert(sameColor((*d)[v], want));
    }
  }

  /* Both copies live at once, and they disagree. There is no source pick any
   * more (C3): the store channel wins wherever one exists, because writing
   * grid elements is the Host storage class's privilege and a Derived
   * attribute's mesh path never gets a channel. So painting the slot column of
   * a name that HAS a channel changes nothing -- outside this test the two are
   * never both live. */
  const float4 D(0.125f, 0.875f, 0.375f, 1.0f);
  const float4 held = (*cdata)[gridVert[0]];
  {
    AttrRef &ref = m.v.attrs.ensure(AttrType::FLOAT4, "col", /*materialize=*/true);
    auto *d = ref.get_data<float4>();
    for (int v : m.v) {
      (*d)[v] = float4(1.0f, 1.0f, 1.0f, 1.0f);
    }
    /* Through every occurrence of the vert, the way a dab leaves it. */
    for (int g = 0; g < grids; g++) {
      const int vert = gridVert[g];
      const float t = float(vert + 1) / 16.0f;
      (*d)[lvl.gridVerts[g * w * w]] = vert == gridVert[0] ?
                                           D :
                                           float4(t, 1.0f - t, 0.5f, 1.0f);
    }
  }
  test_assert(scatterVert("col") == 0);
  test_assert(sameColor((*cdata)[gridVert[0]], held));

  /* Refusals leave no half-built cage layer behind. */
  test_assert(scatterVert("nope") == 0);
  test_assert(!cage->v.attrs.has(AttrType::FLOAT4, "nope"));
  test_assert(mr.scatterVertFloat4ToCage(0, "col", nullptr, 0, tg) == 0);
  test_assert(mr.scatterVertFloat4ToCage(mr.maxLevel() + 1, "col", nullptr, 0, tg) == 0);

  /* C1: the collapse. A non-persisting channel is the Blender case -- colour is
   * a `Derived` attribute there, so the cage is the only author and nothing at
   * grid resolution may survive the scatter. */
  {
    const int dch = Multires_gridChannelEnsure(&mr,
                                               "dcol",
                                               4,
                                               int(subdiv::GridElemDomain::Vertex),
                                               int(AttrType::FLOAT4),
                                               /*persist=*/0,
                                               int(subdiv::GridLevelRule::Authored));
    test_assert(dch > 0);
    /* Every sample of every grid, corners included: a dab paints the whole
     * region it covers, not just the lattice corners. */
    for (int g = 0; g < grids; g++) {
      for (int i = 0; i < w * w; i++) {
        float *dst = &buf[g * perGrid + i * 4];
        const float t = float(g * w * w + i + 1) / float(grids * w * w + 1);
        dst[0] = t;
        dst[1] = 1.0f - t;
        dst[2] = 0.25f;
        dst[3] = 1.0f;
      }
    }
    test_assert(Multires_gridChannelWrite(&mr, level, dch, 0, grids, buf.data(), buf.size()) ==
                perGrid * grids);
    tg.clear();
    test_assert(mr.scatterVertFloat4ToCage(level, "dcol", nullptr, 0, tg) > 0);
    /* Every cage vert moved, so every grid of every face re-derives. */
    test_assert(int(tg.size()) == grids);

    int comps = 0;
    const float *after = mr.gridAttrs().samples(level, "dcol", &comps);
    test_assert(after && comps == 4);
    Vector<float> snap;
    snap.resize(size_t(grids) * w * w * 4);
    for (size_t i = 0; i < snap.size(); i++) {
      snap[i] = after[i];
    }

    /* The assertion that says nothing at grid resolution is authoritative:
     * drop the derived layer and rebuild it whole from the cage -- session
     * channel overlay included -- and every sample must come back identical.
     * It fails if the scatter left the channel holding the pre-collapse paint,
     * which the overlay would then reinstate. */
    mr.gridAttrs().invalidate("dcol");
    const float *rebuilt = mr.gridAttrs().samples(level, "dcol", &comps);
    test_assert(rebuilt && comps == 4);
    for (size_t i = 0; i < snap.size(); i++) {
      test_assert(snap[i] == rebuilt[i]);
    }
    /* And a second scatter has nothing to carry: the corners already agree
     * with the cage they were just written to. */
    tg.clear();
    test_assert(mr.scatterVertFloat4ToCage(level, "dcol", nullptr, 0, tg) == 0);
  }

  alloc::Delete(cage);
}

/* C1: a dab finer than a base face. It moves no cage vert, so the collapse has
 * nothing to find from the cage side -- and without the dab's own region its
 * paint would stay on the level slot, which is exactly what the viewport draws
 * during a mesh-path stroke. The rule is that a Derived attribute is never
 * authoritative at grid resolution: sub-face paint must come out as no paint,
 * not as paint that evaporates when a later dab moves a neighbour. */
static void gateSubFaceDabCollapse()
{
  Mesh *cage = makeGrid(2);
  {
    AttrRef &ref = cage->v.attrs.ensure(AttrType::FLOAT4, "color", /*materialize=*/true);
    ref.use = AttrUse::COLOR;
    auto *d = ref.get_data<float4>();
    for (int v : cage->v) {
      const float t = float(v + 1) / 16.0f;
      (*d)[v] = float4(t, 1.0f - t, 0.5f, 1.0f);
    }
  }
  Multires mr;
  mr.init(*cage, 2);

  const int level = 2;
  subdiv::MultiresSlot *slot = mr.setActiveLevel(level);
  test_assert(slot && slot->mesh && slot->tree);
  if (!slot || !slot->mesh || !slot->tree) {
    alloc::Delete(cage);
    return;
  }
  Mesh &m = *slot->mesh;
  const subdiv::SubdivLevel &lvl = mr.refiner.levels[level - 1];
  const int w = lvl.gridSide + 1;

  AttrRef sref = m.v.attrs.find_attribute(AttrType::FLOAT4, "color");
  test_assert(sref.exists());
  auto *sdata = sref.get_data<float4>();
  test_assert(sdata != nullptr);
  if (!sdata) {
    alloc::Delete(cage);
    return;
  }

  /* The middle of grid 0's lattice: a sample no cage vert aliases, so painting
   * it is the sub-face case by construction. */
  const int mid = w / 2;
  test_assert(mid > 0 && mid < w);
  const int vid = lvl.gridVerts[size_t(mid) * w + mid];
  const float4 before = (*sdata)[vid];
  const float4 paint(1.0f, 0.0f, 0.0f, 1.0f);
  test_assert(!sameColor(before, paint));
  (*sdata)[vid] = paint;

  /* A dab centred on that sample, radius a fraction of the cage cell so it
   * cannot reach a corner even if the leaf it lands in is wider. */
  const float3 co = m.v.co[vid];
  const float dab[4] = {co[0], co[1], co[2], 0.05f};

  Vector<int> tg;
  test_assert(mr.scatterVertFloat4ToCage(level, "color", dab, 1, tg) == 0);
  test_assert(tg.size() > 0);
  /* No cage vert moved... */
  auto *cdata = cage->v.attrs.find_attribute(AttrType::FLOAT4, "color").get_data<float4>();
  test_assert(cdata != nullptr);
  for (int v : cage->v) {
    const float t = float(v + 1) / 16.0f;
    test_assert(sameColor((*cdata)[v], float4(t, 1.0f - t, 0.5f, 1.0f)));
  }
  /* ...and the paint is gone from the copy the viewport draws. */
  test_assert(sameColor((*sdata)[vid], before));

  /* Without a region there is nothing to collapse, which is the state this
   * gate exists to rule out: paint it again, scatter with no dab, and the
   * slot still holds grid-resolution paint. */
  (*sdata)[vid] = paint;
  tg.clear();
  test_assert(mr.scatterVertFloat4ToCage(level, "color", nullptr, 0, tg) == 0);
  test_assert(sameColor((*sdata)[vid], paint));

  alloc::Delete(cage);
}

/* B1's channel c-api: the surface an embedding host actually consumes, so it
 * is graded the way one would use it -- enumerate, describe, write a level,
 * read it back, and survive a level round trip. */
static void gateChannelCapi()
{
  Mesh *cage = makeGrid(2);
  Multires mr;
  mr.init(*cage, 2);

  /* Channel 0 is disp: persistent, and the one Delta channel. */
  test_assert(Multires_gridChannelCount(&mr) >= 1);
  char nbuf[32] = {0};
  test_assert(Multires_gridChannelName(&mr, 0, nbuf, sizeof(nbuf)) == 4);
  test_assert(std::strcmp(nbuf, "disp") == 0);
  int fpe = 0, dom = 0, type = 0, persist = 0, rule = 0;
  test_assert(Multires_gridChannelInfo(&mr, 0, &fpe, &dom, &type, &persist, &rule));
  test_assert(fpe == 3 && persist == 1);
  test_assert(rule == int(subdiv::GridLevelRule::Delta));
  /* A short buffer truncates and still reports the real length. */
  char tiny[3] = {0};
  test_assert(Multires_gridChannelName(&mr, 0, tiny, 3) == 4 && std::strcmp(tiny, "di") == 0);
  test_assert(Multires_gridChannelName(&mr, 99, nbuf, sizeof(nbuf)) == -1);

  /* A host-persisted authored colour layer -- the case B1 exists for. */
  const int col = Multires_gridChannelEnsure(&mr,
                                             "col",
                                             4,
                                             int(subdiv::GridElemDomain::Vertex),
                                             int(AttrType::FLOAT4),
                                             /*persist=*/1,
                                             int(subdiv::GridLevelRule::Authored));
  test_assert(col > 0 && Multires_gridChannelFind(&mr, "col") == col);
  test_assert(Multires_gridChannelInfo(&mr, col, &fpe, &dom, &type, &persist, &rule));
  test_assert(fpe == 4 && persist == 1 && rule == int(subdiv::GridLevelRule::Authored));
  /* Re-declaring at another width is a failure, not a silent redefinition;
   * re-declaring the persistence is how a host claims a layer. */
  test_assert(Multires_gridChannelEnsure(&mr, "col", 2, int(subdiv::GridElemDomain::Vertex),
                                         int(AttrType::FLOAT4), 1,
                                         int(subdiv::GridLevelRule::Authored)) == -1);
  test_assert(Multires_gridChannelEnsure(&mr, "col", 4, int(subdiv::GridElemDomain::Vertex),
                                         int(AttrType::FLOAT4), 0,
                                         int(subdiv::GridLevelRule::Authored)) == col);
  test_assert(Multires_gridChannelInfo(&mr, col, nullptr, nullptr, nullptr, &persist, nullptr));
  test_assert(persist == 0);
  Multires_gridChannelEnsure(&mr, "col", 4, int(subdiv::GridElemDomain::Vertex),
                             int(AttrType::FLOAT4), 1, int(subdiv::GridLevelRule::Authored));

  const int level = 2, grids = mr.refiner.gridCount();
  const int perGrid = Multires_gridChannelGridFloats(&mr, level, col);
  const int S = mr.refiner.levels[level - 1].gridSide;
  test_assert(perGrid == (S + 1) * (S + 1) * 4);

  /* An untouched authored level reads back as zeros and stays unallocated:
   * a host must be able to ask what is there without paying for storage. */
  Vector<float> buf;
  buf.resize(perGrid * grids);
  for (int i = 0; i < int(buf.size()); i++) {
    buf[i] = -1.0f;
  }
  test_assert(!Multires_gridChannelLevelAllocated(&mr, level, col));
  test_assert(Multires_gridChannelRead(&mr, level, col, 0, grids, buf.data(), buf.size()) ==
              perGrid * grids);
  for (float f : buf) {
    test_assert(f == 0.0f);
  }
  test_assert(!Multires_gridChannelLevelAllocated(&mr, level, col));

  /* Write a per-float pattern, read it back. */
  for (int i = 0; i < int(buf.size()); i++) {
    buf[i] = float(i) * 0.5f + 1.0f;
  }
  test_assert(Multires_gridChannelWrite(&mr, level, col, 0, grids, buf.data(), buf.size()) ==
              perGrid * grids);
  test_assert(Multires_gridChannelLevelAllocated(&mr, level, col));
  Vector<float> back;
  back.resize(buf.size());
  test_assert(Multires_gridChannelRead(&mr, level, col, 0, grids, back.data(), back.size()) ==
              perGrid * grids);
  for (int i = 0; i < int(buf.size()); i++) {
    test_assert(back[i] == buf[i]);
  }
  /* A grid range is addressable on its own -- a host streams, it does not have
   * to hold a whole level. */
  Vector<float> one;
  one.resize(perGrid);
  test_assert(Multires_gridChannelRead(&mr, level, col, 1, 1, one.data(), one.size()) == perGrid);
  for (int i = 0; i < perGrid; i++) {
    test_assert(one[i] == buf[perGrid + i]);
  }
  /* Out of range and undersized buffers are refusals, not overruns. */
  test_assert(Multires_gridChannelRead(&mr, level, col, grids, 1, one.data(), one.size()) == 0);
  test_assert(Multires_gridChannelRead(&mr, level, col, 0, 1, one.data(), perGrid - 1) == 0);

  /* The level round trip an Authored channel promises: subdivide, delete
   * higher, and the paint at the surviving level is untouched. */
  mr.addLevel();
  mr.removeTopLevel();
  test_assert(Multires_gridChannelRead(&mr, level, col, 0, grids, back.data(), back.size()) ==
              perGrid * grids);
  int diffs = 0;
  for (int i = 0; i < int(buf.size()); i++) {
    diffs += back[i] != buf[i];
  }
  fprintf(stderr, "channel c-api: add/remove level diffs=%d\n", diffs);
  test_assert(diffs == 0);

  /* disp is not removable; the authored channel is. */
  test_assert(Multires_gridChannelRemove(&mr, 0) == 0);
  test_assert(Multires_gridChannelRemove(&mr, col) == 1);
  test_assert(Multires_gridChannelFind(&mr, "col") == -1);

  alloc::Delete(cage);
}

/* C4: a paint edit at a fine level reaches every level below it on a downward
 * level switch, the way a position edit does — but per channel, and exactly
 * once.
 *
 * The debt is what makes "exactly once" possible: restriction is not the
 * inverse of subdivision, so re-running it on a level that is already current
 * would replace the coarse level's own paint with a blurred copy of what it
 * seeded upward. The same argument is why the debt is per channel and not per
 * level: a mask edit at level 3 must not drag an untouched colour layer through
 * the filter. */
/* A cage write-back at one level leaves every OTHER resident level's derived
 * copy behind: materialize() hands back a slot it found without re-deriving,
 * so a slot built before the paint still carries pre-paint colour. That is not
 * just a display gap -- scatterVertFloat4ToCage READS the level mesh, so
 * painting on a stale slot writes stale colour back onto the cage and reverts
 * the finer level's paint. */
static void gateResidentSlotFreshness()
{
  Mesh *cage = makeGrid(2);
  {
    AttrRef &ref = cage->v.attrs.ensure(AttrType::FLOAT4, "color", /*materialize=*/true);
    ref.use = AttrUse::COLOR;
    auto *d = ref.get_data<float4>();
    for (int v : cage->v) {
      (*d)[v] = float4(1.0f, 1.0f, 1.0f, 1.0f);
    }
  }
  Multires mr;
  mr.init(*cage, 3);

  /* Level 2 is visited first, so its slot predates the paint. */
  subdiv::MultiresSlot *coarse = mr.setActiveLevel(2);
  test_assert(coarse && coarse->mesh);
  subdiv::MultiresSlot *fine = mr.setActiveLevel(3);
  test_assert(fine && fine->mesh);
  if (!coarse || !fine || !coarse->mesh || !fine->mesh) {
    alloc::Delete(cage);
    return;
  }

  Vector<int> gridVert;
  test_assert(mr.gridCageVerts(gridVert));
  const int movedCageVert = gridVert.size() > 0 ? gridVert[0] : -1;
  test_assert(movedCageVert >= 0);

  /* Paint grid 0's corner sample at level 3 and push it onto the cage. */
  const float4 painted(0.25f, 0.5f, 0.75f, 1.0f);
  {
    const subdiv::SubdivLevel &lvl = mr.refiner.levels[3 - 1];
    const int w = lvl.gridSide + 1;
    auto *fcol = fine->mesh->v.attrs.find_attribute(AttrType::FLOAT4, "color").get_data<float4>();
    test_assert(fcol != nullptr);
    if (!fcol) {
      alloc::Delete(cage);
      return;
    }
    (*fcol)[lvl.gridVerts[0 * w * w]] = painted;
  }
  Vector<int> touched;
  test_assert(mr.scatterVertFloat4ToCage(3, "color", nullptr, 0, touched) > 0);
  {
    auto *cageCol = cage->v.attrs.find_attribute(AttrType::FLOAT4, "color").get_data<float4>();
    test_assert(cageCol && sameColor((*cageCol)[movedCageVert], painted));
  }

  /* Back to the level that was resident all along. Its mesh is what the mesh
   * path reads and what the next scatter would push onto the cage, so it has
   * to carry the paint -- not the white it was derived with. */
  subdiv::MultiresSlot *again = mr.setActiveLevel(2);
  test_assert(again == coarse); /* still resident: the staleness window */
  {
    const subdiv::SubdivLevel &lvl = mr.refiner.levels[2 - 1];
    const int w = lvl.gridSide + 1;
    auto *ccol = again->mesh->v.attrs.find_attribute(AttrType::FLOAT4, "color").get_data<float4>();
    test_assert(ccol != nullptr);
    if (ccol) {
      test_assert(sameColor((*ccol)[lvl.gridVerts[0 * w * w]], painted));
    }
  }

  /* And the round trip does not revert the cage: scattering the untouched
   * coarse level writes back what it now holds, which agrees. */
  Vector<int> tg2;
  mr.scatterVertFloat4ToCage(2, "color", nullptr, 0, tg2);
  {
    auto *cageCol = cage->v.attrs.find_attribute(AttrType::FLOAT4, "color").get_data<float4>();
    test_assert(cageCol && sameColor((*cageCol)[movedCageVert], painted));
  }

  printf("resident slot freshness: coarse level followed the cage write-back\n");
  alloc::Delete(cage);
}

static void gateAttrDownPropagation()
{
  using subdiv::GridElemDomain;
  using subdiv::GridLevelRule;

  Mesh *cage = makeGrid(2);
  Multires mr;
  mr.init(*cage, 3);
  auto &st = mr.store;
  const int col = st.addChannel(util::string("col"), 4, GridElemDomain::Vertex,
                                AttrType::FLOAT4, true, GridLevelRule::Authored);
  const int other = st.addChannel(util::string("other"), 4, GridElemDomain::Vertex,
                                  AttrType::FLOAT4, true, GridLevelRule::Authored);
  const int grids = mr.refiner.gridCount();
  const int wf = subdiv::GridsStore::elemWidth(3, GridElemDomain::Vertex);
  const int w2 = subdiv::GridsStore::elemWidth(2, GridElemDomain::Vertex);

  mr.setActiveLevel(3);
  /* Both channels are painted at the finest level; only `col` is reported. */
  for (int g = 0; g < grids; g++) {
    for (int v = 0; v < wf; v++) {
      for (int u = 0; u < wf; u++) {
        for (int k = 0; k < 4; k++) {
          const float f = std::sin(float(g) + float(u) * 0.21f + float(v) * 0.37f + float(k));
          st.elem(3, col, g, u, v)[k] = f;
          st.elem(3, other, g, u, v)[k] = f;
        }
      }
    }
  }
  mr.noteAttrEdit(3, col);
  test_assert(st.channelLevelDebt(3, col) && !st.channelLevelDebt(3, other));
  test_assert(st.anyChannelLevelDebt(3));

  /* `other` holds paint of its own one level down, which nothing may disturb. */
  for (int g = 0; g < grids; g++) {
    for (int v = 0; v < w2; v++) {
      for (int u = 0; u < w2; u++) {
        for (int k = 0; k < 4; k++) {
          st.elem(2, other, g, u, v)[k] = 0.5f;
        }
      }
    }
  }

  /* Down two levels: the debt is settled on the way, one step at a time. */
  mr.setActiveLevel(1);
  test_assert(!st.anyChannelLevelDebt(3) && !st.anyChannelLevelDebt(2));
  int moved = 0;
  for (int g = 0; g < grids; g++) {
    for (int v = 0; v < w2; v++) {
      for (int u = 0; u < w2; u++) {
        for (int k = 0; k < 4; k++) {
          moved += st.elem(2, col, g, u, v)[k] != 0.0f ? 1 : 0;
          /* Untouched channel, untouched level: still the paint we wrote. */
          test_assert(st.elem(2, other, g, u, v)[k] == 0.5f);
        }
      }
    }
  }
  test_assert(moved > 0);

  /* Exactly once: with the debt settled, an up-then-down round trip must leave
   * the coarse levels bit-identical. (A varying field, so a second pass of the
   * filter would show; a constant is a fixed point and would prove nothing.) */
  Vector<float> snap;
  for (int l = 1; l <= 2; l++) {
    const int wl = subdiv::GridsStore::elemWidth(l, GridElemDomain::Vertex);
    for (int g = 0; g < grids; g++) {
      for (int v = 0; v < wl; v++) {
        for (int u = 0; u < wl; u++) {
          for (int k = 0; k < 4; k++) {
            snap.append(st.elem(l, col, g, u, v)[k]);
          }
        }
      }
    }
  }
  mr.setActiveLevel(3);
  mr.setActiveLevel(1);
  int idx = 0, diffs = 0;
  for (int l = 1; l <= 2; l++) {
    const int wl = subdiv::GridsStore::elemWidth(l, GridElemDomain::Vertex);
    for (int g = 0; g < grids; g++) {
      for (int v = 0; v < wl; v++) {
        for (int u = 0; u < wl; u++) {
          for (int k = 0; k < 4; k++) {
            diffs += st.elem(l, col, g, u, v)[k] != snap[idx++] ? 1 : 0;
          }
        }
      }
    }
  }
  test_assert(diffs == 0);

  /* The debt is not derivable from the store, so an undo snapshot carries it —
   * by channel name, since an index outlives nothing. */
  mr.setActiveLevel(3);
  mr.noteAttrEdit(3, col);
  int size = 0;
  uint8_t *blob = Multires_serializeStore(&mr, &size);
  test_assert(blob && size > 0);
  st.setChannelLevelDebt(3, col, false);
  test_assert(!st.anyChannelLevelDebt(3));
  test_assert(Multires_restoreStore(&mr, blob, size) == 1);
  freeMeshBuffer(blob);
  test_assert(st.channelLevelDebt(3, st.findChannel(util::string("col"))));
  test_assert(!st.channelLevelDebt(3, st.findChannel(util::string("other"))));

  printf("attr down-propagation: %d grids, %d coarse samples moved\n", grids, moved);
  alloc::Delete(cage);
}

/* Independent transcription of the seed 4-tap, run per grid on dense lattices
 * (delta fields here) so the edit gate is not a snapshot of the code under
 * test. */
static void prolongRef(const Vector<float> &coarse, int wc, Vector<float> &fine, int wf)
{
  fine.resize(size_t(wf) * wf);
  for (int v = 0; v < wf; v++) {
    for (int u = 0; u < wf; u++) {
      const int cu = u >> 1, cv = v >> 1, du = u & 1, dv = v & 1;
      fine[size_t(v) * wf + u] = 0.25f * (coarse[size_t(cv) * wc + cu] +
                                          coarse[size_t(cv) * wc + cu + du] +
                                          coarse[size_t(cv + dv) * wc + cu] +
                                          coarse[size_t(cv + dv) * wc + cu + du]);
    }
  }
}

/* MK1 (grids-native completion): the mask edit contract. A whole-domain flush
 * is a SEED and reaches no other level; a touched-verts flush is an EDIT whose
 * delta prolongates into every finer level (added onto authored detail, alive
 * finer domains re-mirrored) while the level below takes down-debt. Plus the
 * add/drop round trip and the seed-on-unallocated-finer branch. */
static void gateMaskEditPropagation()
{
  using subdiv::GridElemDomain;
  using subdiv::GridLevelDomain;

  Mesh *cage = makeGrid(2);
  Multires mr;
  mr.init(*cage, 3);
  auto &st = mr.store;
  const int grids = st.gridCount();
  const int w2 = subdiv::GridsStore::elemWidth(2, GridElemDomain::Vertex);
  const int w3 = subdiv::GridsStore::elemWidth(3, GridElemDomain::Vertex);

  /* Author fine-lattice detail at the top via the seed flush. */
  GridLevelDomain *d3 = mr.gridDomain(3);
  for (int v = 0; v < d3->vertCount(); v++) {
    d3->mask[v] = 0.05f * float(v % 7);
  }
  d3->flushMaskToStore();
  const int ch = st.findChannel(util::string(GridLevelDomain::kMaskChannelName));
  test_assert(ch >= 0);
  /* A seed reaches no other level and owes nothing. */
  test_assert(!st.channelLevelAllocated(2, ch));
  test_assert(!st.anyChannelLevelDebt(3) && !st.anyChannelLevelDebt(2));

  Vector<float> top0;
  for (int g = 0; g < grids; g++) {
    for (int v = 0; v < w3; v++) {
      for (int u = 0; u < w3; u++) {
        top0.append(*st.elem(3, ch, g, u, v));
      }
    }
  }

  /* Edit every slot of grid 0 at level 2 through the edit path. */
  GridLevelDomain *d2 = mr.gridDomain(2);
  const uint64_t gen = mr.domainGeneration();
  Vector<int> edited;
  const int *gv = d2->gridVerts(0);
  for (int i = 0; i < w2 * w2; i++) {
    edited.append(gv[i]);
  }
  for (int i = 0; i < int(edited.size()); i++) {
    d2->mask[edited[i]] = 0.4f + 0.02f * float(i);
  }
  d2->flushMaskToStore(std::span<const int>(edited.data(), edited.size()));
  test_assert(mr.domainGeneration() == gen); /* neither domain was rebuilt */
  test_assert(st.channelLevelDebt(2, ch));

  /* Reference: dense level-2 delta per grid (level 2 held zeros, so the delta
   * is the new value, landed on every seam replica), prolonged one step. */
  Vector<float> delta2;
  delta2.resize(size_t(grids) * w2 * w2);
  for (int i = 0; i < int(delta2.size()); i++) {
    delta2[i] = 0.0f;
  }
  for (int i = 0; i < int(edited.size()); i++) {
    auto occs = d2->occurrences(edited[i]);
    for (size_t j = 0; j < occs.size(); j += 3) {
      delta2[(size_t(occs[j]) * w2 + occs[j + 2]) * w2 + occs[j + 1]] = d2->mask[edited[i]];
    }
  }
  Vector<float> gslice, fine;
  gslice.resize(size_t(w2) * w2);
  int bad = 0, moved = 0;
  for (int g = 0; g < grids; g++) {
    for (int i = 0; i < w2 * w2; i++) {
      gslice[i] = delta2[size_t(g) * w2 * w2 + i];
    }
    prolongRef(gslice, w2, fine, w3);
    for (int v = 0; v < w3; v++) {
      for (int u = 0; u < w3; u++) {
        const float expect = top0[(size_t(g) * w3 + v) * w3 + u] + fine[size_t(v) * w3 + u];
        const float got = *st.elem(3, ch, g, u, v);
        bad += std::fabs(got - expect) > 1e-6f ? 1 : 0;
        moved += fine[size_t(v) * w3 + u] != 0.0f ? 1 : 0;
      }
    }
  }
  test_assert(bad == 0);   /* detail + prolonged delta, everywhere */
  test_assert(moved > 0);  /* the edit actually reached the top */

  /* The alive finer domain re-mirrored the store. */
  int mirrorBad = 0;
  for (int v = 0; v < d3->vertCount(); v++) {
    auto occs = d3->occurrences(v);
    mirrorBad += d3->mask[v] != *st.elem(3, ch, occs[0], occs[1], occs[2]) ? 1 : 0;
  }
  test_assert(mirrorBad == 0);

  /* A later SEED at level 2 still reaches no other level and notes nothing. */
  st.setChannelLevelDebt(2, ch, false);
  Vector<float> top1;
  for (int g = 0; g < grids; g++) {
    for (int v = 0; v < w3; v++) {
      for (int u = 0; u < w3; u++) {
        top1.append(*st.elem(3, ch, g, u, v));
      }
    }
  }
  for (int v = 0; v < d2->vertCount(); v++) {
    d2->mask[v] = 0.25f;
  }
  d2->flushMaskToStore();
  int seedBad = 0, idx = 0;
  for (int g = 0; g < grids; g++) {
    for (int v = 0; v < w3; v++) {
      for (int u = 0; u < w3; u++) {
        seedBad += *st.elem(3, ch, g, u, v) != top1[idx++] ? 1 : 0;
      }
    }
  }
  test_assert(seedBad == 0);
  test_assert(!st.channelLevelDebt(2, ch));

  /* Add/drop round trip: the new top is prolonged on the way in, injected
   * back on the way out — bit-identical at the level that survives. */
  st.addLevel();
  test_assert(st.channelLevelAllocated(4, ch));
  st.dropTopLevel();
  int rtBad = 0;
  idx = 0;
  for (int g = 0; g < grids; g++) {
    for (int v = 0; v < w3; v++) {
      for (int u = 0; u < w3; u++) {
        rtBad += *st.elem(3, ch, g, u, v) != top1[idx++] ? 1 : 0;
      }
    }
  }
  test_assert(rtBad == 0);

  /* An edit under a finer level nothing ever authored: the finer level seeds
   * whole from the post-edit level below (its implicit zeros want exactly the
   * prolongation), instead of keeping a delta-only ghost. */
  Mesh *cage2 = makeGrid(2);
  Multires m2;
  m2.init(*cage2, 3);
  auto &s2 = m2.store;
  GridLevelDomain *e2 = m2.gridDomain(2);
  Vector<int> ed2;
  const int *gv2 = e2->gridVerts(1);
  for (int i = 0; i < w2 * w2; i++) {
    ed2.append(gv2[i]);
  }
  for (int i = 0; i < int(ed2.size()); i++) {
    e2->mask[ed2[i]] = 0.1f + 0.03f * float(i);
  }
  e2->flushMaskToStore(std::span<const int>(ed2.data(), ed2.size()));
  const int ch2 = s2.findChannel(util::string(GridLevelDomain::kMaskChannelName));
  test_assert(ch2 >= 0 && s2.channelLevelAllocated(3, ch2));
  int seed2Bad = 0;
  for (int g = 0; g < grids; g++) {
    for (int i = 0; i < w2 * w2; i++) {
      gslice[i] = *s2.elem(2, ch2, g, i % w2, i / w2);
    }
    prolongRef(gslice, w2, fine, w3);
    for (int v = 0; v < w3; v++) {
      for (int u = 0; u < w3; u++) {
        seed2Bad += std::fabs(*s2.elem(3, ch2, g, u, v) - fine[size_t(v) * w3 + u]) > 1e-6f ? 1 : 0;
      }
    }
  }
  test_assert(seed2Bad == 0);

  printf("mask edit propagation: %d grids, %d fine samples moved\n", grids, moved);
  alloc::Delete(cage);
  alloc::Delete(cage2);
}

/* MK4 (grids-native completion): the mask sync protocol. The store's mask
 * channel is the one truth and Multires::maskGeneration() is how caches learn
 * it moved: every content change — seed flush, edit flush, raw channel write,
 * blob restore, down-propagation settle, level restack — bumps it, a non-mask
 * channel write does not, and the settle also refreshes the coarser alive
 * domain's dense mirror in place (hosts refetch the cached domain without a
 * rebuild, so a stale mirror would survive a pointer-equal refetch). */
static void gateMaskSyncProtocol()
{
  using subdiv::GridLevelDomain;

  Mesh *cage = makeGrid(2);
  Multires mr;
  mr.init(*cage, 3);
  auto &st = mr.store;
  const int grids = st.gridCount();

  /* Starts nonzero so a host cache initialized to 0 always refreshes. */
  uint64_t gen = mr.maskGeneration();
  test_assert(gen != 0);
  test_assert(Multires_maskGeneration(&mr) == gen);
  test_assert(Multires_maskGeneration(nullptr) == 0);

  /* Seed flush bumps. */
  GridLevelDomain *d3 = mr.gridDomain(3);
  for (int v = 0; v < d3->vertCount(); v++) {
    d3->mask[v] = 0.1f + 0.04f * float(v % 5);
  }
  d3->flushMaskToStore();
  test_assert(mr.maskGeneration() > gen);
  gen = mr.maskGeneration();
  const int ch = st.findChannel(util::string(GridLevelDomain::kMaskChannelName));
  test_assert(ch >= 0);

  /* Edit flush bumps (and takes level-3 debt for the settle below). */
  Vector<int> edited;
  const int *gv = d3->gridVerts(0);
  const int w3 = subdiv::GridsStore::elemWidth(3, subdiv::GridElemDomain::Vertex);
  for (int i = 0; i < w3 * w3; i++) {
    edited.append(gv[i]);
  }
  for (int i = 0; i < int(edited.size()); i++) {
    d3->mask[edited[i]] = 0.6f + 0.01f * float(i % 9);
  }
  d3->flushMaskToStore(std::span<const int>(edited.data(), edited.size()));
  test_assert(mr.maskGeneration() > gen);
  gen = mr.maskGeneration();
  test_assert(st.channelLevelDebt(3, ch));

  /* Settle: the restriction moves level 2's store content underneath the
   * alive domain there, whose mirror must be refreshed IN PLACE. */
  GridLevelDomain *d2 = mr.gridDomain(2);
  const uint64_t dgen = mr.domainGeneration();
  test_assert(mr.propagateAttrsDown(3) > 0);
  test_assert(mr.domainGeneration() == dgen); /* refreshed, not rebuilt */
  test_assert(mr.maskGeneration() > gen);
  gen = mr.maskGeneration();
  int mirrorBad = 0, coarseMoved = 0;
  for (int v = 0; v < d2->vertCount(); v++) {
    auto occs = d2->occurrences(v);
    mirrorBad += d2->mask[v] != *st.elem(2, ch, occs[0], occs[1], occs[2]) ? 1 : 0;
    coarseMoved += d2->mask[v] != 0.0f ? 1 : 0;
  }
  test_assert(mirrorBad == 0);
  test_assert(coarseMoved > 0);

  /* Raw channel write on the mask channel bumps and re-mirrors the alive
   * domain at the written level. */
  const int perGrid = Multires_gridChannelGridFloats(&mr, 3, ch);
  test_assert(perGrid == w3 * w3);
  Vector<float> flat;
  flat.resize(size_t(perGrid));
  for (int i = 0; i < perGrid; i++) {
    flat[i] = 0.75f;
  }
  test_assert(Multires_gridChannelWrite(&mr, 3, ch, 0, 1, flat.data(), perGrid) == perGrid);
  test_assert(mr.maskGeneration() > gen);
  gen = mr.maskGeneration();
  int wrBad = 0;
  for (int i = 0; i < w3 * w3; i++) {
    wrBad += d3->mask[gv[i]] != 0.75f ? 1 : 0;
  }
  test_assert(wrBad == 0);

  /* A non-mask channel write is not a mask change. */
  const int sc = Multires_gridChannelEnsure(&mr, "scratch", 1, 0, int(AttrType::FLOAT), 1, 1);
  test_assert(sc >= 0);
  test_assert(Multires_gridChannelWrite(&mr, 3, sc, 0, 1, flat.data(), perGrid) == perGrid);
  test_assert(mr.maskGeneration() == gen);

  /* Blob restore replaced the channels: bump. */
  int size = 0;
  uint8_t *blob = Multires_serializeStore(&mr, &size);
  test_assert(blob && size > 0);
  test_assert(Multires_restoreStore(&mr, blob, size) == 1);
  freeMeshBuffer(blob);
  test_assert(mr.maskGeneration() > gen);
  gen = mr.maskGeneration();

  /* Level restacks change the channel's level roster: bump both ways. */
  mr.addLevel();
  test_assert(mr.maskGeneration() > gen);
  gen = mr.maskGeneration();
  mr.removeTopLevel();
  test_assert(mr.maskGeneration() > gen);

  printf("mask sync protocol: %d grids, gen %llu\n", grids,
         (unsigned long long)mr.maskGeneration());
  alloc::Delete(cage);
}

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  gateStoragePolicy();
  gateBilinearPoint(1);
  gateBilinearPoint(2);
  gateFaceVaryingUv(UvSmooth::PreserveBoundaries, false);
  gateFaceVaryingUv(UvSmooth::PreserveBoundaries, true);
  gateFaceVaryingUv(UvSmooth::None, true);
  gateFaceVaryingIsNotBilinear();
  gateFaceSetColors();
  gateSlotAttrs();
  gateInvalidation();
  gateCageScatter();
  gateCageVertScatter();
  gateCageColorSmooth();
  gateSubFaceDabCollapse();
  gateChannelCapi();
  gateResidentSlotFreshness();
  gateAttrDownPropagation();
  gateMaskEditPropagation();
  gateMaskSyncProtocol();

  return test_end();
}
