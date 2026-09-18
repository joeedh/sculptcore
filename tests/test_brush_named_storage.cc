#include "brush/brush_executor.h"
#include "brush/compiler/emit_registry.h"
#include "test_util.h"

#include <cstring>
#include <limits>

namespace sculptcore::brush {
inline constexpr int kExtraSlot_storage_gain = 20;
inline constexpr int kExtraSlot_storage_work = 21;
} // namespace sculptcore::brush
#include "named_storage.brush.gen.h"

test_init;
using namespace sculptcore;
using namespace sculptcore::brush;
using props::Prop;
using props::PropError;

static void storageValues()
{
  Brush brush;
  brush.setNamedFloat(8, 7.0f);
  brush.ensureNamedFloatDefault(0, 0.25f);
  brush.ensureNamedFloatDefault(8, 3.0f);
  test_assert(brush.getNamedFloat(0) == 0.25f);
  test_assert(brush.getNamedFloat(8) == 7.0f);
  test_assert(!brush.namedFloats.initialized(1));
  brush.ensureNamedFloatDefault(1, 4.0f);
  test_assert(brush.getNamedFloat(1) == 4.0f);

  brush.setNamedInt(8, 16777217);
  brush.ensureNamedIntDefault(0, INT32_MAX);
  brush.ensureNamedIntDefault(1, INT32_MIN);
  brush.ensureNamedIntDefault(8, 3);
  test_assert(brush.getNamedInt(8) == 16777217);
  test_assert(brush.getNamedInt(0) == INT32_MAX);
  test_assert(brush.getNamedInt(1) == INT32_MIN);

  brush.setNamedBool(8, false);
  brush.ensureNamedBoolDefault(0, true);
  brush.ensureNamedBoolDefault(8, true);
  test_assert(brush.getNamedBool(0));
  test_assert(!brush.getNamedBool(8));
  test_assert(brush.namedBools.initialized(8));
  test_assert(!brush.namedBools.initialized(1));

  for (Prop type : {Prop::FLOAT32, Prop::INT32, Prop::BOOL}) {
    test_assert(brush.setNamedScalar(type, -1, 1) == PropError::ERROR_INVALID_VALUE);
    test_assert(brush.setNamedScalar(type, kNamedUniformSlotLimit, 1) ==
                PropError::ERROR_INVALID_VALUE);
    test_assert(brush.setNamedScalar(type, 30, std::numeric_limits<double>::infinity()) ==
                PropError::ERROR_INVALID_VALUE);
    test_assert(
        brush.setNamedScalar(type, 30, std::numeric_limits<double>::quiet_NaN()) ==
        PropError::ERROR_INVALID_VALUE);
  }
  test_assert(brush.setNamedScalar(Prop::INT32, 30, 2147483648.0) ==
              PropError::ERROR_INVALID_VALUE);
  test_assert(brush.setNamedScalar(Prop::INT32, 30, 1.5) ==
              PropError::ERROR_INVALID_VALUE);
  test_assert(brush.setNamedScalar(Prop::BOOL, 30, 0.5) ==
              PropError::ERROR_INVALID_VALUE);
  test_assert(brush.setNamedScalar(Prop::BOOL, 30, 2) == PropError::ERROR_INVALID_VALUE);
  test_assert(brush.setNamedScalar(Prop::FLOAT32, 30, 1e100) ==
              PropError::ERROR_INVALID_VALUE);
  test_assert(brush.setNamedScalar(Prop::FLOAT64, 30, 1) ==
              PropError::ERROR_INVALID_TYPE);
  test_assert(brush.namedFloats.size() == 9 && brush.namedInts.size() == 9 &&
              brush.namedBools.size() == 9);
  test_assert(brush.getNamedFloat(-1) == 0 && brush.getNamedInt(65536) == 0 &&
              !brush.getNamedBool(99));
  test_assert(brush.setNamedScalar(Prop::FLOAT32, 2, 0.1) == PropError::ERROR_NONE);
  test_assert(brush.getNamedFloat(2) == 0.1f);

#ifdef SCULPTCORE_EXTRA_BRUSHES
  Brush defaults;
  defaults.setNamedFloat(100, 5.0f);
  ensureExtraUniformDefaults(defaults);
  for (int slot = 0; slot < extraNamedFloatCount; slot++) {
    test_assert(defaults.namedFloats.initialized(slot));
    test_assert(defaults.getNamedFloat(slot) == kExtraNamedFloatDefaults[slot]);
    auto *descriptor = extraNamedUniformDescriptor(Prop::FLOAT32, slot);
    test_assert(descriptor && descriptor->type == Prop::FLOAT32);
  }
  test_assert(defaults.getNamedFloat(100) == 5.0f);
#endif
}

