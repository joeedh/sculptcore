#include "brush/brush_executor.h"
#include "brush/brush_preparation.h"
#include "test_util.h"
#include <limits>
#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

test_init;
using namespace sculptcore;
using namespace sculptcore::brush;
using math::BasicMix;
using props::DeviceType;
using props::Prop;
using props::PropError;
constexpr int pressure = int(DeviceType::PRESSURE);
constexpr int tilt = int(DeviceType::TILTX);
constexpr int multiply = int(BasicMix::MULTIPLY);
constexpr int add = int(BasicMix::ADD);

static void
declare(Brush &brush, const char *name, Prop type, double value, bool dynamic = true)
{
  props::ScalarDeclaration declaration{name, type, true, value, false, 0, 0, dynamic};
  test_assert(brush.props.struct_def->registerScalars({&declaration, 1}).error ==
              PropError::ERROR_NONE);
}

static void typed(Prop type)
{
  Brush brush;
  const int t = int(type);
  double value = type == Prop::BOOL ? 1 : type == Prop::INT32 ? 16777217 : 4;
  declare(brush, "value", type, value);
  test_assert(brush.readScalarChecked("value", t, false).value == value);
  test_assert(brush.writeScalarChecked("value", t, value) == 0);
  auto generation = brush.configurationGeneration();
  test_assert(brush.writeScalarChecked(
                  "value", t, std::numeric_limits<double>::quiet_NaN()) != 0);
  test_assert(brush.writeScalarChecked("value", int(Prop::STRING), 0) != 0);
  if (type != Prop::FLOAT32) {
    test_assert(brush.writeScalarChecked("value", t, 0.5) != 0);
  }
  test_assert(brush.configurationGeneration() == generation);
  test_assert(brush.readScalarChecked("value", t, false).value == value);

  test_assert(brush.configureDynamicChecked("value", t, pressure, multiply, 1) == 0);
  test_assert(brush.configureDynamicChecked("value", t, tilt, add, 1) == 0);
  auto *dynamics = brush.propDynamics("value");
  test_assert(dynamics && dynamics->devices.size() == 2);
  brush.pushDeviceInput(pressure, 0);
  brush.pushDeviceInput(tilt, 1);
  auto evaluated = brush.readScalarChecked("value", t, true);
  test_assert(evaluated.status == 0 && evaluated.value == 1);
  test_assert(brush.moveDynamicChecked("value", t, tilt, 0) == 0);
  evaluated = brush.readScalarChecked("value", t, true);
  test_assert(evaluated.status == 0 && evaluated.value == 0);
  test_assert(brush.readScalarChecked("value", t, false).value == value);
  test_assert(brush.enableDynamicChecked("value", t, pressure, 0) == 0);
  test_assert(brush.replaceDynamicTableChecked("value", t, pressure, {0.25f, 0.75f}) ==
              0);
  brush.addPropDynamicByName("value", pressure, add, 0.5f);
  test_assert(dynamics->devices.size() == 2 &&
              dynamics->devices[1].type == DeviceType::PRESSURE);
  test_assert(dynamics->devices[1].curveTable[0] == 0.25f &&
              (int(dynamics->devices[1].flag) & int(props::DynamicFlags::DISABLED)));
  generation = brush.configurationGeneration();
  props::Dynamics before = *dynamics;
  test_assert(brush.configureDynamicChecked("value", t, 4, add, 1) != 0);
  test_assert(brush.configureDynamicChecked("value", t, pressure, 999, 1) != 0);
  test_assert(brush.configureDynamicChecked("value", t, pressure, add, -1) != 0);
  test_assert(brush.enableDynamicChecked("value", t, pressure, 2) != 0);
  test_assert(brush.moveDynamicChecked("value", t, pressure, 2) != 0);
  test_assert(brush.replaceDynamicTableChecked("value", t, pressure, {0.0f}) != 0);
  test_assert(brush.configurationGeneration() == generation);
  for (int i = 0; i < 2; i++) {
    test_assert(dynamics->devices[i].sameConfiguration(before.devices[i]));
  }

  brush.props.struct_def->lookupLocal("value")->flag = props::PropFlag::READ_ONLY;
  test_assert(brush.writeScalarChecked("value", t, 0) == int(PropError::ERROR_READ_ONLY));
  test_assert(brush.clearDynamicsChecked("value", t) == int(PropError::ERROR_READ_ONLY));
  test_assert(brush.readScalarChecked("value", t, false).status == 0);
  test_assert(brush.configurationGeneration() == generation);
  brush.props.struct_def->lookupLocal("value")->flag = props::PropFlag::NONE;

  // Full replacement repairs corrupt state; malformed input preserves it exactly.
  dynamics->devices.append(dynamics->devices[0]);
  test_assert(brush.configureDynamicChecked("value", t, pressure, add, 1) != 0);
  test_assert(
      brush.replaceDynamicsChecked(
          "value", t, {pressure, pressure}, {add, add}, {1, 1}, {1, 1}, {0, 0, 0}, {}) !=
      0);
  test_assert(dynamics->devices.size() == 3 &&
              brush.configurationGeneration() == generation);
  test_assert(brush.replaceDynamicsChecked("value",
                                           t,
                                           {pressure, tilt},
                                           {multiply, add},
                                           {1, 0.5f},
                                           {1, 0},
                                           {0, 2, 2},
                                           {0, 1}) == 0);
  test_assert(dynamics->devices.size() == 2);
  generation = brush.configurationGeneration();
  for (const auto &offsets : {Vector<int>{1, 2, 2},
                              Vector<int>{0, -1, 2},
                              Vector<int>{0, 3, 2},
                              Vector<int>{0, 2},
                              Vector<int>{0, 1, 2}})
  {
    test_assert(brush.replaceDynamicsChecked("value",
                                             t,
                                             {pressure, tilt},
                                             {multiply, add},
                                             {1, 1},
                                             {1, 1},
                                             offsets,
                                             {0, 1}) != 0);
  }
  test_assert(brush.replaceDynamicsChecked("value",
                                           t,
                                           {pressure},
                                           {multiply},
                                           {1},
                                           {1},
                                           {0, 2},
                                           {0, std::numeric_limits<float>::infinity()}) !=
              0);
  test_assert(brush.configurationGeneration() == generation);
  dynamics->devices[0].flag = props::DynamicFlags(1024);
  test_assert(brush.clearDynamicsChecked("value", t) == 0);
  test_assert(dynamics->devices.size() == 0);
}

