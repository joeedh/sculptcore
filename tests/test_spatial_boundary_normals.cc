// Boundary-vert normals across spatial leaves. Per-leaf normal updates are
// owner-gated, so without the skirt (cached neighbor-owned fan faces) and the
// cross-boundary halo (hinting owners of foreign verts on dirty tris), verts on
// leaf-ownership boundaries end up with partial fans after sculpting. This test
// deforms a multi-leaf grid with strokes and asserts every live vert's normal
// matches a whole-mesh reference computed with the same accumulation algorithm
// (raw area-weighted tri normals, fan triangulation, normalized).
#include "test_util.h"

#include "brush/brush_executor.h"
#include "debug/scene.h"
#include "debug/script.h"
#include "litestl/math/geom.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

test_init;

static constexpr float kDeg = 3.14159265f / 180.0f;

using namespace sculptcore;
using namespace sculptcore::debug_app;
using namespace sculptcore::mesh;
using litestl::math::float3;
using litestl::math::triNormal;
using litestl::util::Vector;

// Whole-mesh vert normals with the exact spatial-tree algorithm: sum the raw
// (area-weighted) normal of every fan tri into its three corner verts, then
// normalize. Fan triangulation matches appendFaceTris (tris + quads).
static void referenceNormals(Mesh *m, Vector<float3> &out)
{
  // Post-stroke topology can still be frozen (link columns dropped); the fan
  // walk below needs them live. Thawing rebuilds links, never touches v.no.
  if (m->topo_frozen) {
    m->thawTopo();
  }
  out.resize(m->v.capacity());
  for (int i = 0; i < int(out.size()); i++) {
    out[i] = float3{0, 0, 0};
  }

  for (int f : m->f) {
    int l = m->f.l[f];
    int c0 = m->l.c[l];
    int c1 = m->c.next[c0];
    int c2 = m->c.next[c1];

    auto accum = [&](int ca, int cb, int cc) {
      int v1 = m->c.v[ca], v2 = m->c.v[cb], v3 = m->c.v[cc];
      float3 n = triNormal(m->v.co[v1], m->v.co[v2], m->v.co[v3]);
      out[v1] += n;
      out[v2] += n;
      out[v3] += n;
    };
    accum(c0, c1, c2);
    if (m->l.size[l] > 3) {
      accum(c0, c2, m->c.next[c2]);
    }
  }

  for (int v : m->v) {
    out[v].normalize();
  }
}

// Compare live v.no against the reference; returns the mismatch count and
// reports the worst offender.
static int compareNormals(Mesh *m, const Vector<float3> &ref, const char *label)
{
  int bad = 0;
  float worst = 0.0f;
  int worstV = -1;
  for (int v : m->v) {
    float d = (m->v.no[v] - ref[v]).length();
    if (d > worst) {
      worst = d;
      worstV = v;
    }
    if (d > 2e-3f) {
      bad++;
    }
  }
  fprintf(
      stderr, "%s: %d mismatched normals, worst=%g at v=%d\n", label, bad, worst, worstV);
  return bad;
}

