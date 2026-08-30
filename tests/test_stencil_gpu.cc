/* GPU stencil amplification (displacementAndSubSurf plan, S5 gate). Amplifies
 * an edit level (with nonzero stored displacement baked into its positions)
 * to the render level through WgpuStencilAmplify's chained per-level SpMV and
 * asserts the result is BIT-IDENTICAL to the CPU chain (Multires
 * materialization with zero finer-level disp). Then the screenshot A/B: the
 * CPU-materialized and GPU-amplified level-N meshes render through the same
 * offscreen WebGPU path and the PNGs must be byte-identical. Dispatch wall
 * time + stencil buffer sizes print as the recorded per-frame budget.
 * `test_stencil_gpu.cc_out bench` runs the target-density measurement. */
#include "test_util.h"

#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "spatial/spatial.h"
#include "subdiv/multires.h"
#include "subdiv/subdiv.h"
#include "webgpu/wgpu_backend.h"
#include "webgpu/wgpu_context.h"
#include "webgpu/wgpu_screenshot.h"
#include "webgpu/wgpu_stencil.h"

#include "litestl/math/matrix.h"
#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace litestl;
using namespace litestl::math;
using litestl::util::Vector;
using subdiv::Multires;
using subdiv::MultiresSlot;

/* --- fixed camera (mirrors wgpu_demo.cc's inline math) --- */

static mat4 perspectiveM(float fovyRad, float aspect, float zn, float zf)
{
  mat4 m;
  m.zero();
  float f = 1.0f / std::tan(fovyRad * 0.5f);
  float *d = static_cast<float *>(m);
  d[0] = f / aspect;
  d[5] = f;
  d[10] = (zf + zn) / (zn - zf);
  d[11] = -1.0f;
  d[14] = (2.0f * zf * zn) / (zn - zf);
  return m;
}

static mat4 lookAtM(float3 eye, float3 target, float3 up)
{
  float3 f = target - eye;
  f.normalize();
  float3 s = f.cross(up);
  s.normalize();
  float3 u = s.cross(f);
  mat4 m;
  m.identity();
  float *d = static_cast<float *>(m);
  d[0] = s[0];
  d[4] = s[1];
  d[8] = s[2];
  d[1] = u[0];
  d[5] = u[1];
  d[9] = u[2];
  d[2] = -f[0];
  d[6] = -f[1];
  d[10] = -f[2];
  d[12] = -s.dot(eye);
  d[13] = -u.dot(eye);
  d[14] = f.dot(eye);
  return m;
}

static mat4 mulM(const mat4 &a, const mat4 &b)
{
  mat4 r;
  const float *ad = static_cast<const float *>(a);
  const float *bd = static_cast<const float *>(b);
  float *rd = static_cast<float *>(r);
  for (int c = 0; c < 4; c++) {
    for (int row = 0; row < 4; row++) {
      float sum = 0.0f;
      for (int k = 0; k < 4; k++) {
        sum += ad[k * 4 + row] * bd[c * 4 + k];
      }
      rd[c * 4 + row] = sum;
    }
  }
  return r;
}

static webgpu::DrawUniforms sceneUniforms(int w, int h)
{
  float aspect = float(w) / float(h);
  mat4 vp = mulM(perspectiveM(0.9f, aspect, 0.05f, 100.0f),
                 lookAtM(float3(1.8f, 1.8f, 1.8f), float3(0, 0, 0), float3(0, 0, 1)));
  webgpu::DrawUniforms u;
  u.drawMatrix = vp;
  u.normalMatrix.identity();
  return u;
}

/* --- helpers --- */

static void snapshotCo(Mesh *m, Vector<float3> &out)
{
  out.resize(m->v.count);
  for (int i = 0; i < m->v.count; i++) {
    out[i] = m->v.co[i];
  }
}

