#pragma once

#include "input.h"

#include "brush/brushes/types.h"
#include "litestl/math/vector.h"

namespace sculptcore::brush {
struct CommandExecutor;
}

namespace sculptcore::debug_app {

struct Scene;
class GpuStrokeSession;

/** Drives interactive sculpting: routes mouse drags into brush strokes,
 *  orbits/pans/zooms the camera, and handles undo/redo keys. Reads
 *  framebuffer size from `Scene::swapchain` when projecting screen rays.
 *
 *  Modifier precedence for LMB: Shift+LMB strokes with the smooth brush (a
 *  temporary tool override, mirroring the TS app), Ctrl+LMB pans, Alt+LMB
 *  orbits, plain LMB strokes with the active tool. RMB or MMB drag also orbits.
 *  Scroll zooms. Ctrl+Z / Ctrl+Y route to the scene's MeshLog, refused while a
 *  stroke is open. */
struct InteractiveController : InputHandler {
  using float2 = litestl::math::float2;
  using float3 = litestl::math::float3;

  InteractiveController(Scene *scene);
  ~InteractiveController() override;

  bool handle(const InputEvent &e) override;

  /* Flush a WgpuNative stroke's per-frame accumulated dab readback (no-op for
   * other backends / when no stroke is open). The interactive frame loop calls
   * this once after polling input, so a burst of catch-up dabs costs a single
   * readback instead of one per dab. */
  void flushGpuReadback();

private:
  /* LMB drag intent, decided on press from current modifiers. */
  enum class DragMode { None, Stroke, Orbit, Pan };

  void beginStroke(float2 cursor);
  void continueStroke(float2 cursor);
  void endStroke();

  void doOrbit(float2 delta);
  void doPan(float2 delta);
  void doZoom(float deltaY);

  /** Screen→world ray through `cursor` in window pixels. */
  bool screenRay(float2 cursor, float3 &origin, float3 &dir) const;
  /** Cast ray through cursor; emits world hit + interpolated normal. */
  bool pickSurface(float2 cursor, float3 &hit, float3 &normal) const;

  Scene *scene_;

  /* Pointer state. */
  float2 cursor_{0, 0};
  float2 dragStart_{0, 0};
  float2 dragPrev_{0, 0};
  unsigned int mods_ = 0;
  bool lmbDown_ = false;
  bool rmbDown_ = false;
  bool mmbDown_ = false;

  /* Active drag (set on LMB-press; cleared on LMB-release). */
  DragMode lmbDrag_ = DragMode::None;
  /* Orbit/pan triggered by RMB or MMB (independent of LMB). */
  bool rightOrMiddleOrbit_ = false;

  /* Stroke state. Exactly one of exec_ (C++ backend) / gpuSession_ (WGSL
   * backend, GPU compute) is non-null while a stroke is open; the backend is
   * chosen on press from scene_->currentBackend. */
  brush::CommandExecutor *exec_ = nullptr;
  GpuStrokeSession *gpuSession_ = nullptr;
  float3 strokeLastPos_{0, 0, 0};
  bool strokeHasLast_ = false;
  float strokeResidual_ = 0.0f;
  uint32_t dyntopoSeed_ = 1; /* per-dab seed for the dyntopo pre-pass */
  /* Shift→smooth override: when toolOverridden_ is set, scene_->currentTool was
   * swapped to SMOOTH on press and savedTool_ holds the tool to restore on
   * release (covers both the C++ and GPU dab paths, which read currentTool). */
  bool toolOverridden_ = false;
  brush::SculptBrushes savedTool_ = brush::SculptBrushes::DRAW;
};

} // namespace sculptcore::debug_app
