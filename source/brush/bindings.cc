#include "brush.h"
#include "brush_executor.h"
#include "stroke_driver.h"

#include "litestl/binding/manager.h"

namespace litestl::binding {
const BindingBase *Binder<sculptcore::brush::FalloffKind>::bind()
{
  using namespace sculptcore::brush;
  types::Enum *e = new types::Enum("sculptcore::gpu::FalloffKind", sizeof(FalloffKind));

  e->addItem("LINEAR", FalloffKind::Linear);
  e->addItem("SMOOTHSTEP", FalloffKind::Smoothstep);
  e->addItem("GUASSIAN", FalloffKind::Gaussian);
  e->addItem("CURVE", FalloffKind::Curve);
  return e;
}
const BindingBase *Binder<sculptcore::brush::FalloffShape>::bind()
{
  using namespace sculptcore::brush;
  types::Enum *e = new types::Enum("sculptcore::gpu::FalloffShape", sizeof(FalloffShape));

  e->addItem("SPHERICAL", FalloffShape::Spherical);
  e->addItem("CUBE", FalloffShape::Cube);
  e->addItem("LINEAR", FalloffShape::Linear);
  e->addItem("BOX", FalloffShape::Box);
  return e;
}
const BindingBase *Binder<sculptcore::brush::AttrElemDomain>::bind()
{
  using namespace sculptcore::brush;
  types::Enum *e =
      new types::Enum("sculptcore::brush::AttrElemDomain", sizeof(AttrElemDomain));

  e->addItem("VERTEX", AttrElemDomain::Vertex);
  e->addItem("FACE", AttrElemDomain::Face);
  e->addItem("EDGE", AttrElemDomain::Edge);
  e->addItem("CORNER", AttrElemDomain::Corner);
  return e;
}
const BindingBase *Binder<sculptcore::brush::TexCoordSpace>::bind()
{
  using namespace sculptcore::brush;
  types::Enum *e =
      new types::Enum("sculptcore::brush::TexCoordSpace", sizeof(TexCoordSpace));

  e->addItem("GLOBAL", TexCoordSpace::Global);
  e->addItem("VIEW_PLANE", TexCoordSpace::ViewPlane);
  e->addItem("VIEW_REPEAT", TexCoordSpace::ViewRepeat);
  e->addItem("STROKE_CURVED", TexCoordSpace::StrokeCurved);
  e->addItem("PROJECTED", TexCoordSpace::Projected);
  return e;
}
const BindingBase *Binder<sculptcore::brush::StrokeSpaceMode>::bind()
{
  using namespace sculptcore::brush;
  types::Enum *e =
      new types::Enum("sculptcore::brush::StrokeSpaceMode", sizeof(StrokeSpaceMode));

  e->addItem("SCREEN", StrokeSpaceMode::Screen);
  e->addItem("WORLD", StrokeSpaceMode::World);
  return e;
}
const BindingBase *Binder<sculptcore::brush::StrokeMethod>::bind()
{
  using namespace sculptcore::brush;
  types::Enum *e =
      new types::Enum("sculptcore::brush::StrokeMethod", sizeof(StrokeMethod));

  e->addItem("PATH", StrokeMethod::Path);
  e->addItem("ANCHORED", StrokeMethod::Anchored);
  e->addItem("DRAG_DOT", StrokeMethod::DragDot);
  return e;
}
const BindingBase *Binder<sculptcore::brush::AnchoredLiveMode>::bind()
{
  using namespace sculptcore::brush;
  types::Enum *e =
      new types::Enum("sculptcore::brush::AnchoredLiveMode", sizeof(AnchoredLiveMode));

  e->addItem("RADIUS", AnchoredLiveMode::Radius);
  e->addItem("ANGLE", AnchoredLiveMode::Angle);
  return e;
}
} // namespace litestl::binding

namespace sculptcore::brush {

litestl::binding::types::Struct<DabSample> *DabSample::defineBindings()
{
  using namespace litestl::binding;
  types::Struct<DabSample> *st =
      new types::Struct<DabSample>("sculptcore::brush::DabSample", sizeof(DabSample));

  BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);