static void pending()
{
  Brush brush;
  declare(brush, "value", Prop::INT32, 10);
  const int t = int(Prop::INT32);
  test_assert(brush.configureDynamicChecked("value", t, pressure, multiply, 1) == 0);
  test_assert(brush.replaceDynamicTableChecked("value", t, pressure, {0, 1}) == 0);
  auto *dynamics = brush.propDynamics("value");
  auto generation = brush.configurationGeneration();
  test_assert(brush.setDynamicSampleChecked("value", t, pressure, 2, 3, 1) == 0);
  test_assert(brush.configurationGeneration() == generation + 1);
  test_assert(dynamics->devices[0].hasPendingCurve() &&
              dynamics->devices[0].curveTable.size() == 2);
  test_assert(brush.readScalarChecked("value", t, true).status ==
              int(PropError::ERROR_INVALID_DYNAMICS));
  test_assert(brush.enableDynamicChecked("value", t, pressure, 0) == 0);
  test_assert(brush.configureDynamicChecked("value", t, pressure, add, 0.5f) == 0);
  test_assert(brush.readScalarChecked("value", t, true).status ==
              int(PropError::ERROR_INVALID_DYNAMICS));
  test_assert(brush.setDynamicSampleChecked("value", t, pressure, 2, 3, 0.75f) == 0);
  generation = brush.configurationGeneration();
  props::Dynamics copy = *dynamics;
  test_assert(!copy.devices[0].hasDeviceValue && copy.devices[0].hasPendingCurve());
  test_assert(brush.setDynamicSampleChecked("value", t, pressure, 0, 65537, 0) != 0);
  test_assert(brush.setDynamicSampleChecked("value", t, pressure, -1, 3, 0) != 0);
  test_assert(brush.setDynamicSampleChecked(
                  "value", t, pressure, 0, 3, std::numeric_limits<float>::quiet_NaN()) !=
              0);
  test_assert(brush.configurationGeneration() == generation);
  test_assert(copy.devices[0].sameConfiguration(dynamics->devices[0]));
  test_assert(brush.setDynamicSampleChecked("value", t, pressure, 0, 3, 0.25f) == 0);
  test_assert(brush.setDynamicSampleChecked("value", t, pressure, 1, 3, 0.5f) == 0);
  test_assert(!dynamics->devices[0].hasPendingCurve() &&
              copy.devices[0].hasPendingCurve());
  test_assert(dynamics->devices[0].curveTable.size() == 3 &&
              dynamics->devices[0].curveTable[2] == 0.75f);
  test_assert(brush.setDynamicSampleChecked("value", t, pressure, 1, 3, 0.9f) == 0);
  test_assert(!dynamics->devices[0].hasPendingCurve() &&
              dynamics->devices[0].curveTable[1] == 0.9f);
  test_assert(brush.setDynamicSampleChecked("value", t, pressure, 0, 4, 1) == 0);
  test_assert(brush.setDynamicSampleChecked("value", t, pressure, 1, 2, 0.5f) == 0);
  test_assert(brush.setDynamicSampleChecked("value", t, pressure, 0, 2, 1) == 0);
  test_assert(!dynamics->devices[0].hasPendingCurve() &&
              dynamics->devices[0].curveTable.size() == 2);
  test_assert(brush.setDynamicSampleChecked("value", t, pressure, 0, 4, 1) == 0);
  test_assert(brush.replaceDynamicTableChecked("value", t, pressure, {}) == 0);
  test_assert(!dynamics->devices[0].hasPendingCurve() &&
              dynamics->devices[0].curveTable.size() == 0);
  Vector<float> limit;
  limit.resize(props::kDeviceCurveSampleLimit);
  for (auto &v : limit) {
    v = 1;
  }
  test_assert(brush.replaceDynamicTableChecked("value", t, pressure, limit) == 0);
  generation = brush.configurationGeneration();
  limit.append(1);
  test_assert(brush.replaceDynamicTableChecked("value", t, pressure, limit) != 0);
  test_assert(brush.configurationGeneration() == generation);
}

