#include "stroke_driver.h"

#include "litestl/math/matrix.h"
#include "spatial/spatial.h"

#include <cmath>

namespace sculptcore::brush {

using curve::arcLengthWalk;
using curve::crToBezier;
using curve::Cubic;
using curve::evalCubic;
using curve::lerpV;
using curve::subCubic;

/** Centripetal Catmull-Rom: no cusps/loops on clustered jittery input. */
static constexpr double ALPHA = 0.5;
/** Parameter half-width of the per-dab world-curve slice stored on the sample. */
static constexpr double SLICE_HALF = 0.15;

static inline double lerpNum(double a, double b, double t)
{
  return a + (b - a) * t;
}

static inline float3 toFloat3(const double3 &v)
{
  return float3(float(v[0]), float(v[1]), float(v[2]));
}

static inline double3 normalized3(const double3 &v)
{
  double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (len == 0.0) {
    return v;
  }
  return double3(v[0] / len, v[1] / len, v[2] / len);
}

Mat4 Mat4::inverted() const
{
  // Matrix::invert() is layout-agnostic — it round-trips the buffer through
  // Eigen and back, so the result is the true inverse in our own indexing.
  litestl::math::Matrix<double, 4> tmp(&m[0][0]);
  tmp.invert();

  Mat4 out;
  const double *src = static_cast<const double *>(tmp);
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      out.m[i][j] = src[i * 4 + j];
    }
  }
  return out;
}

void StrokeView::update()
{
  irendermat = rendermat.inverted();

  iobmat.identity();
  localRendermat = rendermat;

  if (hasObjectMatrix) {
    iobmat = obmat.inverted();
    // local->clip: obmat (local->world) applies before the camera rendermat.
    localRendermat = obmat.then(rendermat);
  }

  iobmatDir = iobmat;
  iobmatDir.clearTranslation();
  localIrendermat = localRendermat.inverted();
}

double StrokeView::project(double *co, int n, const Mat4 &mat) const
{
  double4 tmp;
  tmp[0] = co[0];
  tmp[1] = co[1];
  if (n > 2) {
    tmp[2] = co[2];
  }
  tmp[3] = 1.0;

  tmp = mat.mulVec4(tmp);

  if (tmp[3] != 0.0) {
    tmp[0] /= tmp[3];
    tmp[1] /= tmp[3];
    tmp[2] /= tmp[3];
  }

  const double w = tmp[3];

  tmp[0] = (tmp[0] * 0.5 + 0.5) * viewSize[0];
  tmp[1] = (1.0 - (tmp[1] * 0.5 + 0.5)) * viewSize[1];

  for (int i = 0; i < n; i++) {
    co[i] = tmp[i];
  }

  return w;
}

double StrokeView::unproject(double *co, int n, const Mat4 &imat) const
{
  double4 tmp;
  tmp[0] = (co[0] / viewSize[0]) * 2.0 - 1.0;
  tmp[1] = (1.0 - co[1] / viewSize[1]) * 2.0 - 1.0;

  if (n > 2) {
    tmp[2] = co[2];
  }
  tmp[3] = n > 3 ? co[3] : 1.0;

  tmp = imat.mulVec4(tmp);

  const double w = tmp[3];

  if (tmp[3] != 0.0) {
    tmp[0] /= tmp[3];
    tmp[1] /= tmp[3];
    tmp[2] /= tmp[3];
  }

  for (int i = 0; i < n; i++) {
    co[i] = tmp[i];
  }

  return w;
}

double3 StrokeView::getViewVec(double x, double y) const
{
  double co[3] = {x, y, -camNear - 0.001};
  unproject(co, 3, irendermat);

  return normalized3(
      double3(co[0] - cameraPos[0], co[1] - cameraPos[1], co[2] - cameraPos[2]));
}

void BrushStrokeDriver::setViewRow(int matId, int row, float x, float y, float z, float w)
{
  if (row < 0 || row > 3) {
    return;
  }

  Mat4 &mat = matId == 0 ? view_.rendermat : view_.obmat;
  mat.m[row][0] = x;
  mat.m[row][1] = y;
  mat.m[row][2] = z;
  mat.m[row][3] = w;
}