template <typename T> static void authoredWrites(Prop type)
{
  Brush brush;
  NamedUniformStore<T> store;
  const double base = type == Prop::BOOL ? 0 : 2;
  const double authored = type == Prop::BOOL ? 1 : type == Prop::INT32 ? 16777217 : 3.5;
  props::ScalarDeclaration declaration{"authored", type, true, base, false, 0, 0, true};
  test_assert(brush.props.struct_def->registerScalars({&declaration, 1}).error ==
              PropError::ERROR_NONE);
  NamedUniformDescriptor descriptor{"authored", type, true};
  auto write = [&](double value) {
    return named_uniform_detail::StoreAccess::write(
        store, brush.props, &descriptor, type, 3, value);
  };
  test_assert(write(authored) == PropError::ERROR_NONE);
  test_assert(store.get(3) == T(authored));
  double read = -1;
  test_assert(brush.props.readScalar("authored", type, read) == PropError::ERROR_NONE);
  test_assert(read == authored);
  auto *property = brush.props.struct_def->lookupLocal("authored");
  property->flag = props::PropFlag::READ_ONLY;
  test_assert(write(base) == PropError::ERROR_READ_ONLY);
  test_assert(store.get(3) == T(authored));
  property->flag = props::PropFlag::NONE;
  test_assert(brush.props.readScalar("authored", type, read) == PropError::ERROR_NONE &&
              read == authored);

  using P = std::conditional_t<
      std::is_same_v<T, float>,
      props::Float32Prop,
      std::conditional_t<std::is_same_v<T, bool>, props::BoolProp, props::Int32Prop>>;
  auto *typed = static_cast<P *>(static_cast<props::detail::PropBaseType *>(property));
  const auto previousMax = typed->max;
  typed->max = 0;
  test_assert(write(0) == PropError::ERROR_SCHEMA_CONFLICT);
  test_assert(store.get(3) == T(authored));
  typed->max = previousMax;
  test_assert(brush.props.readScalar("authored", type, read) == PropError::ERROR_NONE &&
              read == authored);

  props::ScalarDeclaration limited{"limited", type, true, 0, true, 0, 0, true};
  test_assert(brush.props.struct_def->registerScalars({&limited, 1}).error ==
              PropError::ERROR_NONE);
  descriptor.name = "limited";
  test_assert(write(1) == PropError::ERROR_INVALID_VALUE);
  test_assert(store.get(3) == T(authored));
  test_assert(brush.props.readScalar("limited", type, read) == PropError::ERROR_NONE &&
              read == 0);
  descriptor.name = "wrong";
  brush.props.struct_def->String("wrong", "Wrong", -1);
  test_assert(write(base) == PropError::ERROR_INVALID_TYPE);
  test_assert(store.get(3) == T(authored));
  descriptor.name = "authored";
  descriptor.dynamic = false;
  test_assert(write(base) == PropError::ERROR_NONE);
  test_assert(store.get(3) == T(base));
  test_assert(brush.props.readScalar("authored", type, read) == PropError::ERROR_NONE &&
              read == authored);

  Brush child;
  child.props.struct_def->parent = brush.props.struct_def;
  descriptor.dynamic = true;
  test_assert(named_uniform_detail::StoreAccess::write(
                  store, child.props, &descriptor, type, 3, authored) ==
              PropError::ERROR_INVALID_OWNER);
  test_assert(store.get(3) == T(base));
  test_assert(brush.props.readScalar("authored", type, read) == PropError::ERROR_NONE &&
              read == authored);
  child.props.struct_def->parent = nullptr;
  test_assert(named_uniform_detail::StoreAccess::write(
                  store, child.props, &descriptor, type, 3, authored) ==
              PropError::ERROR_NONE);
  test_assert(store.get(3) == T(authored));
  descriptor.type = Prop::STRING;
  test_assert(write(base) == PropError::ERROR_INVALID_TYPE);
  test_assert(store.get(3) == T(authored));
}