static void targets()
{
  Brush brush, child;
  declare(brush, "value", Prop::INT32, 42);
  declare(brush, "static", Prop::BOOL, 1, false);
  brush.props.struct_def->Int32("undeclared", "Undeclared", -1);
  brush.props.struct_def->Float64("legacy", "Legacy", -1);
  child.props.struct_def->parent = brush.props.struct_def;
  test_assert(
      child.configureDynamicChecked("value", int(Prop::INT32), pressure, add, 1) ==
      int(PropError::ERROR_INVALID_OWNER));
  test_assert(child.writeScalarChecked("value", int(Prop::INT32), 0) ==
              int(PropError::ERROR_INVALID_OWNER));
  test_assert(child.readScalarChecked("value", int(Prop::INT32), false).status ==
              int(PropError::ERROR_INVALID_OWNER));
  test_assert(brush.configureDynamicChecked("value", int(Prop::BOOL), pressure, add, 1) ==
              int(PropError::ERROR_INVALID_TYPE));
  test_assert(
      brush.configureDynamicChecked("static", int(Prop::BOOL), pressure, add, 1) ==
      int(PropError::ERROR_INVALID_DYNAMICS));
  test_assert(
      brush.configureDynamicChecked("undeclared", int(Prop::INT32), pressure, add, 1) ==
      int(PropError::ERROR_INVALID_DYNAMICS));
  brush.addPropDynamicByName("legacy", pressure, add, 1);
  test_assert(brush.propDynamics("legacy")->devices.size() == 0);
  auto *property =
      static_cast<props::Int32Prop *>(static_cast<props::detail::PropBaseType *>(
          brush.props.struct_def->lookupLocal("value")));
  property->max = 5;
  test_assert(
      brush.configureDynamicChecked("value", int(Prop::INT32), pressure, add, 1) ==
      int(PropError::ERROR_SCHEMA_CONFLICT));
  child.props.struct_def->parent = nullptr;
  brush.invert = true;
  brush.writeProps();
  brush.addPropDynamic(int(BrushProp::Invert), pressure, multiply, 1);
  brush.pushDeviceInput(pressure, 0);
  brush.loadProps();
  test_assert(!brush.invert && brush.props.lookupValue<bool>("invert", false));
}