void BrushStrokeDriver::setViewParams(float camX,
                                      float camY,
                                      float camZ,
                                      float viewW,
                                      float viewH,
                                      float glW,
                                      float glH,
                                      float camNear,
                                      bool hasObjectMatrix)
{
  view_.cameraPos = double3(camX, camY, camZ);
  view_.viewSize = double2(viewW, viewH);
  view_.glSize = double2(glW, glH);
  view_.camNear = camNear;
  view_.hasObjectMatrix = hasObjectMatrix;
}

float BrushStrokeDriver::getMatrixElem(int matId, int row, int col) const
{
  if (row < 0 || row > 3 || col < 0 || col > 3) {
    return 0.0f;
  }
  const Mat4 &mat = matId == 0 ? view_.localRendermat : view_.localIrendermat;
  return float(mat.m[row][col]);
}

void BrushStrokeDriver::pushColor(float r, float g, float b, float a)
{
  pendingColor_ = double4(r, g, b, a);
}

void BrushStrokeDriver::push(float x,
                             float y,
                             float pressure,
                             float tiltX,
                             float tiltY,
                             float twist,
                             bool invert,
                             bool useAltBrush,
                             float radius,
                             float strength,
                             float spacing)
{
  if (ended_) {
    return;
  }

  StrokeInput input;
  input.x = x;
  input.y = y;
  input.pressure = pressure;
  input.tiltX = tiltX;
  input.tiltY = tiltY;
  input.twist = twist;
  input.invert = invert;
  input.useAltBrush = useAltBrush;
  input.params.radius = radius;
  input.params.strength = strength;
  input.params.spacing = spacing;
  input.params.color = pendingColor_;

  inQueue_.append(input);
}

void BrushStrokeDriver::end()
{
  ended_ = true;
}

void BrushStrokeDriver::reset()
{
  inQueue_.clear();
  cps_.clear();
  out_.clear();
  walkCarry_ = 0.0;
  strokeS_ = 0.0;
  firstHit_ = false;
  lastHitWorld_ = double3(0.0, 0.0, 0.0);
  hasPrevEmitted_ = false;
  prevEmittedIdx_ = -1;
  rawEmitted_ = false;
  ended_ = false;
  done_ = false;
  hasAnchor_ = false;
  hasPreview_ = false;
}

int BrushStrokeDriver::poll()
{
  out_.clear();
  prevEmittedIdx_ = -1;

  if (done_) {
    return 0;
  }

  view_.update();

  for (int i = 0; i < int(inQueue_.size()); i++) {
    ingest(inQueue_[i]);
  }
  inQueue_.clear();

  if (ended_) {
    flush();
    done_ = true;
  }

  return int(out_.size());
}

DabSample *BrushStrokeDriver::sampleAt(int i)
{
  if (i < 0 || i >= int(out_.size())) {
    return nullptr;
  }
  return &out_[i];
}

bool BrushStrokeDriver::rayCast(const double3 &origin,
                                const double3 &dir,
                                double3 &p,
                                double3 &normal)
{
  if (!tree_) {
    return false;
  }

  // World -> object local; directions must not pick up the object translation.
  const double3 o = view_.iobmat.mulPoint(origin);
  const double3 d = view_.iobmatDir.mulPoint(dir);

  spatial::CastRayIsect isect;
  float3 of{float(o[0]), float(o[1]), float(o[2])};
  float3 df{float(d[0]), float(d[1]), float(d[2])};

  if (!tree_->castRay(of, df, isect)) {
    return false;
  }

  const double3 lp(isect.p[0], isect.p[1], isect.p[2]);
  const double3 ln(isect.normal[0], isect.normal[1], isect.normal[2]);

  Mat4 dirmat = view_.obmat;
  dirmat.clearTranslation();

  p = view_.obmat.mulPoint(lp);
  normal = normalized3(dirmat.mulPoint(ln));
  return true;
}

