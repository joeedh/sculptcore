#include "interactive.h"

#include "scene.h"

#include "brush/brush_executor.h"
#include "brush/stroke_spacing.h"
#include "litestl/math/matrix.h"
#include "litestl/util/vector.h"
#include "spatial/spatial.h"
#include "spatial/spatial_base.h"

#ifdef SBRUSH_GPU_DISPATCH
#include "gpu_stroke.h"

#include <string>
#endif

#include "GLFW/glfw3.h"

#include <cmath>

namespace sculptcore::debug_app {

using litestl::math::float2;
using litestl::math::float3;
using litestl::math::mat4;
using litestl::util::Vector;

namespace {

constexpr float kOrbitRadPerPx = 0.005f;
constexpr float kZoomFactor = 1.1f;
constexpr float kMinViewDistance = 0.05f;

float framebufferAspect(const Scene &s)
{
  int w = s.swapchain.width > 0 ? s.swapchain.width : s.width;
  int h = s.swapchain.height > 0 ? s.swapchain.height : s.height;
  if (h <= 0) {
    return 1.0f;
  }
  return float(w) / float(h);
}

void framebufferSize(const Scene &s, int &w, int &h)
{
  w = s.swapchain.width > 0 ? s.swapchain.width : s.width;
  h = s.swapchain.height > 0 ? s.swapchain.height : s.height;
}

float3 transformPoint(const mat4 &m, float3 p)
{
  const float *d = static_cast<const float *>(m);
  float x = d[0] * p[0] + d[4] * p[1] + d[8] * p[2] + d[12];
  float y = d[1] * p[0] + d[5] * p[1] + d[9] * p[2] + d[13];
  float z = d[2] * p[0] + d[6] * p[1] + d[10] * p[2] + d[14];
  float w = d[3] * p[0] + d[7] * p[1] + d[11] * p[2] + d[15];
  if (std::fabs(w) > 1e-7f) {
    return float3(x / w, y / w, z / w);
  }
  return float3(x, y, z);
}

} // namespace

InteractiveController::InteractiveController(Scene *scene) : scene_(scene) {}

InteractiveController::~InteractiveController()
{
  endStroke();
}

bool InteractiveController::screenRay(float2 cursor, float3 &origin, float3 &dir) const
{
  int w = 0, h = 0;
  framebufferSize(*scene_, w, h);
  if (w <= 0 || h <= 0) {
    return false;
  }
  float aspect = float(w) / float(h);
  mat4 vp = scene_->camera.viewProj(aspect);
  mat4 inv = vp.inverse();
  float ndcX = (2.0f * cursor[0] / float(w)) - 1.0f;
  float ndcY = 1.0f - (2.0f * cursor[1] / float(h));
  float3 nearP = transformPoint(inv, float3(ndcX, ndcY, -1.0f));
  float3 farP = transformPoint(inv, float3(ndcX, ndcY, 1.0f));
  origin = scene_->camera.eye;
  float3 d = farP - nearP;
  if (d.lengthSqr() < 1e-12f) {
    return false;
  }
  d.normalize();
  dir = d;
  return true;
}

bool InteractiveController::pickSurface(float2 cursor, float3 &hit, float3 &normal) const
{
  if (!scene_->tree) {
    return false;
  }
  float3 origin, dir;
  if (!screenRay(cursor, origin, dir)) {
    return false;
  }
  spatial::CastRayIsect isect;
  if (!scene_->tree->castRay(origin, dir, isect)) {
    return false;
  }
  hit = isect.p;
  normal = isect.normal;
  return true;
}

void InteractiveController::beginStroke(float2 cursor)
{
  if (!scene_->tree) {
    return;
  }
  float3 hit, normal;
  if (!pickSurface(cursor, hit, normal)) {
    return;
  }

  strokeLastPos_ = hit;
  strokeHasLast_ = true;
  strokeResidual_ = 0.0f;

#ifdef SBRUSH_GPU_DISPATCH
  // WGSL backend: drive the stroke through the persistent GPU compute session
  // (upload once, dab per move, read back on release). begin() fails cleanly
  // for a tool with no GPU kernel, in which case we fall back to the C++ path.
  if (scene_->currentBackend == BrushBackend::Wgsl ||
      scene_->currentBackend == BrushBackend::WgpuNative) {
    std::string err;
    gpuSession_ = new GpuStrokeSession();
    if (scene_->currentBackend == BrushBackend::WgpuNative) {
      // WebGPU compute can't share buffers with the Vulkan renderer, so there is
      // no live scatter: each dab reads its moved verts back to the CPU mesh and
      // marks the touched nodes dirty so the Vulkan path redraws them.
      gpuSession_->enableInteractiveReadback();
    } else {
      // Wgsl: GPU-resident live-render path — dabs scatter into the render VBOs +
      // read back only touched verts, so the mesh deforms during the drag (the
      // batch/verify path never sets this and stays full-readback-at-end).
      gpuSession_->enableLiveRender(scene_->backendWindow ? scene_->backendWindow
                                                          : scene_->backend);
    }
    if (gpuSession_->begin(*scene_, err)) {
      gpuSession_->dab(*scene_, hit, normal, err);
      scene_->lastStroke.valid = true;
      scene_->lastStroke.origin = hit;
      scene_->lastStroke.normal = normal;
      scene_->lastStroke.radius = scene_->brush.radius;
      return;
    }
    delete gpuSession_;
    gpuSession_ = nullptr;
  }
#endif

  // C++ backend: profiled here (the WGSL path self-profiles inside the session).
  scene_->profiler.beginStroke();
  auto ptBegin = StrokeProfiler::now();

  exec_ = new brush::CommandExecutor(scene_->tree, &scene_->brush);
  exec_->meshLog = &scene_->meshLog;
  exec_->beginStep();

  Vector<spatial::SpatialNode *> nodes;
  scene_->tree->filterNodes(hit, scene_->brush.radius, nodes);
  if (nodes.size() != 0) {
    auto ptDab = StrokeProfiler::now();
    exec_->execBrush(scene_->currentTool, &nodes, hit, normal);
    exec_->clearIsFirstOfStep();
    scene_->profiler.addDab(StrokeProfiler::ms(ptDab, StrokeProfiler::now()), 0,
                            0);
  }
  scene_->profiler.addBegin(StrokeProfiler::ms(ptBegin, StrokeProfiler::now()));
  scene_->lastStroke.valid = true;
  scene_->lastStroke.origin = hit;
  scene_->lastStroke.normal = normal;
  scene_->lastStroke.radius = scene_->brush.radius;
}

void InteractiveController::continueStroke(float2 cursor)
{
  if ((!exec_ && !gpuSession_) || !strokeHasLast_) {
    return;
  }
  float3 hit, normal;
  if (!pickSurface(cursor, hit, normal)) {
    return;
  }

  brush::StrokeSpacer spacer;
  spacer.spacing = scene_->brush.radius * scene_->brush.spacing;
  spacer.has_last = true;
  spacer.last_pos = strokeLastPos_;
  spacer.residual = strokeResidual_;

  /* Skip the first emit-of-segment (which would be `strokeLastPos_` itself —
   * already deposited). StrokeSpacer's first advance is a no-op since
   * has_last is preset; subsequent advances will emit interior dabs. */
  auto emit = [&](float3 p) {
#ifdef SBRUSH_GPU_DISPATCH
    if (gpuSession_) {
      std::string err;
      gpuSession_->dab(*scene_, p, normal, err);
      scene_->lastStroke.origin = p;
      scene_->lastStroke.normal = normal;
      return;
    }
#endif
    if (!exec_) return;
    Vector<spatial::SpatialNode *> nodes;
    scene_->tree->filterNodes(p, scene_->brush.radius, nodes);
    if (nodes.size() == 0) {
      return;
    }
    auto ptDab = StrokeProfiler::now();
    exec_->execBrush(scene_->currentTool, &nodes, p, normal);
    exec_->clearIsFirstOfStep();
    scene_->profiler.addDab(StrokeProfiler::ms(ptDab, StrokeProfiler::now()), 0,
                            0);
    scene_->lastStroke.origin = p;
    scene_->lastStroke.normal = normal;
  };

  spacer.advance(hit, emit);
  strokeLastPos_ = spacer.last_pos;
  strokeResidual_ = spacer.residual;
}

void InteractiveController::endStroke()
{
#ifdef SBRUSH_GPU_DISPATCH
  if (gpuSession_) {
    gpuSession_->end(*scene_);
    delete gpuSession_;
    gpuSession_ = nullptr;
    strokeHasLast_ = false;
    strokeResidual_ = 0.0f;
    return;
  }
#endif
  if (!exec_) {
    return;
  }
  auto ptEnd = StrokeProfiler::now();
  exec_->endStep();
  delete exec_;
  exec_ = nullptr;
  strokeHasLast_ = false;
  strokeResidual_ = 0.0f;
  scene_->profiler.addEnd(StrokeProfiler::ms(ptEnd, StrokeProfiler::now()));
  scene_->profiler.endStroke();
}

void InteractiveController::flushGpuReadback()
{
#ifdef SBRUSH_GPU_DISPATCH
  if (gpuSession_) {
    gpuSession_->flushInteractiveReadback(*scene_);
  }
#endif
}

void InteractiveController::doOrbit(float2 delta)
{
  float3 fwd = scene_->camera.target - scene_->camera.eye;
  float dist = fwd.length();
  if (dist < 1e-6f) {
    return;
  }
  fwd.normalize();
  float3 worldUp(0, 0, 1);
  float3 right = fwd.cross(worldUp);
  if (right.lengthSqr() < 1e-8f) {
    right = float3(1, 0, 0);
  }
  right.normalize();

  float yaw = -delta[0] * kOrbitRadPerPx;
  float pitch = -delta[1] * kOrbitRadPerPx;

  /* Rotate eye around target: yaw around world-up, then pitch around the
   * camera-right axis. Use simple Rodrigues-style rotations. */
  auto rotateAxis = [](float3 v, float3 axis, float ang) -> float3 {
    float c = std::cos(ang), s = std::sin(ang);
    float3 a = axis;
    return v * c + a.cross(v) * s + a * (a.dot(v) * (1.0f - c));
  };

  float3 rel = scene_->camera.eye - scene_->camera.target;
  rel = rotateAxis(rel, worldUp, yaw);
  /* Recompute right after yaw so pitch stays orthogonal. */
  float3 fwd2 = rel * -1.0f;
  fwd2.normalize();
  float3 right2 = fwd2.cross(worldUp);
  if (right2.lengthSqr() < 1e-8f) {
    right2 = right;
  } else {
    right2.normalize();
  }
  rel = rotateAxis(rel, right2, pitch);
  scene_->camera.eye = scene_->camera.target + rel;
}

void InteractiveController::doPan(float2 delta)
{
  float3 fwd = scene_->camera.target - scene_->camera.eye;
  float dist = fwd.length();
  if (dist < 1e-6f) {
    return;
  }
  fwd.normalize();
  float3 up = scene_->camera.up;
  float3 right = fwd.cross(up);
  right.normalize();
  float3 trueUp = right.cross(fwd);
  trueUp.normalize();

  int w = 0, h = 0;
  framebufferSize(*scene_, w, h);
  if (h <= 0) {
    return;
  }
  /* Convert pixel delta to world-space at the target plane. */
  float worldPerPx = (2.0f * dist * std::tan(scene_->camera.fovy * 0.5f)) / float(h);
  float3 ofs = right * (-delta[0] * worldPerPx) + trueUp * (delta[1] * worldPerPx);
  scene_->camera.eye = scene_->camera.eye + ofs;
  scene_->camera.target = scene_->camera.target + ofs;
}

void InteractiveController::doZoom(float deltaY)
{
  float3 rel = scene_->camera.eye - scene_->camera.target;
  float dist = rel.length();
  if (dist < 1e-6f) {
    return;
  }
  float factor = std::pow(kZoomFactor, -deltaY);
  float newDist = dist * factor;
  if (newDist < kMinViewDistance) {
    newDist = kMinViewDistance;
  }
  scene_->camera.eye = scene_->camera.target + rel * (newDist / dist);
}

bool InteractiveController::handle(const InputEvent &e)
{
  switch (e.kind) {
  case InputKind::CursorPos: {
    float2 prev = cursor_;
    cursor_ = float2(e.u.cursor.x, e.u.cursor.y);
    float2 delta(cursor_[0] - prev[0], cursor_[1] - prev[1]);
    if (lmbDrag_ == DragMode::Stroke) {
      continueStroke(cursor_);
    } else if (lmbDrag_ == DragMode::Orbit || rightOrMiddleOrbit_) {
      doOrbit(delta);
    } else if (lmbDrag_ == DragMode::Pan) {
      doPan(delta);
    }
    dragPrev_ = cursor_;
    return false;
  }
  case InputKind::MouseButton: {
    bool press = (e.u.mb.action == ButtonAction::Press);
    mods_ = e.u.mb.mods.bits;
    cursor_ = float2(e.u.mb.x, e.u.mb.y);
    if (e.u.mb.button == MouseButton::Left) {
      lmbDown_ = press;
      if (press) {
        dragStart_ = cursor_;
        dragPrev_ = cursor_;
        bool shift = (mods_ & 0x1) != 0;
        bool ctrl = (mods_ & 0x2) != 0;
        bool alt = (mods_ & 0x4) != 0;
        (void)ctrl;
        if (shift) {
          lmbDrag_ = DragMode::Pan;
        } else if (alt) {
          lmbDrag_ = DragMode::Orbit;
        } else {
          lmbDrag_ = DragMode::Stroke;
          beginStroke(cursor_);
        }
      } else {
        if (lmbDrag_ == DragMode::Stroke) {
          endStroke();
        }
        lmbDrag_ = DragMode::None;
      }
    } else if (e.u.mb.button == MouseButton::Right) {
      rmbDown_ = press;
      rightOrMiddleOrbit_ = rmbDown_ || mmbDown_;
    } else if (e.u.mb.button == MouseButton::Middle) {
      mmbDown_ = press;
      rightOrMiddleOrbit_ = rmbDown_ || mmbDown_;
    }
    return false;
  }
  case InputKind::Scroll: {
    doZoom(e.u.scroll.dy);
    return true;
  }
  case InputKind::Key: {
    if (e.u.key.action == ButtonAction::Release) {
      return false;
    }
    mods_ = e.u.key.mods.bits;
    bool ctrl = (mods_ & 0x2) != 0;
    bool shift = (mods_ & 0x1) != 0;
    if (!ctrl) {
      return false;
    }
    if (exec_) {
      /* Refuse undo/redo mid-stroke. */
      return false;
    }
    if (!scene_->mesh || !scene_->tree) {
      return false;
    }
    if (e.u.key.key == GLFW_KEY_Z && !shift) {
      scene_->meshLog.undo(scene_->mesh, scene_->tree);
      return true;
    }
    if ((e.u.key.key == GLFW_KEY_Z && shift) || e.u.key.key == GLFW_KEY_Y) {
      scene_->meshLog.redo(scene_->mesh, scene_->tree);
      return true;
    }
    return false;
  }
  case InputKind::FramebufferSize: {
    scene_->handleResize();
    return false;
  }
  case InputKind::Char:
    return false;
  }
  return false;
}

} // namespace sculptcore::debug_app