static void injectDisp(Multires &mr, int maxDispLevel)
{
  for (int level = 1; level <= maxDispLevel; level++) {
    subdiv::SubdivLevel &lvl = mr.refiner.levels[level - 1];
    int S = lvl.gridSide, w = S + 1;
    for (int g = 0; g < mr.store.gridCount(); g++) {
      const int *gv = &lvl.gridVerts[g * w * w];
      for (int v = 0; v < w; v++) {
        for (int u = 0; u < w; u++) {
          int vid = gv[v * w + u];
          float *d = mr.store.elem(level, 0, g, u, v);
          d[0] = float((vid + level) % 5) * 0.015625f;
          d[1] = float((vid + 2 * level) % 3) * 0.03125f;
          d[2] = float(vid % 7) * 0.0078125f;
        }
      }
    }
  }
}

static bool renderMeshPNG(webgpu::WgpuContext &ctx, Mesh *m, const char *path, int *nonBg)
{
  const int w = 384, h = 384;
  gpu::GPUManager gpuMgr;
  spatial::SpatialTree tree(m);
  tree.leaf_limit = 512;
  tree.depth_limit = 10;
  tree.gpu_tri_target = 2048;
  tree.buildAll();

  bool ok = false;
  {
    webgpu::WgpuTarget target;
    if (target.create(&ctx, w, h, WGPUTextureFormat_RGBA8Unorm)) {
      webgpu::WebGpuBackend backend(&gpuMgr, &ctx, WGPUTextureFormat_RGBA8Unorm);
      webgpu::DrawUniforms u = sceneUniforms(w, h);
      tree.update(&gpuMgr);
      if (backend.beginFrame(target, 0.10f, 0.11f, 0.13f, 1.0f)) {
        backend.draw(tree.getDrawBatch(), u);
        backend.endFrame();
        ok = webgpu::captureTargetToPNG(path, ctx, target, nonBg);
      }
    }
  }
  return ok;
}

static bool filesEqual(const char *a, const char *b)
{
  std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
  if (!fa || !fb) {
    return false;
  }
  std::string sa((std::istreambuf_iterator<char>(fa)), std::istreambuf_iterator<char>());
  std::string sb((std::istreambuf_iterator<char>(fb)), std::istreambuf_iterator<char>());
  return sa.size() > 0 && sa == sb;
}

/* Max component-wise ULP distance between two position sets. */
static int maxUlp(const Vector<float3> &a, const Vector<float3> &b, int &diffCount)
{
  int worst = 0;
  diffCount = 0;
  for (int i = 0; i < int(a.size()); i++) {
    for (int c = 0; c < 3; c++) {
      float fa = a[i][c], fb = b[i][c];
      int32_t ia, ib;
      std::memcpy(&ia, &fa, 4);
      std::memcpy(&ib, &fb, 4);
      int d = std::abs(ia - ib);
      if (d != 0) {
        diffCount++;
        worst = d > worst ? d : worst;
      }
    }
  }
  return worst;
}

static void bench(webgpu::WgpuContext &ctx)
{
  for (int maxLevel = 6; maxLevel <= 7; maxLevel++) {
    Mesh *cage = createCube(8, 1.0f);
    Multires mr;
    mr.init(*cage, maxLevel);
    MultiresSlot *s2 = mr.setActiveLevel(2);

    Vector<float3> src;
    snapshotCo(s2->mesh, src);

    webgpu::WgpuStencilAmplify amp;
    if (!amp.init(&ctx, mr.refiner, 2, maxLevel)) {
      printf("bench L%d: init FAILED (storage budget?)\n", maxLevel);
      alloc::Delete(cage);
      continue;
    }
    /* Warm + measure: second dispatch is the steady-state per-frame cost. */
    amp.dispatch(reinterpret_cast<const float *>(src.data()), int(src.size()));
    double cold = amp.lastDispatchMs;
    amp.dispatch(reinterpret_cast<const float *>(src.data()), int(src.size()));
    printf("bench L2->L%d: fine=%d cold=%.2fms warm=%.2fms\n",
           maxLevel,
           amp.fineCount(),
           cold,
           amp.lastDispatchMs);
    fflush(stdout);

    /* Bit-exactness at density (also exercises the 2D-dispatch linearization,
     * which only kicks in past 65535 workgroups). */
    Vector<float3> gpuFine, cpuFine;
    if (amp.readback(gpuFine)) {
      MultiresSlot *sf = mr.materialize(maxLevel);
      snapshotCo(sf->mesh, cpuFine);
      int diffs = 0;
      int ulp = maxUlp(cpuFine, gpuFine, diffs);
      printf("bench L2->L%d: diffs=%d maxUlp=%d\n", maxLevel, diffs, ulp);
      fflush(stdout);
    }
    alloc::Delete(cage);
  }
}