void BrushStrokeDriver::ingest(const StrokeInput &input)
{
  const double2 screen(input.x, input.y);
  const double3 viewvec = view_.getViewVec(screen[0], screen[1]);
  const double3 origin = view_.cameraPos;

  double3 world;
  double3 normal;
  bool hit = false;

  if (strokeMethod == StrokeMethod::Anchored && hasAnchor_) {
    // Grab-family drag over empty space: once anchored, every later input
    // projects onto the plane through the anchor, facing the camera as it was
    // at anchor time rather than the live (actively deforming) surface.
    world = projectOntoAnchorPlane(origin, viewvec);
    normal = anchorCP_.viewvec;
  } else {
    double3 hitP, hitN;
    if (rayCast(origin, viewvec, hitP, hitN)) {
      world = hitP;
      normal = hitN;
      hit = true;
      firstHit_ = true;
      lastHitWorld_ = world;
    } else if (strokeMethod == StrokeMethod::Anchored) {
      // Anchored can't start a stroke over empty space.
      return;
    } else if (firstHit_) {
      world = synthesizeMiss(screen);
      normal = viewvec;
    } else if (spaceMode == StrokeSpaceMode::Screen) {
      world = double3(origin[0] + viewvec[0], origin[1] + viewvec[1], origin[2] + viewvec[2]);
      normal = viewvec;
    } else {
      // world-mode before any hit: no plane to project onto yet, discard
      return;
    }
  }

  ControlPoint cp;
  cp.screen = screen;
  cp.world = world;
  cp.normal = normal;
  cp.viewvec = viewvec;
  cp.hit = hit;
  cp.pressure = input.pressure;
  cp.tiltX = input.tiltX;
  cp.tiltY = input.tiltY;
  cp.twist = input.twist;
  cp.invert = input.invert;
  cp.useAltBrush = input.useAltBrush;
  cp.params = input.params;

  if (strokeMethod == StrokeMethod::Anchored) {
    ingestAnchored(cp);
    return;
  } else if (strokeMethod == StrokeMethod::DragDot) {
    emitDot(cp);
    return;
  }

  cps_.append(cp);

  // first control point => one raw, non-interpolated dab
  if (!rawEmitted_) {
    rawEmitted_ = true;
    emitRaw(cps_[cps_.size() - 1]);
    return;
  }

  // 1-segment lookahead: with a right neighbor present, the segment between
  // cps[L-3] and cps[L-2] is now fully determined.
  const int L = int(cps_.size());
  if (L >= 3) {
    emitSegment(L - 3, false);
  }
}

void BrushStrokeDriver::ingestAnchored(const ControlPoint &cp)
{
  if (!hasAnchor_) {
    anchorCP_ = cp;
    hasAnchor_ = true;
    emitAnchored(anchorCP_, cp);
    return;
  }
  emitAnchored(anchorCP_, cp);
}

void BrushStrokeDriver::flush()
{
  if (strokeMethod != StrokeMethod::Path) {
    // ANCHORED/DRAG_DOT already emitted one dab per input.
    return;
  }

  const int L = int(cps_.size());
  if (L >= 2) {
    emitSegment(L - 2, true);
  }
}

double3 BrushStrokeDriver::synthesizeMiss(const double2 &screen)
{
  double p[4] = {lastHitWorld_[0], lastHitWorld_[1], lastHitWorld_[2], 1.0};

  view_.project(p, 4, view_.rendermat); // -> screen px, keeps depth in p[2]
  p[0] = screen[0];
  p[1] = screen[1];
  view_.unproject(p, 4, view_.irendermat);

  return double3(p[0], p[1], p[2]);
}

void BrushStrokeDriver::pushSample(const DabSample &ps)
{
  out_.append(ps);
  prevEmitted_ = ps;
  hasPrevEmitted_ = true;
  prevEmittedIdx_ = int(out_.size()) - 1;
}

void BrushStrokeDriver::emitRaw(const ControlPoint &cp)
{
  DabSample ps = makeSample(cp.world, cp.screen, cp, cp, 0.0, cp.normal, cp.hit);
  ps.isInterp = false;
  ps.strokeS = float(strokeS_);
  ps.dstrokeS = 0.0f;
  ps.angle = 0.0f;
  ps.futureAngle = 0.0f;
  ps.hasCurve = false;

  pushSample(ps);
}

