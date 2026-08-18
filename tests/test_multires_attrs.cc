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
#include "subdiv/grid_attrs.h"
#include "mesh/mesh_proxy.h"
#include "subdiv/multires.h"
#include "subdiv/subdiv.h"
#include "vdm/vdm_store.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>

test_init;

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

  return test_end();
}
