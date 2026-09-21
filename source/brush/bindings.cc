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
  e->addItem("ROUNDED_BOX", FalloffShape::RoundedBox);
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


litestl::binding::types::Struct<Brush> *Brush::defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<Brush> *st =
        new types::Struct<Brush>("sculptcore::brush::Brush", sizeof(Brush));

    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);

    BIND_STRUCT_MEMBER(st, falloffCurveSize);
    BIND_STRUCT_MEMBER(st, falloff_shape);
    BIND_STRUCT_MEMBER(st, falloff_kind);
    BIND_STRUCT_MEMBER(st, strength);
    BIND_STRUCT_MEMBER(st, radius);
    BIND_STRUCT_MEMBER(st, spacing);
    BIND_STRUCT_MEMBER(st, planeoff);
    BIND_STRUCT_MEMBER(st, autosmooth);
    BIND_STRUCT_MEMBER(st, invert);
    BIND_STRUCT_MEMBER(st, mu);
    BIND_STRUCT_MEMBER(st, nu);
    BIND_STRUCT_MEMBER(st, unboundedExtent);
    BIND_STRUCT_MEMBER(st, pinch);
    BIND_STRUCT_MEMBER(st, planeHeight);
    BIND_STRUCT_MEMBER(st, planeDepth);
    BIND_STRUCT_MEMBER(st, rotateAngle);
    BIND_STRUCT_MEMBER(st, projection);
    BIND_STRUCT_MEMBER(st, rake);
    BIND_STRUCT_MEMBER(st, reproject_uvs);
    BIND_STRUCT_MEMBER(st, automask_cavity);
    BIND_STRUCT_MEMBER(st, cavity_factor);
    BIND_STRUCT_MEMBER(st, cavity_blur_steps);
    BIND_STRUCT_MEMBER(st, cavity_inverted);
    BIND_STRUCT_MEMBER(st, cavity_use_curve);
    BIND_STRUCT_MEMBER(st, cavityCurveSize);
    BIND_STRUCT_MEMBER(st, automask_view_normal);
    BIND_STRUCT_MEMBER(st, cull_backfaces);
    BIND_STRUCT_MEMBER(st, view_normal_limit);
    BIND_STRUCT_MEMBER(st, view_normal_falloff);
    BIND_STRUCT_MEMBER(st, viewDir);
    BIND_STRUCT_MEMBER(st, enhance_rings);
    BIND_STRUCT_MEMBER(st, enhance_inner);
    BIND_STRUCT_MEMBER(st, grabFrom);
    BIND_STRUCT_MEMBER(st, grabTo);
    BIND_STRUCT_MEMBER(st, falloff_dir);
    BIND_STRUCT_MEMBER(st, falloff_extent);
    BIND_STRUCT_MEMBER(st, falloff_roundness);
    BIND_STRUCT_MEMBER(st, planeSide);
    BIND_STRUCT_MEMBER(st, strokeDir);
    BIND_STRUCT_MEMBER(st, strokeDirHostSet);
    BIND_STRUCT_MEMBER(st, wingAngle);
    BIND_STRUCT_MEMBER(st, wingNormalA);
    BIND_STRUCT_MEMBER(st, wingNormalB);
    BIND_STRUCT_MEMBER(st, activeGroup);
    BIND_STRUCT_MEMBER(st, brushColor);
    BIND_STRUCT_MEMBER(st, mixMode);
    BIND_STRUCT_MEMBER(st, tex_width);
    BIND_STRUCT_MEMBER(st, tex_height);
    BIND_STRUCT_MEMBER(st, coord_space);
    BIND_STRUCT_MEMBER(st, tex_repeat);
    BIND_STRUCT_MEMBER(st, props);
    BIND_STRUCT_METHOD(st, setNamedFloat, MARGS("slot", "value"));
    BIND_STRUCT_METHOD(st, getNamedFloat, MARGS("slot"));
    BIND_STRUCT_METHOD(st, setFalloffCurveEntry, MARGS("i", "f"));
    BIND_STRUCT_METHOD(st, setCavityCurveEntry, MARGS("i", "f"));
    BIND_STRUCT_METHOD(st, replaceFalloffCurveChecked, MARGS("samples"));
    BIND_STRUCT_METHOD(st, replaceCavityCurveChecked, MARGS("samples"));
    BIND_STRUCT_METHOD(st, setTexture, MARGS("width", "height", "pixels"));
    BIND_STRUCT_METHOD(st, clearTexture, MARGS());
    BIND_STRUCT_MEMBER(st, texture_script_error);
    BIND_STRUCT_METHOD(st, setTextureScript, MARGS("source"));
    BIND_STRUCT_METHOD(st, clearTextureScript, MARGS());
    BIND_STRUCT_METHOD(st, textureParamCount, MARGS());
    BIND_STRUCT_METHOD(st, queriedTextureParamEntry, MARGS("i"));
    BIND_STRUCT_METHOD(st, setTextureParamAt, MARGS("i", "value"));
    BIND_STRUCT_METHOD(st, setTextureRampAt, MARGS("i", "lut"));
    BIND_STRUCT_METHOD(st, evalTextureAt, MARGS("px", "py", "pz", "nx", "ny", "nz"));
    BIND_STRUCT_METHOD(st, textureUsesMap, MARGS());
    BIND_STRUCT_METHOD(st, loadProps, MARGS());
    BIND_STRUCT_METHOD(st, writeProps, MARGS());
    BIND_STRUCT_METHOD(st, writeDabProps, MARGS());
    BIND_STRUCT_METHOD(st, pushDeviceInput, MARGS("type", "value"));
    BIND_STRUCT_METHOD(st, clearDeviceInputs, MARGS());
    BIND_STRUCT_METHOD(st, clearPropDynamics, MARGS("propId"));
    BIND_STRUCT_METHOD(
        st, addPropDynamic, MARGS("propId", "deviceType", "mixMode", "mixFactor"));
    BIND_STRUCT_METHOD(
        st, setPropDynamicSample, MARGS("propId", "deviceType", "i", "n", "value"));
    // Name-keyed dynamics for any registered float uniform (custom kernel
    // uniforms the BrushProp ids can't reach). The bridge enumerates the
    // uniform manifest and routes its configure calls through these.
    BIND_STRUCT_METHOD(st, clearPropDynamicsByName, MARGS("name"));
    BIND_STRUCT_METHOD(
        st, addPropDynamicByName, MARGS("name", "deviceType", "mixMode", "mixFactor"));
    BIND_STRUCT_METHOD(
        st, setPropDynamicSampleByName, MARGS("name", "deviceType", "i", "n", "value"));
    BIND_STRUCT_METHOD(st, setPropsParent, MARGS("parentProps"));
    BIND_STRUCT_METHOD(st, clearPropsParent, MARGS());
    BIND_STRUCT_METHOD(st,
                       replaceCommonResponseDynamicsChecked,
                       MARGS("propId",
                             "scalarType",
                             "devices",
                             "modes",
                             "factors",
                             "enabled",
                             "offsets",
                             "samples",
                             "kinds",
                             "parameters"));
    BIND_STRUCT_METHOD(st, configurationGeneration, MARGS());
    BIND_STRUCT_METHOD(
        st, readCommonScalarChecked, MARGS("propId", "scalarType", "evaluate"));
    BIND_STRUCT_METHOD(
        st, writeCommonScalarChecked, MARGS("propId", "scalarType", "value"));
    BIND_STRUCT_METHOD(st,
                       configureCommonDynamicChecked,
                       MARGS("propId", "scalarType", "device", "mode", "factor"));
    BIND_STRUCT_METHOD(st,
                       enableCommonDynamicChecked,
                       MARGS("propId", "scalarType", "device", "enabled"));
    BIND_STRUCT_METHOD(
        st, moveCommonDynamicChecked, MARGS("propId", "scalarType", "device", "index"));
    BIND_STRUCT_METHOD(st, clearCommonDynamicsChecked, MARGS("propId", "scalarType"));
    BIND_STRUCT_METHOD(st,
                       replaceCommonDynamicTableChecked,
                       MARGS("propId", "scalarType", "device", "samples"));
    BIND_STRUCT_METHOD(
        st,
        setCommonDynamicSampleChecked,
        MARGS("propId", "scalarType", "device", "index", "count", "value"));
    BIND_STRUCT_METHOD(st,
                       replaceCommonDynamicsChecked,
                       MARGS("propId",
                             "scalarType",
                             "devices",
                             "modes",
                             "factors",
                             "enabled",
                             "offsets",
                             "samples"));

    return st;
  }



  litestl::binding::types::Struct<CommandExecutor> *CommandExecutor::defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<CommandExecutor> *st = new types::Struct<CommandExecutor>(
        "sculptcore::brush::CommandExecutor", sizeof(CommandExecutor));

    BIND_STRUCT_CONSTRUCTOR(st, "main", SpatialTree *, Brush *);
    BIND_STRUCT_MEMBER(st, brush);
    BIND_STRUCT_MEMBER(st, tree);
    BIND_STRUCT_MEMBER(st, meshLog);
    BIND_STRUCT_MEMBER(st, lastDynTopoStats);
    BIND_STRUCT_METHOD(st, beginStep, MARGS("hasDyntopo"));
    BIND_STRUCT_METHOD(st, endStep, MARGS());
    BIND_STRUCT_METHOD(
        st, execBrush, MARGS("mesh", "brushType", "nodes", "origin", "normal"));
    BIND_STRUCT_METHOD(st, execProgram, MARGS("prog", "nodes", "origin", "normal"));
    BIND_STRUCT_METHOD(st, applyDynTopoDab, MARGS("center", "radius", "params", "seed"));
    BIND_STRUCT_METHOD_SIG(
        st,
        applyDab,
        int,
        MARGS("prog", "center", "normal", "radius", "params", "seed"),
        (BrushProgram *, float3, float3, float, dyntopo::DynTopoParams *, uint32_t));
    // params==nullptr disables dyntopo for the dab; mark it nullable so the
    // generated TS accepts undefined (the _SIG macro isn't chainable).
    st->methods[st->methods.size() - 1]->argIsNullable("params");
    BIND_STRUCT_METHOD(st, endDynTopoStroke, MARGS());
    BIND_STRUCT_METHOD(st, clearIsFirstOfStep, MARGS());
    BIND_STRUCT_METHOD(st, beginPreviewDab, MARGS("center", "radius"));
    BIND_STRUCT_METHOD(st, extendPreviewDab, MARGS("center", "radius"));
    BIND_STRUCT_METHOD(st, rollbackPreviewDab, MARGS());
    BIND_STRUCT_METHOD(st, previewActive, MARGS());
    BIND_STRUCT_METHOD(st, commitPreviewDab, MARGS());
    BIND_STRUCT_METHOD(st, setNeighborMode, MARGS("mode"));
    BIND_STRUCT_METHOD(st, setNonAccum, MARGS("nonAccum"));
    BIND_STRUCT_METHOD(st, setAnchoredGrab, MARGS("anchored"));
    BIND_STRUCT_METHOD(st, setGrabAccumAdd, MARGS("add"));
    BIND_STRUCT_METHOD(st, setStrokeGen, MARGS("gen"));
    BIND_STRUCT_METHOD(st,
                       setPlaneFrame,
                       MARGS("normalMode",
                             "centerMode",
                             "originalNormal",
                             "originalPlane",
                             "normalRadiusFactor",
                             "areaRadiusFactor",
                             "stabilizeNormal",
                             "stabilizePlane",
                             "viewX",
                             "viewY",
                             "viewZ"));
    BIND_STRUCT_METHOD(st, setImageSign, MARGS("sx", "sy", "sz", "isMirror"));
    BIND_STRUCT_METHOD(st, lastUniformValidationOk, MARGS());
    BIND_STRUCT_METHOD(st, preflightRaw, MARGS("type"));
    BIND_STRUCT_METHOD(st, preflightRawProgram, MARGS("program"));
    BIND_STRUCT_METHOD(st, supportsResolved, MARGS("type"));
    BIND_STRUCT_METHOD(st, supportsResolvedProgram, MARGS("program"));
    BIND_STRUCT_METHOD(st, queryUniformManifest, MARGS("brushType"));
    BIND_STRUCT_METHOD(st, uniformQueryToken, MARGS());
    BIND_STRUCT_METHOD(st, uniformSnapshotChecked, MARGS("token", "uniformIndex"));
    BIND_STRUCT_METHOD(st,
                       readUniformScalarChecked,
                       MARGS("token", "uniformIndex", "scalarType", "evaluate"));
    BIND_STRUCT_METHOD(st,
                       writeUniformScalarChecked,
                       MARGS("token", "uniformIndex", "scalarType", "value"));
    BIND_STRUCT_METHOD(
        st,
        configureUniformDynamicChecked,
        MARGS("token", "uniformIndex", "scalarType", "device", "mode", "factor"));
    BIND_STRUCT_METHOD(st,
                       enableUniformDynamicChecked,
                       MARGS("token", "uniformIndex", "scalarType", "device", "enabled"));
    BIND_STRUCT_METHOD(st,
                       moveUniformDynamicChecked,
                       MARGS("token", "uniformIndex", "scalarType", "device", "index"));
    BIND_STRUCT_METHOD(
        st, clearUniformDynamicsChecked, MARGS("token", "uniformIndex", "scalarType"));
    BIND_STRUCT_METHOD(st,
                       replaceUniformDynamicTableChecked,
                       MARGS("token", "uniformIndex", "scalarType", "device", "samples"));
    BIND_STRUCT_METHOD(
        st,
        setUniformDynamicSampleChecked,
        MARGS(
            "token", "uniformIndex", "scalarType", "device", "index", "count", "value"));
    BIND_STRUCT_METHOD(st,
                       replaceUniformDynamicsChecked,
                       MARGS("token",
                             "uniformIndex",
                             "scalarType",
                             "devices",
                             "modes",
                             "factors",
                             "enabled",
                             "offsets",
                             "samples"));

    BIND_STRUCT_METHOD(st,
                       replaceUniformResponseDynamicsChecked,
                       MARGS("token",
                             "uniformIndex",
                             "scalarType",
                             "devices",
                             "modes",
                             "factors",
                             "enabled",
                             "offsets",
                             "samples",
                             "kinds",
                             "parameters"));
    BIND_STRUCT_METHOD(st, queriedUniformEntry, MARGS("idx"));
    BIND_STRUCT_METHOD(st, filterRadiusFloor, MARGS("brushType"));
    BIND_STRUCT_METHOD(st, clearUniformDynamics, MARGS("idx"));
    BIND_STRUCT_METHOD(
        st, addUniformDynamic, MARGS("idx", "deviceType", "mixMode", "mixFactor"));
    BIND_STRUCT_METHOD(
        st, setUniformDynamicSample, MARGS("idx", "deviceType", "i", "n", "value"));
    BIND_STRUCT_METHOD(st, setRenderMatrix, MARGS("m16"));

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