void BrushStrokeDriver::emitAnchored(const ControlPoint &anchor, const ControlPoint &cur)
{
  DabSample ps =
      makeSample(anchor.world, anchor.screen, anchor, anchor, 0.0, anchor.normal, anchor.hit);
  ps.isInterp = false;
  ps.strokeS = float(strokeS_);
  ps.dstrokeS = 0.0f;
  ps.hasCurve = false;

  const double3 worldVec(cur.world[0] - anchor.world[0],
                         cur.world[1] - anchor.world[1],
                         cur.world[2] - anchor.world[2]);
  ps.anchorVec = toFloat3(toLocalDir(worldVec));

  const double dx = cur.screen[0] - anchor.screen[0];
  const double dy = cur.screen[1] - anchor.screen[1];

  // The brush angle tracks the cursor in *both* live modes; the mode only
  // decides whether the drag length also drives the radius.
  ps.liveAngle = float(std::atan2(dy, dx));

  if (anchoredLiveMode != AnchoredLiveMode::Angle) {
    const double dragPx = std::sqrt(dx * dx + dy * dy);
    if (dragPx > 1e-5) {
      // ps.radius rides in the brush's own unit.
      ps.radius = float(radiusIsWorld ? worldRadiusAt(anchor.world, dragPx) : dragPx);
    }
  }

  pushSample(ps);
}

void BrushStrokeDriver::emitDot(const ControlPoint &cp)
{
  DabSample ps = makeSample(cp.world, cp.screen, cp, cp, 0.0, cp.normal, cp.hit);
  ps.isInterp = false;
  ps.strokeS = float(strokeS_);
  ps.dstrokeS = 0.0f;
  ps.angle = 0.0f;
  ps.futureAngle = 0.0f;
  ps.hasCurve = false;

  previewCP_ = cp;
  hasPreview_ = true;

  pushSample(ps);
}

void BrushStrokeDriver::emitSegment(int i, bool rightClamp)
{
  const ControlPoint &p1 = cps_[i];
  const ControlPoint &p2 = cps_[i + 1];
  const ControlPoint &p0 = i > 0 ? cps_[i - 1] : p1;
  const ControlPoint &p3 = rightClamp ? p2 : cps_[i + 2];

  const Cubic<2> screenB = crToBezier(p0.screen, p1.screen, p2.screen, p3.screen, ALPHA);
  const Cubic<3> worldB = crToBezier(p0.world, p1.world, p2.world, p3.world, ALPHA);

  const double spacing = p2.params.spacing;
  const double radius = p2.params.radius;

  double spacingDist;
  curve::WalkResult walk;

  if (spaceMode == StrokeSpaceMode::World) {
    const double worldRadius =
        radiusIsWorld ? radius : worldRadiusAt(evalCubic(worldB, 0.5), radius);
    spacingDist = std::max(spacing * 2.0 * worldRadius, 1e-5);
    walk = arcLengthWalk(worldB, spacingDist, walkCarry_);
  } else {
    // A world-unit radius must resolve to px before driving the screen walk, or
    // sub-pixel spacing floods the stroke with dabs.
    const double radiusPx =
        radiusIsWorld ? screenRadiusAt(evalCubic(worldB, 0.5), radius) : radius;
    spacingDist = std::max(spacing * 2.0 * radiusPx, 1e-5);
    walk = arcLengthWalk(screenB, spacingDist, walkCarry_);
  }

  walkCarry_ = walk.carryOut;

  for (double t : walk.ts) {
    const double3 worldP = evalCubic(worldB, t);
    const double2 screenP = evalCubic(screenB, t);
    const double3 normalP = lerpV(p1.normal, p2.normal, t);
    const bool hit = t < 0.5 ? p1.hit : p2.hit;

    DabSample ps = makeSample(worldP, screenP, p1, p2, t, normalP, hit);
    ps.isInterp = true;

    strokeS_ += spacing;
    ps.strokeS = float(strokeS_);
    ps.dstrokeS = float(spacing);

    if (hasPrevEmitted_) {
      ps.dScreenP[0] = ps.screenP[0] - prevEmitted_.screenP[0];
      ps.dScreenP[1] = ps.screenP[1] - prevEmitted_.screenP[1];
      ps.dp[0] = ps.p[0] - prevEmitted_.p[0];
      ps.dp[1] = ps.p[1] - prevEmitted_.p[1];
      ps.dp[2] = ps.p[2] - prevEmitted_.p[2];
      ps.angle = std::atan2(ps.dScreenP[1], ps.dScreenP[0]);

      if (prevEmittedIdx_ >= 0) {
        out_[prevEmittedIdx_].futureAngle = ps.angle;
      }
      prevEmitted_.futureAngle = ps.angle;
    }

    const Cubic<3> slice = subCubic(worldB, t - SLICE_HALF, t + SLICE_HALF);
    ps.hasCurve = true;
    ps.curve0 = toFloat3(toLocal(slice[0]));
    ps.curve1 = toFloat3(toLocal(slice[1]));
    ps.curve2 = toFloat3(toLocal(slice[2]));
    ps.curve3 = toFloat3(toLocal(slice[3]));
    ps.futureAngle = ps.angle;

    pushSample(ps);
  }
}

