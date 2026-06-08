#include "vk_overlay.h"

#include "vk_backend.h"

#include "gpu/batch.h"
#include "gpu/command.h"
#include "gpu/manager.h"
#include "gpu/vbo.h"
#include "spatial/shaders/spatial_shaders.h"

#include "litestl/util/string.h"

#include <cmath>

namespace sculptcore::vulkan {

using litestl::math::float2;
using litestl::math::float3;
using litestl::math::float4;
using litestl::math::mat4;

namespace {

/* Build a transient line-list batch from pre-filled vertex arrays.
 * Vertex layout matches basicLineShader: position(vec3), color(vec4),
 * uv(vec2). Caller is responsible for destroying the batch + buffers. */
sculptcore::gpu::DrawBatch *buildLineBatch(sculptcore::gpu::GPUManager &mgr,
                                           const float3 *positions,
                                           const float4 *colors,
                                           int vertCount)
{
  using namespace sculptcore::gpu;
  Buffer *posBuf = mgr.createBuffer(
      litestl::util::string("position"), GPUType::FLOAT32, 3, vertCount);
  Buffer *colorBuf = mgr.createBuffer(
      litestl::util::string("color"), GPUType::FLOAT32, 4, vertCount);
  Buffer *uvBuf = mgr.createBuffer(
      litestl::util::string("uv"), GPUType::FLOAT32, 2, vertCount);

  float3 *pos = posBuf->get_data<float3>();
  float4 *clr = colorBuf->get_data<float4>();
  float2 *uv = uvBuf->get_data<float2>();
  for (int i = 0; i < vertCount; i++) {
    pos[i] = positions[i];
    clr[i] = colors[i];
    uv[i] = float2((i & 1) ? 1.0f : 0.0f, 0.0f);
  }
  posBuf->dirty();
  colorBuf->dirty();
  uvBuf->dirty();

  DrawBatch *batch = mgr.createBatch();
  batch->buffers.append(posBuf);
  batch->buffers.append(colorBuf);
  batch->buffers.append(uvBuf);

  ShaderDef *shader = &spatial::spatialShaders.basicLineShader;
  DrawCommand *cmd = mgr.createCommand(
      batch, GPUCmdType::DRAW_LINES, shader, 0, vertCount, vertCount / 2);
  cmd->attrs.append(posBuf);
  cmd->attrs.append(colorBuf);
  cmd->attrs.append(uvBuf);
  return batch;
}

} // namespace

void Overlay::drawAxes(sculptcore::gpu::GPUManager &mgr,
                       VulkanBackend &backend,
                       const mat4 &drawMatrix,
                       float scale)
{
  float3 pos[6] = {
      float3(0, 0, 0), float3(scale, 0, 0),
      float3(0, 0, 0), float3(0, scale, 0),
      float3(0, 0, 0), float3(0, 0, scale),
  };
  float4 clr[6] = {
      float4(1, 0, 0, 1), float4(1, 0, 0, 1),
      float4(0, 1, 0, 1), float4(0, 1, 0, 1),
      float4(0, 0, 1, 1), float4(0, 0, 1, 1),
  };
  auto *batch = buildLineBatch(mgr, pos, clr, 6);
  DrawUniforms u;
  u.drawMatrix = drawMatrix;
  u.normalMatrix.identity();
  u.uColor = float4(1, 1, 1, 1);
  backend.draw(batch, u);
  mgr.destroyBatch(batch, true, true);
}

void Overlay::drawLines(sculptcore::gpu::GPUManager &mgr,
                        VulkanBackend &backend,
                        const mat4 &drawMatrix,
                        const float3 *positions,
                        const float4 *colors,
                        int vertCount)
{
  vertCount &= ~1; // line-list: drop a dangling odd vertex
  if (vertCount < 2) {
    return;
  }
  auto *batch = buildLineBatch(mgr, positions, colors, vertCount);
  DrawUniforms u;
  u.drawMatrix = drawMatrix;
  u.normalMatrix.identity();
  u.uColor = float4(1, 1, 1, 1);
  backend.draw(batch, u);
  mgr.destroyBatch(batch, true, true);
}

void Overlay::drawBrushCursor(sculptcore::gpu::GPUManager &mgr,
                              VulkanBackend &backend,
                              const mat4 &drawMatrix,
                              float3 center,
                              float3 normal,
                              float radius,
                              float4 color)
{
  /* Build an orthonormal basis with `normal` as the up axis. */
  float3 n = normal;
  if (n.lengthSqr() < 1e-8f) {
    n = float3(0, 0, 1);
  }
  n.normalize();
  float3 t = (std::fabs(n[2]) < 0.95f) ? float3(0, 0, 1) : float3(1, 0, 0);
  float3 right = n.cross(t);
  right.normalize();
  float3 fwd = n.cross(right);
  fwd.normalize();

  constexpr int kSegments = 48;
  float3 pos[kSegments * 2];
  float4 clr[kSegments * 2];
  for (int i = 0; i < kSegments; i++) {
    float a0 = float(i) / float(kSegments) * 6.2831853f;
    float a1 = float(i + 1) / float(kSegments) * 6.2831853f;
    float3 p0 = center + (right * std::cos(a0) + fwd * std::sin(a0)) * radius;
    float3 p1 = center + (right * std::cos(a1) + fwd * std::sin(a1)) * radius;
    pos[i * 2 + 0] = p0;
    pos[i * 2 + 1] = p1;
    clr[i * 2 + 0] = color;
    clr[i * 2 + 1] = color;
  }
  auto *batch = buildLineBatch(mgr, pos, clr, kSegments * 2);
  DrawUniforms u;
  u.drawMatrix = drawMatrix;
  u.normalMatrix.identity();
  u.uColor = color;
  backend.draw(batch, u);
  mgr.destroyBatch(batch, true, true);
}

} // namespace sculptcore::vulkan