static void roughnessProbe()
{
  // --- Part 4: interleaved frame updates (the interactive cadence) ---------
  // The app runs update(gpu) every rendered frame BETWEEN dabs of one stroke —
  // the halo/normals/GPU phases interleave with dabs mid-stroke. Drive that
  // cadence at the executor level (masked dyntopo stroke, a frame update after
  // every dab), undo, restroke; then hold determinism and boundary-smoothness
  // to the same bar as the batched flows above.
  {
    // Surface roughness from positions alone: mean/max dihedral angle across
    // interior edges whose verts lie in the stroked band. Independent of vert
    // identity (dyntopo recycles indices) and of v.no.
    auto roughness = [](Scene &s, float &meanOut, float &maxOut) {
      Mesh *m = s.mesh;
      if (m->topo_frozen) {
        m->thawTopo();
      }
      const float3 center{0.15f, 0.0f, 0.17f};
      double sum = 0.0;
      int n = 0;
      float mx = 0.0f;
      for (int e : m->e) {
        int a = m->e.vs[e][0];
        int b = m->e.vs[e][1];
        if ((m->v.co[a] - center).length() > 0.22f ||
            (m->v.co[b] - center).length() > 0.22f)
        {
          continue;
        }
        int c0 = m->e.c[e];
        if (c0 == ELEM_NONE) {
          continue;
        }
        int fA = ELEM_NONE, fB = ELEM_NONE, c = c0;
        do {
          int f = m->l.f[m->c.l[c]];
          if (f != fA && f != fB) {
            if (fA == ELEM_NONE) {
              fA = f;
            } else if (fB == ELEM_NONE) {
              fB = f;
            }
          }
          c = m->c.radial_next[c];
        } while (c != c0 && c != ELEM_NONE);
        if (fA == ELEM_NONE || fB == ELEM_NONE) {
          continue;
        }
        auto faceN = [&](int f) {
          int l = m->f.l[f];
          int ca = m->l.c[l];
          int cb = m->c.next[ca];
          int cc = m->c.next[cb];
          return triNormal(m->v.co[m->c.v[ca]], m->v.co[m->c.v[cb]], m->v.co[m->c.v[cc]]);
        };
        float3 n1 = faceN(fA), n2 = faceN(fB);
        float l1 = n1.length(), l2 = n2.length();
        if (l1 < 1e-12f || l2 < 1e-12f) {
          continue;
        }
        float d = n1.dot(n2) / (l1 * l2);
        d = d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d);
        float ang = std::acos(d);
        sum += ang;
        n++;
        mx = std::fmax(mx, ang);
      }
      meanOut = n ? float(sum / n) : 0.0f;
      maxOut = mx;
    };

    // framePerDab=true mirrors the interactive cadence (update(gpu) between
    // dabs); false is the batched headless cadence that already passed above.
    auto runScenario =
        [&](bool framePerDab, bool maskOn, float &meanRough, float &maxRough) {
          Scene s(128, 128, /*headless=*/true);
          auto r = script::run(s,
                               "make_cube subdivs=100 size=0.5 sphere=1\n"
                               "build_spatial leaf_limit=64 depth_limit=16\n"
                               "set_brush radius=0.15 strength=0.5\n"
                               "set_brush_tool tool=draw\n"
                               "dyntopo enabled=1 detail=0.012 flip=0 smooth=0\n",
                               ".");
          test_assert(r.ok);
          Mesh *m = s.mesh;
          m->recalc_normals();
          s.tree->update(&s.gpu);
          s.brush.automask_view_normal = maskOn;
          s.brush.cull_backfaces = false;
          s.brush.viewDir = float3{0, 0, -1};
          s.brush.view_normal_falloff = 75.0f * kDeg;
          s.brush.writeProps();

          const float3 normal{0.707f, 0, 0.707f};
          auto strokeOnce = [&]() {
            uint32_t gen = ++s.strokeGen;
            brush::CommandExecutor exec(s.tree, &s.brush);
            exec.meshLog = &s.meshLog;
            exec.setNonAccum(true);
            exec.setStrokeGen(int(gen));
            exec.beginStep(true);
            const int NDABS = 8;
            for (int d = 0; d < NDABS; d++) {
              float t = float(d) / float(NDABS - 1);
              float3 c{0.15f, -0.1f + 0.2f * t, 0.17f};
              exec.applyDab(s.currentTool,
                            c,
                            normal,
                            s.brush.radius,
                            &s.dyntopoParams,
                            s.dyntopoSeed + uint32_t(d));
              if (framePerDab) {
                s.tree->update(&s.gpu);
              }
            }
            exec.endDynTopoStroke();
            exec.endStep();
            s.tree->update(&s.gpu);
          };

          strokeOnce();
          s.meshLog.undo(m, s.tree);
          s.tree->update(&s.gpu);
          strokeOnce();
          roughness(s, meanRough, maxRough);
        };

    float meanBatched = 0, maxBatched = 0, meanFrames = 0, maxFrames = 0;
    float meanNoMask = 0, maxNoMask = 0;
    runScenario(false, true, meanBatched, maxBatched);
    runScenario(true, true, meanFrames, maxFrames);
    runScenario(true, false, meanNoMask, maxNoMask);
    fprintf(stderr,
            "dyntopo roughness (radians): batched+mask mean=%g max=%g | "
            "frames+mask mean=%g max=%g | frames+NOmask mean=%g max=%g\n",
            meanBatched,
            maxBatched,
            meanFrames,
            maxFrames,
            meanNoMask,
            maxNoMask);
    // The view mask is DYNAMIC (live normals), so cadence legitimately matters:
    // per-frame normal refreshes feed back into the factor. Bound the feedback
    // instead — a masked stroke must not roughen far beyond an unmasked one.
    test_assert(meanFrames < meanNoMask * 2.0f + 0.02f);
  }
}

