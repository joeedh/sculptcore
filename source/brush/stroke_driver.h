#pragma once

/**
 * Engine-side brush stroke driver: turns a stream of raw pointer events into
 * evenly-spaced dabs along an interpolating (centripetal Catmull-Rom) spline.
 * It is a statement-for-statement port of the TypeScript
 * `scripts/editors/view3d/tools/stroke_driver.ts`; the two must stay in
 * agreement (there is an integration parity test) so the host can switch
 * between them behind a feature flag.
 *
 * The driver mirrors nothing and applies nothing to geometry — a host pushes
 * events, pulls ready samples on its tick, and owns mirroring / yielding / dab
 * application downstream. Raycasting is internal: the driver holds the
 * object's spatial tree and converts world rays into object space itself.
 *
 * All internals are double precision. The arc-length walk carry accumulates
 * over a whole stroke, and float drift there makes host/engine parity flaky.
 */

#include "litestl/binding/binding.h"
#include "litestl/math/math_bindings.h"
#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include "spatial/spatial_base.h"
#include "stroke_curve.h"

namespace sculptcore::spatial {
struct SpatialTree;
}

namespace sculptcore::brush {

/** Whether even spacing is measured in screen pixels or world units. */
enum class StrokeSpaceMode : int {
  Screen = 0,
  World = 1,
};

/**
 * PATH arc-length-walks the spline. ANCHORED fixes the dab origin on the
 * first input and re-emits there on every later input. DRAG_DOT follows the
 * cursor, one dab per input. Values mirror the TS enum exactly.
 */
enum class StrokeMethod : int {
  Path = 0,
  Anchored = 1,
  DragDot = 2,
};

/** ANCHORED only: which scalar the anchor->cursor drag drives live. */
enum class AnchoredLiveMode : int {
  Radius = 0,
  Angle = 1,
};

} // namespace sculptcore::brush

namespace litestl::binding {
template <> struct Binder<sculptcore::brush::StrokeSpaceMode> {
  static const BindingBase *bind();
};
template <> struct Binder<sculptcore::brush::StrokeMethod> {
  static const BindingBase *bind();
};
template <> struct Binder<sculptcore::brush::AnchoredLiveMode> {
  static const BindingBase *bind();
};
} // namespace litestl::binding

namespace sculptcore::brush {

using litestl::math::float2;
using litestl::math::float3;
using litestl::math::float4;
using litestl::util::Vector;

using double2 = litestl::math::Vec<double, 2>;
using double3 = litestl::math::Vec<double, 3>;
using double4 = litestl::math::Vec<double, 4>;

/**
 * 4x4 matrix in path.ux's row-vector convention: a point is a row on the left,
 * so `out[c] = sum_r v[r] * m[r][c]` and composition applies the left operand
 * first. litestl's `math::Matrix` uses the opposite convention, so the driver
 * carries its own type rather than silently transposing at every use.
 */
struct Mat4 {
  double m[4][4];

  Mat4()
  {
    identity();
  }

  void identity()
  {
    for (int i = 0; i < 4; i++) {
      for (int j = 0; j < 4; j++) {
        m[i][j] = i == j ? 1.0 : 0.0;
      }
    }
  }

  /** Transform a row vector; no perspective divide (matches Vector4). */
  double4 mulVec4(const double4 &v) const
  {
    double4 out;
    for (int c = 0; c < 4; c++) {
      out[c] = v[0] * m[0][c] + v[1] * m[1][c] + v[2] * m[2][c] + v[3] * m[3][c];
    }
    return out;
  }

  /** Transform a point (implicit w=1), dropping w. */
  double3 mulPoint(const double3 &v) const
  {
    double3 out;
    for (int c = 0; c < 3; c++) {
      out[c] = v[0] * m[0][c] + v[1] * m[1][c] + v[2] * m[2][c] + m[3][c];
    }
    return out;
  }

  /** Composition: the result applies `*this` first, then `b`. */
  Mat4 then(const Mat4 &b) const
  {
    Mat4 out;
    for (int i = 0; i < 4; i++) {
      for (int j = 0; j < 4; j++) {
        double sum = 0.0;
        for (int k = 0; k < 4; k++) {
          sum += m[i][k] * b.m[k][j];
        }
        out.m[i][j] = sum;
      }
    }
    return out;
  }