int main(int argc, char **argv)
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  webgpu::WgpuContext ctx;
  if (!ctx.initNative()) {
    fprintf(stderr, "wgpu device unavailable\n");
    test_assert(false);
    return 1;
  }

  if (argc > 1 && std::strcmp(argv[1], "bench") == 0) {
    bench(ctx);
    return 0;
  }

  /* Edit level 2 carries nonzero disp (levels 1-2); levels 3-4 are pure
   * subdivision — exactly what the GPU chain amplifies. */
  Mesh *cage = createCube(2, 1.0f);
  Multires mr;
  mr.init(*cage, 4);
  injectDisp(mr, 2);

  MultiresSlot *s2 = mr.setActiveLevel(2);
  Vector<float3> src;
  snapshotCo(s2->mesh, src);

  MultiresSlot *s4 = mr.materialize(4);
  Vector<float3> cpu4;
  snapshotCo(s4->mesh, cpu4);

  webgpu::WgpuStencilAmplify amp;
  test_assert(amp.init(&ctx, mr.refiner, 2, 4));
  test_assert(amp.dispatch(reinterpret_cast<const float *>(src.data()), int(src.size())));
  Vector<float3> gpu4;
  test_assert(amp.readback(gpu4));
  test_assert(int(gpu4.size()) == int(cpu4.size()));

  int diffs = 0;
  int ulp = maxUlp(cpu4, gpu4, diffs);
  fprintf(stderr,
          "amplify L2->L4: verts=%d diffs=%d maxUlp=%d dispatch=%.2fms\n",
          int(cpu4.size()),
          diffs,
          ulp,
          amp.lastDispatchMs);
  test_assert(diffs == 0); /* bit-consistent with the CPU discrete-CC chain */

  /* Screenshot A/B: same topology, CPU vs GPU positions, same render path. */
  Mesh *meshA = mr.buildLevelTopo(4);
  Mesh *meshB = mr.buildLevelTopo(4);
  for (int i = 0; i < int(cpu4.size()); i++) {
    meshA->v.co[i] = cpu4[i];
    meshB->v.co[i] = gpu4[i];
  }
  meshA->recalc_normals();
  meshB->recalc_normals();

  int nonBgA = 0, nonBgB = 0;
  test_assert(renderMeshPNG(ctx, meshA, "stencil_cpu.png", &nonBgA));
  test_assert(renderMeshPNG(ctx, meshB, "stencil_gpu.png", &nonBgB));
  fprintf(stderr, "screenshot A/B: nonBg cpu=%d gpu=%d\n", nonBgA, nonBgB);
  test_assert(nonBgA > 384 * 384 / 100);
  test_assert(nonBgA == nonBgB);
  test_assert(filesEqual("stencil_cpu.png", "stencil_gpu.png"));

  alloc::Delete(meshA);
  alloc::Delete(meshB);
  alloc::Delete(cage);

  /* Skip test_end(): attr name strings stay live in the alloc tracker
   * (mirrors the other spatial/mesh tests). */
  return retval;
}