static void queries()
{
  Brush brush, other;
  CommandExecutor executor(nullptr, &brush), second(nullptr, &brush);
  const int type = int(Prop::FLOAT32);
  test_assert(executor.queryUniformManifest(int(SculptBrushes::KELVINLET)) > 0);
  int token = executor.uniformQueryToken();
  auto snapshot = executor.uniformSnapshotChecked(token, 0);
  test_assert(snapshot.status == 0 && snapshot.name == string("mu"));
  // The legacy borrowed descriptor is writable; it must not route checked calls.
  executor.queriedUniformEntry(0)->name = "radius";
  executor.queriedUniformEntry(0)->scalarType = Prop::BOOL;
  test_assert(executor.writeUniformScalarChecked(token, 0, type, 2) == 0);
  auto result = executor.readUniformScalarChecked(token, 0, type, false);
  test_assert(result.status == 0 && result.value == 2);
  test_assert(brush.props.lookupValue<float>("mu", -1) == 2);
  executor.addUniformDynamic(0, pressure, add, 1);
  test_assert(brush.propDynamics("mu")->devices.size() == 1);
  test_assert(brush.propDynamics("radius")->devices.size() == 0);
  test_assert(executor.writeUniformScalarChecked(token, 0, int(Prop::BOOL), 1) ==
              int(PropError::ERROR_INVALID_TYPE));
  test_assert(second.queryUniformManifest(int(SculptBrushes::KELVINLET)) > 0);
  test_assert(second.uniformQueryToken() != token);
  test_assert(second.writeUniformScalarChecked(token, 0, type, 4) ==
              int(PropError::ERROR_STALE_QUERY));
  executor.brush = &other;
  test_assert(executor.writeUniformScalarChecked(token, 0, type, 4) ==
              int(PropError::ERROR_STALE_QUERY));
  executor.brush = &brush;
  auto *original = brush.props.struct_def;
  brush.props.struct_def = other.props.struct_def;
  test_assert(executor.readUniformScalarChecked(token, 0, type, false).status ==
              int(PropError::ERROR_STALE_QUERY));
  brush.props.struct_def = original;
  test_assert(executor.queryUniformManifest(int(SculptBrushes::KELVINLET)) > 0);
  test_assert(executor.uniformQueryToken() != token);
  test_assert(executor.clearUniformDynamicsChecked(token, 0, type) ==
              int(PropError::ERROR_STALE_QUERY));
  token = executor.uniformQueryToken();
  test_assert(executor.queryUniformManifest(int(SculptBrushes::DRAW)) >= 0);
  test_assert(executor.uniformSnapshotChecked(token, 0).status ==
              int(PropError::ERROR_STALE_QUERY));
  token = executor.uniformQueryToken();
  test_assert(executor.queryUniformManifest(99999) == -1);
  test_assert(executor.uniformQueryToken() != token);
  test_assert(executor.uniformSnapshotChecked(executor.uniformQueryToken(), 0).status !=
              0);
  test_assert(snapshot.name == string("mu") && snapshot.def == 1);
  test_assert(result.status == 0 && result.value == 2);
  test_assert(
      brush.writeCommonScalarChecked(int(BrushProp::Invert), int(Prop::BOOL), 1) == 0);
  test_assert(
      brush.readCommonScalarChecked(int(BrushProp::Invert), int(Prop::BOOL), false)
          .value == 1);
}

static void staticStorage()
{
  Brush brush;
  declare(brush, "wingAngle", Prop::FLOAT32, 1, false);
  declare(brush, "activeGroup", Prop::INT32, 0, false);
  declare(brush, "invert", Prop::BOOL, 0, false);
  brush.wingAngle = 0.75f;
  brush.activeGroup = 2;
  test_assert(brush.readScalarChecked("wingAngle", int(Prop::FLOAT32), false).value ==
              0.75);
  test_assert(brush.readScalarChecked("activeGroup", int(Prop::INT32), false).value == 2);
  test_assert(brush.writeScalarChecked("wingAngle", int(Prop::FLOAT32), 0.5) == 0);
  test_assert(brush.writeScalarChecked("activeGroup", int(Prop::INT32), 16777217) == 0);
  test_assert(brush.wingAngle == 0.5f && brush.activeGroup == 16777217);
  test_assert(brush.props.lookupValue<float>("wingAngle", -1) == 1);
  test_assert(brush.props.lookupValue<int>("activeGroup", -1) == 0);
  auto generation = brush.configurationGeneration();
  test_assert(brush.writeScalarChecked("activeGroup", int(Prop::INT32), 1.5) != 0);
  test_assert(brush.writeScalarChecked("wingAngle", int(Prop::FLOAT32), INFINITY) != 0);
  test_assert(brush.activeGroup == 16777217 && brush.wingAngle == 0.5f);
  test_assert(brush.configurationGeneration() == generation);
  brush.props.struct_def->lookupLocal("wingAngle")->flag = props::PropFlag::READ_ONLY;
  test_assert(brush.writeScalarChecked("wingAngle", int(Prop::FLOAT32), 0.25) ==
              int(PropError::ERROR_READ_ONLY));
  brush.props.struct_def->lookupLocal("wingAngle")->flag = props::PropFlag::NONE;
  brush.wingAngle = NAN;
  test_assert(brush.readScalarChecked("wingAngle", int(Prop::FLOAT32), false).status ==
              int(PropError::ERROR_INVALID_VALUE));
  test_assert(brush.writeScalarChecked("wingAngle", int(Prop::FLOAT32), 0.25) == 0);
  // Static common values still use authored properties, as loadCommonProps does.
  test_assert(brush.writeScalarChecked("invert", int(Prop::BOOL), 1) == 0);
  brush.invert = false;
  test_assert(brush.readScalarChecked("invert", int(Prop::BOOL), false).value == 1);
  brush.loadProps();
  test_assert(brush.invert);
  test_assert(brush.configureDynamicChecked(
                  "invert", int(Prop::BOOL), pressure, multiply, 1) != 0);
}