  BIND_STRUCT_MEMBER(st, p);
  BIND_STRUCT_MEMBER(st, dp);
  BIND_STRUCT_MEMBER(st, screenP);
  BIND_STRUCT_MEMBER(st, dScreenP);
  BIND_STRUCT_MEMBER(st, strokeS);
  BIND_STRUCT_MEMBER(st, dstrokeS);
  BIND_STRUCT_MEMBER(st, isInterp);
  BIND_STRUCT_MEMBER(st, angle);
  BIND_STRUCT_MEMBER(st, futureAngle);
  BIND_STRUCT_MEMBER(st, vec);
  BIND_STRUCT_MEMBER(st, color);
  BIND_STRUCT_MEMBER(st, viewvec);
  BIND_STRUCT_MEMBER(st, vieworigin);
  BIND_STRUCT_MEMBER(st, viewPlane);
  BIND_STRUCT_MEMBER(st, strength);
  BIND_STRUCT_MEMBER(st, radius);
  BIND_STRUCT_MEMBER(st, w);
  BIND_STRUCT_MEMBER(st, invert);
  BIND_STRUCT_MEMBER(st, pressure);
  BIND_STRUCT_MEMBER(st, hit);
  BIND_STRUCT_MEMBER(st, useAltBrush);
  BIND_STRUCT_MEMBER(st, anchorVec);
  BIND_STRUCT_MEMBER(st, liveAngle);
  BIND_STRUCT_MEMBER(st, tiltX);
  BIND_STRUCT_MEMBER(st, tiltY);
  BIND_STRUCT_MEMBER(st, twist);
  BIND_STRUCT_MEMBER(st, hasCurve);
  BIND_STRUCT_MEMBER(st, curve0);
  BIND_STRUCT_MEMBER(st, curve1);
  BIND_STRUCT_MEMBER(st, curve2);
  BIND_STRUCT_MEMBER(st, curve3);

  return st;
}

litestl::binding::types::Struct<BrushStrokeDriver> *BrushStrokeDriver::defineBindings()
{
  using namespace litestl::binding;
  types::Struct<BrushStrokeDriver> *st = new types::Struct<BrushStrokeDriver>(
      "sculptcore::brush::BrushStrokeDriver", sizeof(BrushStrokeDriver));

  BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
  BIND_STRUCT_CONSTRUCTOR(st, "main", spatial::SpatialTree *);

  BIND_STRUCT_MEMBER(st, spaceMode);
  BIND_STRUCT_MEMBER(st, strokeMethod);
  BIND_STRUCT_MEMBER(st, anchoredLiveMode);
  BIND_STRUCT_MEMBER(st, radiusIsWorld);

  BIND_STRUCT_METHOD(st, setViewRow, MARGS("matId", "row", "x", "y", "z", "w"));
  BIND_STRUCT_METHOD(st,
                     setViewParams,
                     MARGS("camX",
                           "camY",
                           "camZ",
                           "viewW",
                           "viewH",
                           "glW",
                           "glH",
                           "camNear",
                           "hasObjectMatrix"));
  BIND_STRUCT_METHOD(st,
                     push,
                     MARGS("x",
                           "y",
                           "pressure",
                           "tiltX",
                           "tiltY",
                           "twist",
                           "invert",
                           "useAltBrush",
                           "radius",
                           "strength",
                           "spacing"));
  BIND_STRUCT_METHOD(st, pushColor, MARGS("r", "g", "b", "a"));
  BIND_STRUCT_METHOD(st, end, MARGS());
  BIND_STRUCT_METHOD(st, reset, MARGS());
  BIND_STRUCT_METHOD(st, poll, MARGS());
  BIND_STRUCT_METHOD(st, sampleAt, MARGS("i"));
  BIND_STRUCT_METHOD(st, finished, MARGS());
  BIND_STRUCT_METHOD(st, hasAnchorScreen, MARGS());
  BIND_STRUCT_METHOD(st, anchorScreenX, MARGS());
  BIND_STRUCT_METHOD(st, anchorScreenY, MARGS());
  BIND_STRUCT_METHOD(st, hasPreviewScreen, MARGS());
  BIND_STRUCT_METHOD(st, previewScreenX, MARGS());
  BIND_STRUCT_METHOD(st, previewScreenY, MARGS());
  BIND_STRUCT_METHOD(st, getMatrixElem, MARGS("matId", "row", "col"));
  BIND_STRUCT_METHOD(st, viewSizeX, MARGS());
  BIND_STRUCT_METHOD(st, viewSizeY, MARGS());

  return st;
}

} // namespace sculptcore::brush

namespace sculptcore::brush::bindings {
void registerBindings(litestl::binding::BindingManager &manager)
{
  using namespace litestl::binding;
  manager.add(Bind<Brush>());
  manager.add(Bind<BrushProgram>());
  manager.add(Bind<CommandExecutor>());
  manager.add(Bind<BrushUniformManifestEntry>());
  manager.add(Bind<BrushAttrManifestEntry>());
  manager.add(Bind<TextureProgramParam>());
  manager.add(Bind<BrushDefFlags>());
  manager.add(Bind<BrushMetadata>());
  manager.add(Bind<StrokeSpaceMode>());
  manager.add(Bind<StrokeMethod>());
  manager.add(Bind<AnchoredLiveMode>());
  manager.add(Bind<DabSample>());
  manager.add(Bind<BrushStrokeDriver>());
  manager.add(Bind<util::Vector<spatial::SpatialNode *>>());
}
} // namespace sculptcore::brush::bindings