static void generatedLoadsAndHostWrites()
{
  Brush brush;
  CommandExecutor::brush_command command;
  command::createNamedstoragetestBrush<CommandExecutor, AccumLive>(command);
  test_assert(command.registerProps(*brush.props.struct_def).error ==
              PropError::ERROR_NONE);
  brush.props.setFloat("storage_gain", 4.0f);
  brush.addPropDynamicByName("storage_gain",
                             int(props::DeviceType::PRESSURE),
                             int(litestl::math::BasicMix::MULTIPLY),
                             1.0f);
  brush.setPropDynamicSampleByName(
      "storage_gain", int(props::DeviceType::PRESSURE), 0, 2, 0.5f);
  brush.setPropDynamicSampleByName(
      "storage_gain", int(props::DeviceType::PRESSURE), 1, 2, 0.5f);
  brush.pushDeviceInput(int(props::DeviceType::PRESSURE), 0.5f);
  command.loadUniformProps(brush, &brush.deviceInputCtx);
  test_assert(brush.getNamedFloat(kExtraSlot_storage_gain) == 2.0f);
  test_assert(brush.props.lookupValue<float>("storage_gain", -1) == 4.0f);
  CommandCtxBase context;
  command.execHost(context, brush);
  test_assert(brush.getNamedFloat(kExtraSlot_storage_work) == 3.0f);
  test_assert(brush.getNamedFloat(kExtraSlot_storage_gain) == 4.0f);
  test_assert(brush.props.lookupValue<float>("storage_gain", -1) == 4.0f);
  command.loadUniformProps(brush, &brush.deviceInputCtx);
  test_assert(brush.getNamedFloat(kExtraSlot_storage_gain) == 2.0f);
  unsigned char packed[112]{};
  command::packNamedstoragetestGpuUniforms(brush, packed);
  float packedGain = 0;
  std::memcpy(&packedGain, packed + 72, sizeof(float));
  test_assert(packedGain == 2.0f);
}

static void registryCompatibility()
{
  using namespace sculptcore::brush::sbrush;
  RegistryEntry first;
  first.stem = "first";
  first.attrName = "first";
  first.cppName = "First";
  first.storeUniforms.append(
      StoreUniform{"gain", Prop::FLOAT32, true, 2, true, 0, 10, true});
  RegistryEntry second = first;
  second.stem = "second";
  second.attrName = "second";
  second.cppName = "Second";
  auto compile = [&](const RegistryEntry &other) {
    Vector<RegistryEntry> entries;
    entries.append(first);
    entries.append(other);
    return emitRegistry(entries, {}, {}, {});
  };
  auto valid = compile(second);
  test_assert(valid.errors.size() == 0);
  test_assert(std::strstr(valid.genHeader.c_str(), "ensureNamedFloatDefault") != nullptr);
  test_assert(std::strstr(valid.genHeader.c_str(),
                          "generatedExtraNamedUniformDescriptor") != nullptr);
  for (int change = 0; change < 5; change++) {
    auto altered = second;
    auto &uniform = altered.storeUniforms[0];
    switch (change) {
    case 0:
      uniform.dynamic = false;
      break;
    case 1:
      uniform.hasDefault = false;
      break;
    case 2:
      uniform.rangeMax = 11;
      break;
    case 3:
      uniform.type = Prop::INT32;
      break;
    case 4:
      uniform.defaultValue = std::numeric_limits<double>::quiet_NaN();
      break;
    }
    auto rejected = compile(altered);
    test_assert(rejected.errors.size() > 0 && rejected.genHeader.size() == 0);
  }
}

int main()
{
  storageValues();
  authoredWrites<float>(Prop::FLOAT32);
  authoredWrites<int32_t>(Prop::INT32);
  authoredWrites<bool>(Prop::BOOL);
  generatedLoadsAndHostWrites();
  registryCompatibility();
  return test_end();
}