static double preparedValue(const PreparedBrushScalars &prepared, const char *name)
{
  for (const auto &value : prepared.values()) {
    if (value.name == string(name))
      return value.value;
  }
  test_assert(false);
  return -999;
}

static void preparedCandidates()
{
  Brush brush;
  CommandExecutor::brush_command command;
  test_assert(
      CommandExecutor::createDeclarationCommand(SculptBrushes::KELVINLET, command));
  auto manifest = [&] {
    return std::span<const BrushUniformManifestEntry>(command.uniforms.data(),
                                                      command.uniforms.size());
  };
  PreparedBrushScalars prepared;
  auto run = [&] {
    return prepareBrushScalars(brush, manifest(), brush.deviceInputCtx, prepared);
  };
  auto *strength =
      static_cast<props::Float32Prop *>(static_cast<props::detail::PropBaseType *>(
          brush.props.struct_def->lookupLocal("strength")));
  int owner = 31, getterCalls = 0;
  strength->owner = &owner;
  brush.radius = 7;
  brush.mu = 19;
  auto generation = brush.configurationGeneration();
  test_assert(run().error == PropError::ERROR_NONE);
  test_assert(prepared.sourceBrush() == &brush &&
              prepared.sourceDefinition() == brush.props.struct_def);
  test_assert(preparedValue(prepared, "mu") == 1 &&
              preparedValue(prepared, "radius") == 0);
  test_assert(brush.radius == 7 && brush.mu == 19 && strength->owner == &owner);
  test_assert(!brush.props.struct_def->lookupLocal("mu"));
  props::ScalarDeclaration retained;
  test_assert(!brush.props.struct_def->scalarDeclaration("mu", retained));
  test_assert(brush.configurationGeneration() == generation);

  strength->Default(16777216).BindingOffset(-1);
  strength->flag = props::PropFlag::READ_ONLY;
  strength->dynamics.configure(DeviceType::PRESSURE, BasicMix::ADD, 1);
  strength->dynamics.configure(DeviceType::TILTX, BasicMix::ADD, 1);
  brush.pushDeviceInput(pressure, 1);
  brush.pushDeviceInput(tilt, 1);
  strength->dynamics.devices[0].curDeviceValue = 0.7f;
  strength->dynamics.devices[0].hasDeviceValue = false;
  test_assert(run().error == PropError::ERROR_NONE);
  test_assert(preparedValue(prepared, "strength") ==
              16777216); // float rounding at each mix
  test_assert(strength->owner == &owner && *strength->internal_value() == 16777216);
  test_assert(strength->dynamics.devices[0].curDeviceValue == 0.7f &&
              !strength->dynamics.devices[0].hasDeviceValue);
  const auto *oldValues = prepared.values().data();
  auto fail = [&](PropError expected) {
    const bool hadDeclaration = brush.props.struct_def->scalarDeclaration("mu", retained);
    test_assert(run().error == expected);
    test_assert(prepared.values().data() == oldValues &&
                preparedValue(prepared, "mu") == 1);
    test_assert(!brush.props.struct_def->lookupLocal("mu") &&
                brush.props.struct_def->scalarDeclaration("mu", retained) ==
                    hadDeclaration);
    test_assert(brush.mu == 19 && brush.radius == 7 && strength->owner == &owner);
    test_assert(brush.configurationGeneration() == generation);
  };
  strength->getter = [&](float *value, void *) {
    getterCalls++;
    return value;
  };
  fail(PropError::ERROR_INVALID_OWNER);
  test_assert(getterCalls == 0);
  strength->getter = {};
  strength->Name("renamed_strength");
  fail(PropError::ERROR_SCHEMA_CONFLICT);
  strength->Name("strength");
  brush.props.owner = &owner;
  strength->BindingOffset(0);
  fail(PropError::ERROR_INVALID_OWNER);
  strength->BindingOffset(-1);
  for (int id = 0; id <= int(BrushProp::Invert); id++) {
    brush.props.struct_def->lookupLocal(brushPropName(id))->binding_offset = -1;
  }
  test_assert(run().error == PropError::ERROR_NONE); // an owner alone is allowed
  oldValues = prepared.values().data();
  brush.props.owner = nullptr;
  strength->dynamics.devices[0].curveTable.append(0.5f);
  strength->dynamics.devices[0].flag = props::DynamicFlags::DISABLED;
  fail(PropError::ERROR_INVALID_DYNAMICS);
  strength->dynamics.devices[0].curveTable.clear();
  strength->dynamics.devices[0].flag = props::DynamicFlags::NONE;
  auto &stray = brush.props.struct_def->Float64("stray", "Stray", -1);
  stray.dynamics.configure(DeviceType::PRESSURE, BasicMix::MULTIPLY, 1);
  stray.dynamics.devices[0].flag = props::DynamicFlags::DISABLED;
  fail(PropError::ERROR_INVALID_DYNAMICS);
  stray.dynamics.devices.clear();
  auto &vector = brush.props.struct_def->Vec3f("vector", "Vector", -1);
  vector.dynamics.configure(DeviceType::PRESSURE, BasicMix::MULTIPLY, 1);
  fail(PropError::ERROR_INVALID_DYNAMICS);
  vector.dynamics.devices.clear();
  auto &renamed = brush.props.struct_def->Float32("renamed", "Renamed", -1);
  renamed.Name("strength");
  renamed.dynamics.configure(DeviceType::PRESSURE, BasicMix::MULTIPLY, 1);
  fail(PropError::ERROR_INVALID_DYNAMICS);
  renamed.dynamics.devices.clear();
  auto duplicate = command.uniforms[0];
  command.uniforms.append(duplicate);
  fail(PropError::ERROR_SCHEMA_CONFLICT);
  command.uniforms.pop_back();
  command.uniforms[0].storeSlot = 0;
  fail(PropError::ERROR_SCHEMA_CONFLICT);
  command.uniforms[0].storeSlot = -1;
  command.uniforms[0].hasDefault = false;
  fail(PropError::ERROR_INVALID_VALUE); // absent mu cannot publish zero outside its range
  command.uniforms[0].hasDefault = true;
  strength->Default(std::numeric_limits<float>::infinity());
  fail(PropError::ERROR_INVALID_VALUE);
  strength->Default(16777216);
  props::StructDef parent;
  test_assert(parent.registerScalars(prepared.declarations()).error ==
              PropError::ERROR_NONE);
  brush.props.struct_def->parent = &parent;
  fail(PropError::ERROR_INVALID_OWNER);
  brush.props.struct_def->parent = nullptr;
  auto *definition = brush.props.struct_def;
  props::StructDef empty;
  brush.props.struct_def = &empty;
  test_assert(run().error == PropError::ERROR_NOT_EXISTS &&
              prepared.values().data() == oldValues);
  brush.props.struct_def = definition;

  strength->dynamics.devices.clear();
  command = CommandExecutor::brush_command{};
  test_assert(
      CommandExecutor::createDeclarationCommand(SculptBrushes::WINGSCRAPE, command));
  test_assert(command.registerProps(*definition).error == PropError::ERROR_NONE);
  auto *wing = static_cast<props::Float32Prop *>(
      static_cast<props::detail::PropBaseType *>(definition->lookupLocal("wingAngle")));
  wing->Default(std::numeric_limits<float>::quiet_NaN());
  wing->getter = [&](float *value, void *) {
    getterCalls++;
    return value;
  };
  wing->owner = &owner;
  brush.wingAngle = 0.75f;
  test_assert(run().error == PropError::ERROR_NONE &&
              preparedValue(prepared, "wingAngle") == 0.75);
  test_assert(getterCalls == 0 && wing->owner == &owner &&
              std::isnan(*wing->internal_value()));
  oldValues = prepared.values().data();
  wing->dynamics.configure(DeviceType::PRESSURE, BasicMix::MULTIPLY, 1);
  test_assert(run().error == PropError::ERROR_INVALID_DYNAMICS &&
              prepared.values().data() == oldValues);
  wing->dynamics.devices.clear();
  brush.wingAngle = std::numeric_limits<float>::infinity();
  test_assert(run().error == PropError::ERROR_INVALID_VALUE &&
              prepared.values().data() == oldValues);
  Brush native;
  native.activeGroup = 16777217;
  CommandExecutor::brush_command groups, color;
  test_assert(
      CommandExecutor::createDeclarationCommand(SculptBrushes::POLYGROUP, groups));
  test_assert(prepareBrushScalars(native,
                                  {groups.uniforms.data(), groups.uniforms.size()},
                                  native.deviceInputCtx,
                                  prepared)
                  .error == PropError::ERROR_NONE);
  test_assert(preparedValue(prepared, "activeGroup") == 16777217 &&
              !native.props.struct_def->lookupLocal("activeGroup"));
  test_assert(CommandExecutor::createDeclarationCommand(SculptBrushes::COLOR, color));
  test_assert(prepareBrushScalars(native,
                                  {color.uniforms.data(), color.uniforms.size()},
                                  native.deviceInputCtx,
                                  prepared)
                  .error == PropError::ERROR_NONE);
  for (const auto &value : prepared.values()) {
    test_assert(value.name != string("brushColor")); // known nonscalar uniform stays raw
  }
}