// Post-GPU-stroke normal currency: after a GPU-resident kelvinlet stroke ends
// (final co readback -> CPU normal recompute), BOTH vert normals and the face
// normals the renderer flat-shades from must match a fresh whole-mesh
// reference — interior verts of touched leaves included, not just the
// halo-refreshed node-boundary ring. Skipped when no GPU device exists.
static void gpuStrokeNormalsProbe()
{
  Scene s(128, 128, /*headless=*/true);
  if (!s.ensureGPU()) {
    fprintf(stderr, "gpu-stroke normals: no GPU device; skipped\n");
    return;
  }
  auto r = script::run(s,
                       "make_cube subdivs=100 size=0.5 sphere=1\n"
                       "build_spatial leaf_limit=64 depth_limit=16\n"
                       "set_brush radius=0.15 strength=0.5\n"
                       "set_brush_tool tool=kelvinlet\n"
                       "set_grab from=0.2165,0,0.125 to=0.30,0,0.17\n"
                       "set_backend backend=wgsl\n"
                       "stroke origin=0.2165,0,0.125 normal=0.866,0,0.5\n",
                       ".");
  test_assert(r.ok);
  Mesh *m = s.mesh;

  float disp = 0.0f;
  for (int v : m->v) {
    disp = std::fmax(disp, std::fabs((m->v.co[v]).length() - 0.25f));
  }
  fprintf(stderr, "gpu-stroke max radial disp = %g\n", disp);
  test_assert(disp > 1e-3f); // the GPU stroke actually deformed

  // Vert normals vs whole-mesh reference, mismatches classified by ownership.
  Vector<float3> ref;
  referenceNormals(m, ref);
  auto *vnode = static_cast<mesh::AttrData<int> *>(
      m->v.attrs.find_attribute(mesh::AttrType::INT, ".spatial.v.node").data);
  int badInterior = 0, badBoundary = 0;
  float worst = 0.0f;
  for (int v : m->v) {
    float d = (m->v.no[v] - ref[v]).length();
    worst = std::fmax(worst, d);
    if (d > 2e-3f) {
      // Boundary vert: shares an edge with a vert owned by another leaf.
      bool boundary = false;
      int e0 = m->v.e[v];
      if (e0 != ELEM_NONE) {
        for (int e : mesh::EdgeOfVertIter(m, v, e0)) {
          int o = m->e.vs[e][0] == v ? m->e.vs[e][1] : m->e.vs[e][0];
          if (vnode->safe_get(o) != vnode->safe_get(v)) {
            boundary = true;
            break;
          }
        }
      }
      if (boundary) {
        badBoundary++;
      } else {
        badInterior++;
      }
    }
  }
  fprintf(stderr,
          "gpu-stroke vert normals: worst=%g stale interior=%d boundary=%d\n",
          worst,
          badInterior,
          badBoundary);

  // Face normals (the renderer's flat-shading source).
  int badFace = 0;
  float worstFace = 0.0f;
  for (int f : m->f) {
    int l = m->f.l[f];
    int c0 = m->l.c[l];
    int c1 = m->c.next[c0];
    int c2 = m->c.next[c1];
    float3 n = triNormal(m->v.co[m->c.v[c0]], m->v.co[m->c.v[c1]], m->v.co[m->c.v[c2]]);
    if (m->l.size[l] > 3) {
      int c3 = m->c.next[c2];
      n += triNormal(m->v.co[m->c.v[c0]], m->v.co[m->c.v[c2]], m->v.co[m->c.v[c3]]);
    }
    n.normalize();
    float d = (m->f.no[f] - n).length();
    worstFace = std::fmax(worstFace, d);
    if (d > 2e-3f) {
      badFace++;
    }
  }
  fprintf(stderr, "gpu-stroke face normals: worst=%g stale=%d\n", worstFace, badFace);

  test_assert(badInterior == 0);
  test_assert(badBoundary == 0);
  test_assert(badFace == 0);
}

