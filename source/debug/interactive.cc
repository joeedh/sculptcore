#include "interactive.h"

#include "scene.h"

#include "brush/brush_executor.h"
#include "brush/stroke_driver.h"
#include "litestl/util/alloc.h"
#include "spatial/spatial.h"

#ifdef SBRUSH_GPU_DISPATCH
#include "gpu_stroke.h"

#include <string>
#endif

#include "GLFW/glfw3.h"

#include <cmath>

namespace sculptcore::debug_app {

using litestl::math::float2;
using litestl::math::float3;

namespace {

constexpr float kOrbitRadPerPx = 0.005f;
constexpr float kZoomFactor = 1.1f;
constexpr float kMinViewDistance = 0.05f;

} // namespace

InteractiveController::InteractiveController(Scene *scene) : scene_(scene) {}

InteractiveController::~InteractiveController()
{
  endStroke();
}

void InteractiveController::beginStroke(float2 cursor)
{
  if (!scene_->tree) {
    return;
  }

  driver_ = litestl::alloc::New<brush::BrushStrokeDriver>("interactive stroke driver",
                                                          scene_->tree);
  // Path is the only stroke method the debug app's mouse handling exposes;
  // anchored / drag-dot have no modifier bound to them here.
  driver_->strokeMethod = brush::StrokeMethod::Path;

  // Sample the press before committing to a stroke: the driver discards inputs
  // that miss the surface, so a click over empty space must open neither an
  // undo step nor a GPU session. The dabs stay readable until the next poll().
  pushCursor(cursor);
  scene_->configureStrokeDriver(*driver_);
  int n = driver_->poll();
  if (n == 0) {
    litestl::alloc::Delete<brush::BrushStrokeDriver>(driver_);
    driver_ = nullptr;
    return;
  }

  scene_->lastStroke.valid = true;
  scene_->lastStroke.radius = scene_->brush.radius;

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
      applyPolledDabs(n);
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
  // Keep topology thawed for the whole stroke when dyntopo is on, so the per-dab
  // remesh doesn't fight the brush's per-dab freeze (an O(mesh) thaw each dab).
  exec_->keepTopoThawed = scene_->dyntopoEnabled;
  exec_->beginStep(scene_->dyntopoEnabled);

  // One unified dab per sample through the executor — same dyntopo+deform+meshlog
  // sequence as the TS app and scripted harness, so interactive no longer
  // diverges; dyntopo is logged like everywhere else (the executor drives the
  // combined callbacks).
  applyPolledDabs(n);
  scene_->profiler.addBegin(StrokeProfiler::ms(ptBegin, StrokeProfiler::now()));
}

void InteractiveController::pushCursor(float2 cursor)
{
  driver_->push(cursor[0], cursor[1],
                /*pressure=*/1.0f, /*tiltX=*/0.0f, /*tiltY=*/0.0f, /*twist=*/0.0f,
                scene_->brush.invert, /*useAltBrush=*/false, scene_->brush.radius,
                scene_->brush.strength, scene_->brush.spacing);
}

void InteractiveController::applyPolledDabs(int n)
{
  for (int i = 0; i < n; i++) {
    const brush::DabSample *ps = driver_->sampleAt(i);
    if (!ps) {
      continue;
    }
    // No object matrix, so the driver's object-local sample space is world space.
    applyDab(float3(ps->p[0], ps->p[1], ps->p[2]), ps->vec, ps->radius);
  }
}

void InteractiveController::pumpStroke(float2 cursor, bool finish)
{
  if (finish) {
    driver_->end();
  } else {
    pushCursor(cursor);
  }
  // Re-snapshot the camera every batch — orbiting mid-stroke moves the rays the
  // driver casts and the plane it projects misses onto.
  scene_->configureStrokeDriver(*driver_);
  applyPolledDabs(driver_->poll());
}

void InteractiveController::applyDab(float3 center, float3 normal, float radius)
{
#ifdef SBRUSH_GPU_DISPATCH
  if (gpuSession_) {
    std::string err;
    gpuSession_->dab(*scene_, center, normal, err);
    scene_->lastStroke.origin = center;
    scene_->lastStroke.normal = normal;
    return;
  }
#endif
  if (!exec_) {
    return;
  }
  dyntopo::DynTopoParams *dtp =
      scene_->dyntopoEnabled ? &scene_->dyntopoParams : nullptr;
  auto ptDab = StrokeProfiler::now();
  exec_->applyDab(scene_->currentTool, center, normal, radius, dtp, dyntopoSeed_++);
  scene_->profiler.addDab(StrokeProfiler::ms(ptDab, StrokeProfiler::now()), 0, 0);
  if (scene_->dyntopoEnabled) {
    scene_->tree->update(&scene_->gpu); // regen dirty leaves after remesh
  }
  scene_->lastStroke.origin = center;
  scene_->lastStroke.normal = normal;
  scene_->lastStroke.radius = radius;
}

void InteractiveController::continueStroke(float2 cursor)
{
  if (!driver_) {
    return;
  }
  pumpStroke(cursor, /*finish=*/false);
}

void InteractiveController::endStroke()
{
  // Drain the trailing spline segment first: those dabs belong to this stroke,
  // so they must land before the tool override is dropped and the step closed.
  if (driver_) {
    pumpStroke(cursor_, /*finish=*/true);
    litestl::alloc::Delete<brush::BrushStrokeDriver>(driver_);
    driver_ = nullptr;
  }

  // Undo the shift→smooth tool override latched on press, before any early
  // return so the active tool is restored on every backend path.
  if (toolOverridden_) {
    scene_->currentTool = savedTool_;
    toolOverridden_ = false;
  }
#ifdef SBRUSH_GPU_DISPATCH
  if (gpuSession_) {
    gpuSession_->end(*scene_);
    delete gpuSession_;
    gpuSession_ = nullptr;
    return;
  }
#endif
  if (!exec_) {
    return;
  }
  auto ptEnd = StrokeProfiler::now();
  if (scene_->dyntopoEnabled) {
    exec_->endDynTopoStroke();
  }
  exec_->endStep();
  delete exec_;
  exec_ = nullptr;
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
  scene_->framebufferSize(w, h);
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
        if (ctrl) {
          lmbDrag_ = DragMode::Pan;
        } else if (alt) {
          lmbDrag_ = DragMode::Orbit;
        } else {
          lmbDrag_ = DragMode::Stroke;
          // Shift temporarily strokes with the smooth brush (mirrors the TS
          // app; deliberate per-tool site — host UI convention, not tool
          // knowledge, so the switch audit keeps it):
          // override scene_->currentTool for the drag (both C++ and GPU dab paths
          // read it) and restore it in endStroke.
          savedTool_ = scene_->currentTool;
          toolOverridden_ = shift;
          if (shift) {
            scene_->currentTool = brush::SculptBrushes::SMOOTH;
          }
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
    if (driver_) {
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