static void analyticResponses(Prop type)
{
  Brush brush;
  int t = int(type);
  declare(brush, "value", type, 1);
  const double low = type == Prop::BOOL ? 0 : 3;
  const double high = type == Prop::INT32 ? 16777217 : 1;
  auto install = [&](double threshold) {
    return brush.replaceResponseDynamicsChecked("value",
                                                t,
                                                {pressure},
                                                {int(BasicMix::LINEAR)},
                                                {1},
                                                {1},
                                                {0, 0},
                                                {},
                                                {2},
                                                {threshold, low, high});
  };
  for (double threshold : {0.0, 0.5, 1.0, std::nextafter(0.5, 1.0)}) {
    test_assert(install(threshold) == 0);
    for (float input : {std::nextafter(float(threshold), -INFINITY),
                        float(threshold),
                        std::nextafter(float(threshold), INFINITY)})
    {
      brush.pushDeviceInput(pressure, input);
      auto result = brush.readScalarChecked("value", t, true);
      test_assert(result.status == 0);
      test_assert(result.value ==
                  (std::clamp(double(input), 0.0, 1.0) < threshold ? low : high));
    }
  }
  auto *dynamics = brush.propDynamics("value");
  props::Dynamics copy = *dynamics;
  test_assert(copy.devices[0].sameConfiguration(dynamics->devices[0]));
  auto generation = brush.configurationGeneration();
  for (auto params : {Vector<double>{NAN, 0, 1},
                      Vector<double>{-1, 0, 1},
                      Vector<double>{0.5, INFINITY, 1},
                      Vector<double>{0.5, 0}})
  {
    test_assert(brush.replaceResponseDynamicsChecked(
                    "value", t, {pressure}, {0}, {1}, {1}, {0, 0}, {}, {2}, params) != 0);
    test_assert(brush.configurationGeneration() == generation);
    test_assert(copy.devices[0].sameConfiguration(dynamics->devices[0]));
  }
  test_assert(
      brush.replaceResponseDynamicsChecked(
          "value", t, {pressure}, {0}, {1}, {1}, {0, 2}, {0, 1}, {2}, {0.5, 0, 1}) != 0);
  test_assert(brush.replaceResponseDynamicsChecked(
                  "value", t, {pressure}, {0}, {1}, {1}, {0, 0}, {}, {3}, {0, 0, 0}) !=
              0);
  test_assert(brush.configurationGeneration() == generation);
  test_assert(brush.enableDynamicChecked("value", t, pressure, 0) == 0);
  test_assert(brush.readScalarChecked("value", t, true).value == 1);
  brush.clearDeviceInputs();
  test_assert(brush.enableDynamicChecked("value", t, pressure, 1) == 0);
  test_assert(brush.readScalarChecked("value", t, true).value == 1);
  test_assert(brush.setDynamicSampleChecked("value", t, pressure, 0, 2, 0) == 0);
  test_assert(dynamics->devices[0].responseKind == 2);
  test_assert(brush.readScalarChecked("value", t, true).status != 0);
  test_assert(brush.setDynamicSampleChecked("value", t, pressure, 1, 2, 1) == 0);
  test_assert(dynamics->devices[0].responseKind == 0 &&
              !dynamics->devices[0].hasPendingCurve());
  test_assert(install(0.5) == 0);
  test_assert(brush.replaceDynamicTableChecked("value", t, pressure, {0, 1}) == 0);
  test_assert(dynamics->devices[0].responseKind == 0);
  test_assert(
      brush.replaceResponseDynamicsChecked(
          "value", t, {pressure}, {0}, {1}, {1}, {0, 0}, {}, {1}, {0, 1e100, 0}) == 0);
  brush.pushDeviceInput(pressure, 0.5);
  auto result = brush.readScalarChecked("value", t, true);
  test_assert(result.status == 0);
  test_assert(result.value == (type == Prop::INT32 ? 2147483647.0 : 1.0));
}

