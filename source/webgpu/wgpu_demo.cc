/* Deterministic WebGPU demo scene, shared by two entry points:
 *   - WASM:  webgpuRenderScene() renders to the page canvas (surface path).
 *   - Native: webgpuRenderSceneToPNG() renders offscreen and writes a PNG
 *             (wgpu-native), the cross-backend parity reference for the
 *             Playwright browser golden.
 *
 * The scene (a spherified cube framed from a corner) and camera are fixed so
 * the screenshot is stable. Camera math is inlined (mirrors debug_app::Camera)
 * to keep this module free of the debug app. */

#include <cmath>

#include "wgpu_backend.h"
#include "wgpu_context.h"

#include "gpu/manager.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "spatial/spatial.h"

#include "litestl/math/matrix.h"
#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#include "wgpu_screenshot.h"
#endif

namespace {

using litestl::math::float3;
using litestl::math::mat4;

mat4 perspective(float fovyRad, float aspect, float zn, float zf)
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

mat4 lookAt(float3 eye, float3 target, float3 up)
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

mat4 mul(const mat4 &a, const mat4 &b)
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

/* Fixed view-projection for the demo scene at w×h. */
sculptcore::webgpu::DrawUniforms sceneUniforms(int w, int h)
{
  float aspect = float(w) / float(h);
  float3 eye(1.8f, 1.8f, 1.8f);
  mat4 vp = mul(perspective(0.9f, aspect, 0.05f, 100.0f),
                lookAt(eye, float3(0, 0, 0), float3(0, 0, 1)));

  sculptcore::webgpu::DrawUniforms u;
  u.drawMatrix = vp;
  u.normalMatrix.identity();
  return u;
}

/* One-time scene + GPU state, owned for the process lifetime. */
struct DemoState {
  sculptcore::gpu::GPUManager gpu;
  sculptcore::webgpu::WgpuContext ctx;
  sculptcore::mesh::Mesh *mesh = nullptr;
  sculptcore::spatial::SpatialTree *tree = nullptr;
  sculptcore::webgpu::WebGpuBackend *backend = nullptr;
  bool ready = false;
};

DemoState *g_demo = nullptr;

/* Build the deterministic mesh + spatial tree (no GPU context needed). */
void buildScene(DemoState &d)
{
  d.mesh = sculptcore::mesh::createCube(16, 0.5f, 1.0f);
  d.tree = litestl::alloc::New<sculptcore::spatial::SpatialTree>(
      "SpatialTree (webgpu demo)", d.mesh);
  d.tree->leaf_limit = 512;
  d.tree->depth_limit = 10;
  d.tree->gpu_tri_target = 2048;
  d.tree->buildAll();
}

} // namespace

#ifdef __EMSCRIPTEN__

namespace {
bool ensureSceneSurface(int w, int h)
{
  if (g_demo && g_demo->ready) {
    return true;
  }
  if (!g_demo) {
    g_demo = new DemoState();
  }
  DemoState &d = *g_demo;

  if (!d.ctx.initFromEmscripten()) {
    return false;
  }
  if (!d.ctx.createCanvasSurface("#canvas")) {
    return false;
  }
  d.ctx.configureSurface(w, h, WGPUTextureFormat_BGRA8Unorm);

  buildScene(d);
  d.backend =
      new sculptcore::webgpu::WebGpuBackend(&d.gpu, &d.ctx, WGPUTextureFormat_BGRA8Unorm);
  d.ready = true;
  return true;
}
} // namespace

extern "C" {

/* Render one frame of the fixed scene to the page canvas. Returns 1 on
 * success, 0 if the device/surface wasn't ready. Exported to JS. */
int webgpuRenderScene(int w, int h)
{
  if (!ensureSceneSurface(w, h)) {
    return 0;
  }
  DemoState &d = *g_demo;

  sculptcore::webgpu::DrawUniforms u = sceneUniforms(w, h);
  d.tree->update(&d.gpu);

  if (!d.backend->beginFrameSurface(w, h, 0.10f, 0.11f, 0.13f, 1.0f)) {
    return 0;
  }
  d.backend->draw(d.tree->getDrawBatch(), u);
  d.backend->endFrameSurface();
  return 1;
}

} // extern "C"

#else // native

namespace sculptcore::webgpu {

/* Render the fixed scene offscreen at w×h (RGBA8Unorm) and write it to `path`.
 * Returns true on success. `outNonUniformPixels` (optional) receives the count
 * of non-background pixels so callers can assert geometry was drawn. Used by
 * the native parity test. */
bool webgpuRenderSceneToPNG(const char *path, int w, int h, int *outNonUniformPixels)
{
  DemoState d;
  if (!d.ctx.initNative()) {
    return false;
  }
  buildScene(d);

  bool ok = false;
  {
    WgpuTarget target;
    if (target.create(&d.ctx, w, h, WGPUTextureFormat_RGBA8Unorm)) {
      WebGpuBackend backend(&d.gpu, &d.ctx, WGPUTextureFormat_RGBA8Unorm);
      DrawUniforms u = sceneUniforms(w, h);
      d.tree->update(&d.gpu);

      if (backend.beginFrame(target, 0.10f, 0.11f, 0.13f, 1.0f)) {
        backend.draw(d.tree->getDrawBatch(), u);
        backend.endFrame();
        ok = captureTargetToPNG(path, d.ctx, target, outNonUniformPixels);
      }
    }
  } // backend + target released before the mesh/tree they reference

  litestl::alloc::Delete(d.tree);
  litestl::alloc::Delete(d.mesh);
  return ok;
}

} // namespace sculptcore::webgpu

#endif