// The GPU stroke-end sync's exact flag shape: leaves flagged UpdateNormals
// with EMPTY affected_verts (= full-rebuild request) next to a leaf flagged
// with hints whose halo scan reaches into them. The halo must not append into
// the empty set — that downgrades the full rebuild to an incremental pass over
// just the boundary ring, leaving the leaf's interior stale (the interactive
// "normals not updated after a GPU stroke except at node boundaries" bug).
static void fullRebuildRequestProbe()
{
  Scene s(128, 128, /*headless=*/true);
  auto r = script::run(s,
                       "make_cube subdivs=100 size=0.5 sphere=1\n"
                       "build_spatial leaf_limit=64 depth_limit=16\n",
                       ".");
  test_assert(r.ok);
  Mesh *m = s.mesh;
  m->recalc_normals();
  s.tree->update(&s.gpu);

  // Deform EVERY vert (radial inflate) so every leaf's normals are stale, then
  // flag every leaf the way GpuBrush_endStroke does: UpdateNormals, empty
  // affected — except one leaf, which gets an incremental hint so its halo
  // scan reaches into its full-request neighbors.
  for (int v : m->v) {
    float3 co = m->v.co[v];
    float l = co.length();
    if (l > 1e-9f) {
      m->v.co[v] = co * (1.0f + 0.15f * co[2] / l);
    }
  }
  auto leaves = s.tree->leaves();
  test_assert(leaves.size() > 4);
  for (spatial::SpatialNode *leaf : leaves) {
    leaf->affected_verts.clear();
    leaf->flag |= spatial::Spatial_UpdateNormals | spatial::Spatial_UpdateGPU |
                  spatial::Spatial_RegenBounds;
  }
  // The hinted leaf models a CPU dab: its own moved verts as hints (a correct
  // incremental request). Its halo scan is what reaches into the neighbors.
  spatial::SpatialNode *hinted = leaves[0];
  for (int v : hinted->data->unique_verts) {
    hinted->affected_verts.append(v);
  }

  s.tree->update(&s.gpu);

  Vector<float3> ref;
  referenceNormals(m, ref);
  test_assert(compareNormals(m, ref, "full-rebuild request") == 0);
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  // A/B seam: run only the dyntopo roughness probe (used to baseline against
  // stashed/older engine code whose normals would fail the exactness parts).
  if (getenv("SC_ROUGH_ONLY")) {
    roughnessProbe();
    return test_end();
  }
  fullRebuildRequestProbe();
  gpuStrokeNormalsProbe();

  // Scoped so the scene destructs before test_end()'s leak accounting.
  {
    Scene s(128, 128, /*headless=*/true);
    // Small leaf_limit so the grid spans many leaves and strokes cross ownership
    // boundaries; radius 0.5 keeps each dab region a strict subset of the leaves
    // so the halo path (not just the skirt) is load-bearing.
    auto r = script::run(s,
                         "make_shape kind=grid n=24 m=24 size=2\n"
                         "build_spatial leaf_limit=64 depth_limit=16\n"
                         "set_brush radius=0.5 strength=0.7\n"
                         "set_brush_tool tool=draw\n"
                         "stroke origin=-0.5,-0.5,0 normal=0,0,1\n",
                         ".");
    test_assert(r.ok);
    Mesh *m = s.mesh;

    // The stroke must actually deform the surface — a flat grid cannot expose
    // partial fans (any subset of identical normals still normalizes the same).
    float maxZ = 0.0f;
    for (int v : m->v) {
      maxZ = std::fmax(maxZ, std::fabs(m->v.co[v][2]));
    }
    fprintf(stderr, "post-stroke max |z| = %g\n", maxZ);
    test_assert(maxZ > 1e-3f);

    Vector<float3> ref;
    referenceNormals(m, ref);
    test_assert(compareNormals(m, ref, "stroke 1") == 0);

    // Second stroke elsewhere: skirts now exist, so this exercises the
    // incremental + halo path on a mesh with prior sculpting.
    r = script::run(s, "stroke origin=0.3,0.3,0 normal=0,0,1\n", ".");
    test_assert(r.ok);

    referenceNormals(m, ref);
    test_assert(compareNormals(m, ref, "stroke 2") == 0);

    // GPU slice coherence: a boundary vert is replicated into every leaf slice
    // whose tris reference it. Any replica the frame flush failed to refill
    // renders as a fixed seam along the node boundary, so every slice corner
    // must equal the live position and the live face normal (fill_leaf_slice
    // flat-shades from f.no, not v.no).
    int staleCo = 0, staleNo = 0;
    for (spatial::SpatialNode *gn : s.tree->gpu_nodes()) {
      if (!gn->gpu_data || !gn->gpu_data->pos || !gn->gpu_data->nor) {
        continue;
      }
      float3 *pos = gn->gpu_data->pos->get_data<float3>();
      float3 *nor = gn->gpu_data->nor->get_data<float3>();
      for (auto &slice : gn->gpu_data->slices) {
        spatial::SpatialNode *leaf = slice.leaf;
        if (!leaf || !leaf->data) {
          continue;
        }
        int idx = slice.vert_start;
        for (auto &tri : leaf->data->tris) {
          for (int k = 0; k < 3; k++, idx++) {
            int v = m->c.v[tri.c[k]];
            if ((pos[idx] - m->v.co[v]).length() > 1e-5f) {
              staleCo++;
            }
            if ((nor[idx] - m->f.no[tri.f]).length() > 2e-3f) {
              staleNo++;
            }
          }
        }
      }
    }
    fprintf(stderr,
            "gpu slices: %d stale positions, %d stale face-normals\n",
            staleCo,
            staleNo);
    test_assert(staleCo == 0);
    test_assert(staleNo == 0);

    // Undo -> restroke (the interactive tearing repro): normals must be
    // reference-exact after the undo's replay flags are consumed, and again
    // after sculpting on top of the restored state.
    s.meshLog.undo(m, s.tree);
    s.tree->update(&s.gpu);
    referenceNormals(m, ref);
    test_assert(compareNormals(m, ref, "post-undo") == 0);

    r = script::run(s, "stroke origin=0.1,0.1,0 normal=0,0,1\n", ".");
    test_assert(r.ok);
    referenceNormals(m, ref);
    test_assert(compareNormals(m, ref, "stroke after undo") == 0);
  }

  // --- Part 2: masked undo->restroke displacement smoothness ---------------
  // The interactive tearing repro: a view-normal-masked DRAW stroke near the
  // silhouette of a curved surface, undone and re-stroked. The masked
  // displacement field must stay smooth across leaf-ownership boundaries —
  // node-aligned strength discontinuities show up here as edge-jump outliers
  // on boundary edges (edges whose two verts have different owner leaves).
  {
    Scene s(128, 128, /*headless=*/true);
    auto r = script::run(s,
                         "make_cube subdivs=100 size=0.5 sphere=1\n"
                         "build_spatial leaf_limit=64 depth_limit=16\n"
                         "set_brush radius=0.15 strength=0.5\n"
                         "set_brush_tool tool=draw\n",
                         ".");
    test_assert(r.ok);
    Mesh *m = s.mesh;
    m->recalc_normals();

    // Eye looks down -Z; the +X side of the sphere is the silhouette band,
    // where the view-normal ramp (limit 90, falloff 25 degrees) is live.
    s.brush.automask_view_normal = true;
    s.brush.cull_backfaces = false;
    s.brush.viewDir = float3{0, 0, -1};
    // Wide ramp (the interactive repro sets View Falloff to 75 degrees), so the
    // factor varies across most of the visible hemisphere.
    s.brush.view_normal_falloff = 75.0f * kDeg;
    s.brush.writeProps();

    Vector<float3> co0;
    co0.resize(m->v.capacity());
    for (int v : m->v) {
      co0[v] = m->v.co[v];
    }

    // ~60 degrees off head-on: mid-ramp, so factors vary smoothly across the
    // dab region instead of clamping to 0 at the exact silhouette. The
    // spherified cube's radius is ~size/2 = 0.25.
    const char *strokeCmd = "stroke origin=0.2165,0,0.125 normal=0.866,0,0.5\n";
    r = script::run(s, strokeCmd, ".");
    test_assert(r.ok);
    float disp1 = 0.0f;
    for (int v : m->v) {
      disp1 = std::fmax(disp1, (m->v.co[v] - co0[v]).length());
    }
    fprintf(stderr, "masked stroke 1 max disp = %g\n", disp1);
    s.meshLog.undo(m, s.tree);
    s.tree->update(&s.gpu);
    r = script::run(s, strokeCmd, ".");
    test_assert(r.ok);

    if (m->topo_frozen) {
      m->thawTopo();
    }
    float maxInterior = 0.0f, maxBoundary = 0.0f;
    int boundaryEdges = 0;
    for (int e : m->e) {
      int a = m->e.vs[e][0];
      int b = m->e.vs[e][1];
      float jump =
          std::fabs((m->v.co[a] - co0[a]).length() - (m->v.co[b] - co0[b]).length());
      if (s.tree->treeMesh.v.node[a] != s.tree->treeMesh.v.node[b]) {
        maxBoundary = std::fmax(maxBoundary, jump);
        boundaryEdges++;
      } else {
        maxInterior = std::fmax(maxInterior, jump);
      }
    }
    fprintf(stderr,
            "masked undo-restroke: %d boundary edges, max jump interior=%g boundary=%g\n",
            boundaryEdges,
            maxInterior,
            maxBoundary);
    test_assert(boundaryEdges > 0);
    test_assert(maxInterior > 0.0f); // the stroke actually deformed
    // A node-aligned strength discontinuity makes boundary-edge jumps stand
    // far above the interior gradient; allow modest slack for mesh anisotropy.
    test_assert(maxBoundary < maxInterior * 2.0f);
  }

  // --- Part 3: determinism oracle (race detector) --------------------------
  // Identical masked dyntopo stroke sequences in two fresh scenes must produce
  // bit-identical geometry and normals: per-vert accumulation order is fixed
  // within each leaf's serial loops, so absent data races the parallel_for
  // schedule cannot change any result. Any divergence = a real race.
  {
    auto runScenario = [](litestl::util::Vector<float3> &coOut,
                          litestl::util::Vector<float3> &noOut) {
      Scene s(128, 128, /*headless=*/true);
      auto r = script::run(s,
                           "make_cube subdivs=100 size=0.5 sphere=1\n"
                           "build_spatial leaf_limit=64 depth_limit=16\n"
                           "set_brush radius=0.15 strength=0.5\n"
                           "set_brush_tool tool=draw\n"
                           "dyntopo enabled=1 detail=0.012 flip=0 smooth=0\n",
                           ".");
      test_assert(r.ok);
      Mesh *m = s.mesh;
      m->recalc_normals();
      s.brush.automask_view_normal = true;
      s.brush.cull_backfaces = false;
      s.brush.viewDir = float3{0, 0, -1};
      s.brush.view_normal_falloff = 75.0f * kDeg;
      s.brush.writeProps();

      r = script::run(s,
                      "stroke_path p1=0.15,-0.1,0.17 p2=0.15,0.1,0.17 steps=8 "
                      "normal=0.707,0,0.707\n",
                      ".");
      test_assert(r.ok);
      // Undo -> restroke, mirroring the interactive repro.
      s.meshLog.undo(m, s.tree);
      s.tree->update(&s.gpu);
      r = script::run(s,
                      "stroke_path p1=0.15,-0.1,0.17 p2=0.15,0.1,0.17 steps=8 "
                      "normal=0.707,0,0.707\n",
                      ".");
      test_assert(r.ok);

      coOut.clear();
      noOut.clear();
      for (int v : m->v) {
        coOut.append(m->v.co[v]);
        noOut.append(m->v.no[v]);
      }
    };

    Vector<float3> coA, noA, coB, noB;
    runScenario(coA, noA);
    runScenario(coB, noB);

    test_assert(coA.size() == coB.size());
    int coDiff = 0, noDiff = 0;
    for (int i = 0; i < int(coA.size()); i++) {
      if (std::memcmp(&coA[i], &coB[i], sizeof(float3)) != 0) {
        coDiff++;
      }
      if (std::memcmp(&noA[i], &noB[i], sizeof(float3)) != 0) {
        noDiff++;
      }
    }
    fprintf(stderr,
            "dyntopo determinism: %d verts, co diffs=%d, no diffs=%d\n",
            int(coA.size()),
            coDiff,
            noDiff);
    test_assert(coDiff == 0);
    test_assert(noDiff == 0);
  }

  roughnessProbe();

  return test_end();
}