static void fixedTables()
{
  Brush tables;
  Vector<float> samples;
  samples.resize(256);
  for (int i = 0; i < 256; i++) {
    samples[i] = float(i) / 255;
  }
  test_assert(tables.replaceFalloffCurveChecked(samples));
  test_assert(tables.replaceCavityCurveChecked(samples));
  auto falloff = tables.falloff_curve, cavity = tables.cavity_curve;
  samples[255] = NAN;
  test_assert(!tables.replaceFalloffCurveChecked(samples));
  test_assert(!tables.replaceCavityCurveChecked(samples));
  Vector<float> shortTable{0, 1};
  test_assert(!tables.replaceFalloffCurveChecked(shortTable));
  test_assert(!tables.replaceCavityCurveChecked(shortTable));
  test_assert(tables.falloff_curve == falloff && tables.cavity_curve == cavity);
}

static void hostFloatModes()
{
#if defined(__SSE__) || defined(_M_X64)
  const unsigned saved = _mm_getcsr();
  for (unsigned mode : {0u, 0x40u, 0x8000u, 0x8040u}) {
    _mm_setcsr((saved & ~0x8040u) | mode);
    volatile double tiny = 0x1p-127;
    volatile uint32_t bits = 0x00400000u;
    const float represented = std::bit_cast<float>(uint32_t(bits));
    float converted = 0;
    test_assert(named_uniform_detail::convert(double(tiny), converted) == (mode == 0));
    test_assert(named_uniform_detail::supportedFloat(represented) == (mode == 0));
    Brush brush;
    brush.radius = 1;
    brush.writeProps();
    const auto generation = brush.configurationGeneration();
    const auto status = brush.writeCommonScalarChecked(1, int(Prop::FLOAT32), tiny);
    test_assert((status == 0) == (mode == 0));
    if (mode)
      test_assert(brush.configurationGeneration() == generation &&
                  brush.readCommonScalarChecked(1, int(Prop::FLOAT32), false).value == 1);
    BrushProgram program;
    program.addCommand(int(SculptBrushes::KELVINLET));
    program.setCommandScalarChecked(0, "radius", Prop::FLOAT32, 1);
    test_assert((program.setCommandScalarChecked(0, "radius", Prop::FLOAT32, tiny) ==
                 PropError::ERROR_NONE) == (mode == 0));
    if (mode)
      test_assert(program.commands[0].scalarOverrides[0].value == 1);
    brush.radius = 1;
    brush.writeProps();
    brush.unboundedExtent = represented;
    PreparedBrushScalars stage;
    test_assert(prepareBrushScalars(brush, {}, brush.deviceInputCtx, stage).error ==
                PropError::ERROR_NONE);
    const auto support = validateUnboundedSupport(stage, 1);
    test_assert((support.error == PropError::ERROR_NONE) == (mode == 0));
    const BrushUniformManifestEntry manifest{
        "unboundedExtent", true, true, tiny, false, 0, 0, -1, Prop::FLOAT32, true};
    PreparedBrushScalars initial;
    test_assert((prepareBrushScalars(brush, {&manifest, 1}, brush.deviceInputCtx, initial)
                     .error == PropError::ERROR_NONE) == (mode == 0));
    // Intact raw authored bits must fail before DAZ can reinterpret them as zero.
    auto *base = brush.props.struct_def->lookupLocal("radius");
    auto *radius = static_cast<props::Float32Prop *>(
        static_cast<props::detail::PropBaseType *>(base));
    *radius->internal_value() = represented;
    test_assert((prepareBrushScalars(brush, {}, brush.deviceInputCtx, stage).error ==
                 PropError::ERROR_NONE) == (mode == 0));
    test_assert((_mm_getcsr() & 0x8040u) == mode);
  }
  _mm_setcsr(saved);
  fprintf(stderr,
          "checked scalar IEEE and FTZ/DAZ transport, atomicity and preflight passed\n");
#endif
}

int main()
{
  hostFloatModes();
  fixedTables();
  preparedCandidates();
  staticStorage();
  for (Prop type : {Prop::FLOAT32, Prop::INT32, Prop::BOOL}) {
    typed(type);
    analyticResponses(type);
  }
  pending();
  targets();
  queries();
  return test_end();
}