double BrushStrokeDriver::worldRadiusAt(const double3 &worldP, double radiusPx)
{
  double p[4] = {worldP[0], worldP[1], worldP[2], 1.0};
  const double w = view_.project(p, 4, view_.rendermat);
  return (radiusPx / std::max(view_.glSize[0], view_.glSize[1])) * std::fabs(w);
}

double BrushStrokeDriver::screenRadiusAt(const double3 &worldP, double worldRadius)
{
  const double worldPerPx = worldRadiusAt(worldP, 1.0);
  return worldRadius / std::max(worldPerPx, 1e-12);
}

double3 BrushStrokeDriver::toLocal(const double3 &p) const
{
  return view_.iobmat.mulPoint(p);
}

double3 BrushStrokeDriver::toLocalDir(const double3 &v) const
{
  return view_.iobmatDir.mulPoint(v);
}

double3 BrushStrokeDriver::projectOntoAnchorPlane(const double3 &origin,
                                                  const double3 &viewvec)
{
  const double3 &n = anchorCP_.viewvec;
  const double denom = viewvec[0] * n[0] + viewvec[1] * n[1] + viewvec[2] * n[2];

  if (std::fabs(denom) > 1e-7) {
    const double d = (anchorCP_.world[0] - origin[0]) * n[0] +
                     (anchorCP_.world[1] - origin[1]) * n[1] +
                     (anchorCP_.world[2] - origin[2]) * n[2];
    const double s = d / denom;
    return double3(
        origin[0] + viewvec[0] * s, origin[1] + viewvec[1] * s, origin[2] + viewvec[2] * s);
  }

  return anchorCP_.world;
}

DabSample BrushStrokeDriver::makeSample(const double3 &worldP,
                                        const double2 &screenP,
                                        const ControlPoint &cpA,
                                        const ControlPoint &cpB,
                                        double t,
                                        const double3 &normalP,
                                        bool hit)
{
  DabSample ps;

  double p[4] = {worldP[0], worldP[1], worldP[2], 1.0};
  // World projection w (depth/scale hint); kept as-is regardless of space.
  const double w = view_.project(p, 4, view_.rendermat);

  const double3 lp = toLocal(worldP);
  ps.p = float4(float(lp[0]), float(lp[1]), float(lp[2]), float(w));
  ps.w = float(w);
  ps.screenP = float2(float(screenP[0]), float(screenP[1]));

  // Surface normal + view ray: world -> local through the translation-free
  // inverse, then re-normalized (the inverse scale changes their length).
  ps.vec = toFloat3(normalized3(toLocalDir(normalized3(normalP))));

  const double3 viewvec = lerpV(cpA.viewvec, cpB.viewvec, t);
  const double3 localViewvec = normalized3(toLocalDir(normalized3(viewvec)));
  ps.viewvec = toFloat3(localViewvec);
  ps.viewPlane = toFloat3(localViewvec);

  const double3 lo = toLocal(view_.cameraPos);
  ps.vieworigin = toFloat3(lo);

  ps.radius = float(lerpNum(cpA.params.radius, cpB.params.radius, t));
  ps.strength = float(lerpNum(cpA.params.strength, cpB.params.strength, t));
  ps.pressure = float(lerpNum(cpA.pressure, cpB.pressure, t));
  ps.tiltX = float(lerpNum(cpA.tiltX, cpB.tiltX, t));
  ps.tiltY = float(lerpNum(cpA.tiltY, cpB.tiltY, t));
  ps.twist = float(lerpNum(cpA.twist, cpB.twist, t));
  ps.invert = t < 0.5 ? cpA.invert : cpB.invert;
  ps.useAltBrush = t < 0.5 ? cpA.useAltBrush : cpB.useAltBrush;

  for (int i = 0; i < 4; i++) {
    ps.color[i] = float(lerpNum(cpA.params.color[i], cpB.params.color[i], t));
  }
  ps.hit = hit;

  return ps;
}

} // namespace sculptcore::brush