  void clearTranslation()
  {
    m[3][0] = m[3][1] = m[3][2] = 0.0;
  }

  Mat4 inverted() const;
};

/**
 * Camera + object transform snapshot for one poll batch. Unbound: the host
 * feeds it through `setViewRow`/`setViewParams` and reads derived matrices
 * back through `getMatrixElem`.
 */
struct StrokeView {
  Mat4 rendermat; // world->clip
  Mat4 obmat;     // local->world
  bool hasObjectMatrix = false;

  double3 cameraPos;
  double2 viewSize; // view3d local size, px
  double2 glSize;   // device px
  double camNear = 0.01;

  Mat4 irendermat;      // clip->world
  Mat4 iobmat;          // world->local
  Mat4 iobmatDir;       // world->local, translation cleared
  Mat4 localRendermat;  // local->clip
  Mat4 localIrendermat; // clip->local

  void update();

  /** Port of view3dProject: world point -> view-local px in xy, w returned. */
  double project(double *co, int n, const Mat4 &mat) const;
  /** Port of view3dUnproject. */
  double unproject(double *co, int n, const Mat4 &imat) const;
  /** Port of View3D.getViewVec: normalized world ray direction at a pixel. */
  double3 getViewVec(double x, double y) const;
};

/**
 * One emitted dab. Mirrors the fields of the TS `PaintSample` that the driver
 * actually fills; the host copies them into a real PaintSample. The stroke
 * curve slice rides as its four local-space control points (`hasCurve`), since
 * the binding runtime can't hand back a Bezier.
 */
struct DabSample {
  float4 p; // xyz object-local position, w = world projection w
  float4 dp;
  float2 screenP;
  float2 dScreenP;
  float strokeS = 0.0f;
  float dstrokeS = 0.0f;
  bool isInterp = false;
  float angle = 0.0f;
  float futureAngle = 0.0f;
  float3 vec;
  float4 color;
  float3 viewvec;
  float3 vieworigin;
  float3 viewPlane;
  float strength = 0.0f;
  float radius = 0.0f;
  float w = 0.0f;
  bool invert = false;
  float pressure = 1.0f;
  bool hit = true;
  bool useAltBrush = false;
  float3 anchorVec;
  float liveAngle = 0.0f;
  float tiltX = 0.0f;
  float tiltY = 0.0f;
  float twist = 0.0f;
  bool hasCurve = false;
  float3 curve0;
  float3 curve1;
  float3 curve2;
  float3 curve3;

  DabSample() = default;
  DabSample(const DabSample &b) = default;
  DabSample &operator=(const DabSample &b) = default;

  static litestl::binding::types::Struct<DabSample> *defineBindings();
};

/**
 * Stroke driver. Construct with the object's spatial tree (the no-arg form
 * never hits geometry, which is only useful for screen-space tests).
 *
 * Per batch the host pushes the view snapshot (`setViewRow` x8 +
 * `setViewParams`), pushes any queued inputs, then calls `poll()` and reads
 * `sampleAt(i)` for each of the returned count.
 */
struct BrushStrokeDriver {
  StrokeSpaceMode spaceMode = StrokeSpaceMode::Screen;
  StrokeMethod strokeMethod = StrokeMethod::Path;
  AnchoredLiveMode anchoredLiveMode = AnchoredLiveMode::Radius;
  bool radiusIsWorld = false;

  BrushStrokeDriver() = default;
  BrushStrokeDriver(spatial::SpatialTree *tree) : tree_(tree)
  {
  }

  /** matId 0 = rendermat (world->clip), 1 = obmat (local->world). */
  void setViewRow(int matId, int row, float x, float y, float z, float w);
  void setViewParams(float camX,
                     float camY,
                     float camZ,
                     float viewW,
                     float viewH,
                     float glW,
                     float glH,
                     float camNear,
                     bool hasObjectMatrix);

  /** Enqueue one pointer event; x/y are view3d-local pixels. */
  void push(float x,
            float y,
            float pressure,
            float tiltX,
            float tiltY,
            float twist,
            bool invert,
            bool useAltBrush,
            float radius,
            float strength,
            float spacing);
  /** Brush color for the next `push` (sticky; defaults to opaque white). */
  void pushColor(float r, float g, float b, float a);

  /** Signal pointer-up; the next poll() flushes the trailing segment. */
  void end();
  void reset();

  /** Drain queued events through the spline; returns the ready dab count. */
  int poll();
  DabSample *sampleAt(int i);

  /** true once end() has been called AND the trailing segment is drained */
  bool finished() const
  {
    return done_;
  }

  bool hasAnchorScreen() const
  {
    return hasAnchor_;
  }
  float anchorScreenX() const
  {
    return float(anchorCP_.screen[0]);
  }
  float anchorScreenY() const
  {
    return float(anchorCP_.screen[1]);
  }
  bool hasPreviewScreen() const
  {
    return hasPreview_;
  }
  float previewScreenX() const
  {
    return float(previewCP_.screen[0]);
  }
  float previewScreenY() const
  {
    return float(previewCP_.screen[1]);
  }

  /** matId 0 = localRendermat (local->clip), 1 = localIrendermat. */
  float getMatrixElem(int matId, int row, int col) const;
  float viewSizeX() const
  {
    return float(view_.viewSize[0]);
  }
  float viewSizeY() const
  {
    return float(view_.viewSize[1]);
  }

  static litestl::binding::types::Struct<BrushStrokeDriver> *defineBindings();

private:
  struct StrokeParams {
    double radius = 0.0;
    double strength = 0.0;
    double spacing = 0.0;
    double4 color;
  };

  struct StrokeInput {
    double x = 0.0, y = 0.0;
    double pressure = 1.0;
    double tiltX = 0.0, tiltY = 0.0, twist = 0.0;
    bool invert = false;
    bool useAltBrush = false;
    StrokeParams params;
  };

  struct ControlPoint {
    double2 screen;
    double3 world;
    double3 normal;
    double3 viewvec;
    bool hit = false;
    double pressure = 1.0;
    double tiltX = 0.0, tiltY = 0.0, twist = 0.0;
    bool invert = false;
    bool useAltBrush = false;
    StrokeParams params;
  };

  void ingest(const StrokeInput &input);
  void ingestAnchored(const ControlPoint &cp);
  void flush();
  void emitRaw(const ControlPoint &cp);
  void emitAnchored(const ControlPoint &anchor, const ControlPoint &cur);
  void emitDot(const ControlPoint &cp);
  void emitSegment(int i, bool rightClamp);
  void pushSample(const DabSample &ps);

  bool rayCast(const double3 &origin, const double3 &dir, double3 &p, double3 &normal);
  double3 synthesizeMiss(const double2 &screen);
  double3 projectOntoAnchorPlane(const double3 &origin, const double3 &viewvec);
  double worldRadiusAt(const double3 &worldP, double radiusPx);
  double screenRadiusAt(const double3 &worldP, double worldRadius);
  double3 toLocal(const double3 &p) const;
  double3 toLocalDir(const double3 &v) const;

  DabSample makeSample(const double3 &worldP,
                       const double2 &screenP,
                       const ControlPoint &cpA,
                       const ControlPoint &cpB,
                       double t,
                       const double3 &normalP,
                       bool hit);

  spatial::SpatialTree *tree_ = nullptr;
  StrokeView view_;

  Vector<StrokeInput> inQueue_;
  Vector<ControlPoint> cps_;
  Vector<DabSample> out_;

  double walkCarry_ = 0.0;
  double strokeS_ = 0.0;
  bool firstHit_ = false;
  double3 lastHitWorld_;
  DabSample prevEmitted_;
  bool hasPrevEmitted_ = false;
  // Index of prevEmitted_ inside out_ for the futureAngle back-patch; -1 when
  // the previous dab was returned in an earlier poll batch.
  int prevEmittedIdx_ = -1;
  bool rawEmitted_ = false;
  bool ended_ = false;
  bool done_ = false;

  ControlPoint anchorCP_;
  bool hasAnchor_ = false;
  ControlPoint previewCP_;
  bool hasPreview_ = false;

  double4 pendingColor_ = double4(1.0, 1.0, 1.0, 1.0);
};

} // namespace sculptcore::brush
