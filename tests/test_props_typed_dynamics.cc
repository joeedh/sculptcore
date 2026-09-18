#include "props/prop_struct.h"
#include "test_util.h"

#include <cmath>
#include <limits>

test_init;
using namespace sculptcore::props;
using Mix = litestl::math::BasicMix;

template <typename T>
T evaluate(Dynamics &d,
           T base,
           T minimum = std::numeric_limits<T>::lowest(),
           T maximum = std::numeric_limits<T>::max())
{
  T result{};
  test_assert(d.evaluateChecked(base, minimum, maximum, result));
  return result;
}

static DeviceInputCtx pressure(float value)
{
  DeviceInputCtx ctx;
  ctx.push(int(DeviceType::PRESSURE), value);
  return ctx;
}

static void contextEvaluation()
{
  Dynamics dynamics;
  test_assert(dynamics.configure(DeviceType::PRESSURE, Mix::MULTIPLY, 1));
  test_assert(dynamics.configure(DeviceType::TILTX, Mix::ADD, 0.25f));
  test_assert(dynamics.devices[0].setCurveTable({0.2f, 0.9f}));
  dynamics.inputDeviceDatas(pressure(0.875f));
  auto *table = dynamics.devices[0].curveTable.data();
  auto unchanged = [&]() {
    test_assert(dynamics.devices[0].hasDeviceValue);
    test_assert(dynamics.devices[0].curDeviceValue == 0.875f);
    test_assert(!dynamics.devices[1].hasDeviceValue);
    test_assert(dynamics.devices[0].curveTable.data() == table);
  };
  auto ctx = pressure(0.1f);
  int integer = 0;
  test_assert(dynamics.evaluateChecked(16777217, INT32_MIN, INT32_MAX, ctx, integer));
  test_assert(integer == 4529849);
  unchanged();
  for (const float sample : {0.0f, 0.1f, 0.5f, 1.0f, -1.0f, 2.0f}) {
    ctx = pressure(sample);
    ctx.push(int(DeviceType::TILTX), 0.2f);
    Dynamics cached = dynamics;
    cached.inputDeviceDatas(ctx);
    float value = 0;
    bool boolean = false;
    test_assert(dynamics.evaluateChecked(3.0f, -10.0f, 10.0f, ctx, value));
    test_assert(value == evaluate(cached, 3.0f, -10.0f, 10.0f));
    test_assert(dynamics.evaluateChecked(-3, -10, 10, ctx, integer));
    test_assert(integer == evaluate(cached, -3, -10, 10));
    test_assert(dynamics.evaluateChecked(true, false, true, ctx, boolean));
    test_assert(boolean == evaluate(cached, true, false, true));
    unchanged();
  }
  ctx = pressure(0.25f);
  ctx.inputs.append({DeviceType::PRESSURE, 0.75f});
  float value = 0;
  test_assert(dynamics.evaluateChecked(4.0f, -10.0f, 10.0f, ctx, value));
  test_assert(std::fabs(value - 2.9f) < 1e-6f);
  ctx.inputs.append({DeviceType::PRESSURE, std::numeric_limits<float>::quiet_NaN()});
  test_assert(dynamics.evaluateChecked(4.0f, -10.0f, 10.0f, ctx, value) && value == 4);
  unchanged();
  ctx.clear();
  test_assert(dynamics.evaluateChecked(-0.0f, -10.0f, 10.0f, ctx, value));
  test_assert(std::signbit(value));
  unchanged();

  // Checked StructProp evaluation must also preserve existing caches on errors.
  StructDef def;
  auto &prop = def.Int32("value", "Value", -1);
  prop.Default(16777217);
  prop.dynamics = dynamics;
  prop.dynamics.inputDeviceDatas(pressure(0.875f));
  StructProp properties(&def);
  ctx = pressure(0.1f);
  double output = 0;
  test_assert(properties.evaluateScalar("value", Prop::INT32, ctx, output) ==
              PropError::ERROR_NONE);
  test_assert(output == 4529849);
  test_assert(prop.dynamics.devices[0].curDeviceValue == 0.875f);
  prop.dynamics.devices[0].flag = DynamicFlags::DISABLED;
  test_assert(prop.dynamics.devices[0].setCurveSample(0, 3, 0.5f));
  output = 99;
  test_assert(properties.evaluateScalar("value", Prop::INT32, ctx, output) ==
              PropError::ERROR_INVALID_DYNAMICS);
  test_assert(output == 99 && prop.dynamics.devices[0].curDeviceValue == 0.875f);
  test_assert(prop.dynamics.devices[0].hasDeviceValue);
}

int main()
{
  contextEvaluation();
  {
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float huge = std::numeric_limits<float>::max();
    const int imax = std::numeric_limits<int>::max();
    const int imin = std::numeric_limits<int>::lowest();
    Dynamics d;
    test_assert(std::signbit(evaluate(d, -0.0f)));
    test_assert(d.configure(DeviceType::PRESSURE, Mix::MULTIPLY, 1.0f));
    d.inputDeviceDatas(pressure(0.5f));
    test_assert(evaluate(d, 3) == 2);
    test_assert(evaluate(d, -3) == -2);
    test_assert(evaluate(d, true));
    test_assert(evaluate(d, 3.0f) == 1.5f);
    test_assert(d.configure(DeviceType::TILTX, Mix::ADD, 1.0f));
    auto ctx = pressure(0.5f);
    ctx.push(int(DeviceType::TILTX), 0.4f);
    d.inputDeviceDatas(ctx);
    test_assert(evaluate(d, 3) == 2);
    test_assert(evaluate(d, 3, 0, 1) == 1);
    ctx.push(int(DeviceType::TILTX), 0.6f);
    d.inputDeviceDatas(ctx);
    test_assert(evaluate(d, 3) == 2);
    ctx.push(0, 0.49f);
    ctx.push(int(DeviceType::TILTX), 0.02f);
    d.inputDeviceDatas(ctx);
    test_assert(evaluate(d, true));
    test_assert(evaluate(d, -3, 0, 10) == 0);
    d.inputDeviceDatas(pressure(0.49f));
    test_assert(!evaluate(d, true));
    d.inputDeviceDatas(DeviceInputCtx{});
    test_assert(evaluate(d, 3) == 3);
    test_assert(std::signbit(evaluate(d, -0.0f)));
    ctx = pressure(0.25f);
    ctx.push(0, 0.75f);
    test_assert(ctx.inputs.size() == 1);
    d.inputDeviceDatas(ctx);
    test_assert(evaluate(d, 4) == 3);
    ctx.inputs.append({DeviceType::PRESSURE, nan});
    d.inputDeviceDatas(ctx);
    test_assert(evaluate(d, 4) == 4);
    ctx.push(0, 0.5f);
    d.inputDeviceDatas(ctx);
    test_assert(evaluate(d, 4) == 2);
    ctx.push(0, nan);
    d.inputDeviceDatas(ctx);
    test_assert(evaluate(d, 4) == 4);
    d.inputDeviceDatas(pressure(inf));
    test_assert(evaluate(d, 4) == 4);
    d.inputDeviceDatas(pressure(-1));
    test_assert(evaluate(d, 4) == 0);
    d.inputDeviceDatas(pressure(2));
    test_assert(evaluate(d, 4) == 4);

    d.devices.clear();
    test_assert(d.configure(DeviceType::PRESSURE, Mix::ADD, 1));
    test_assert(d.configure(DeviceType::TILTX, Mix::SUBTRACT, 1));
    for (auto &device : d.devices) {
      device.curveTable.append(3);
      device.curveTable.append(3);
    }
    ctx = pressure(0.5f);
    ctx.push(int(DeviceType::TILTX), 0.5f);
    d.inputDeviceDatas(ctx);
    test_assert(evaluate(d, 3, 0, 4) == 3);
    d.devices.clear();
    const Mix modes[] = {
        Mix::LINEAR, Mix::MULTIPLY, Mix::ADD, Mix::SUBTRACT, Mix::DIFFERENCE};
    const float expected[] = {3.125f, 3.5f, 4.125f, 3.875f, 3.875f};
    for (int i = 0; i < 5; i++) {
      test_assert(d.configure(DeviceType::PRESSURE, modes[i], 0.25f));
      d.inputDeviceDatas(pressure(0.5f));
      test_assert(evaluate(d, 4.0f) == expected[i]);
    }
    test_assert(d.configure(DeviceType::PRESSURE, Mix::ADD, 1));
    d.inputDeviceDatas(pressure(1));
    test_assert(evaluate(d, imax) == imax);
    test_assert(d.configure(DeviceType::PRESSURE, Mix::SUBTRACT, 1));
    test_assert(evaluate(d, imin) == imin);
    d.devices[0].flag = DynamicFlags::DISABLED;
    test_assert(evaluate(d, 4) == 4);
    d.devices[0].curveTable.append(0.25f);
    d.devices[0].curveTable.append(0.75f);
    test_assert(d.configure(DeviceType::PRESSURE, Mix::LINEAR, 0.5f));
    test_assert(d.devices.size() == 1);
    test_assert(d.devices[0].curveTable.size() == 2);
    test_assert(d.devices[0].flag == DynamicFlags::DISABLED);
    auto replacement = d.devices;
    replacement.append(replacement[0]);
    test_assert(!d.replaceStack(replacement));
    test_assert(d.devices.size() == 1);
    test_assert(!d.configure(DeviceType::TWIST, Mix::ADD, 1));
    test_assert(!d.configure(DeviceType::PRESSURE, Mix(999), 1));
    test_assert(!d.configure(DeviceType::PRESSURE, Mix::ADD, nan));
    test_assert(!d.configure(DeviceType::PRESSURE, Mix::ADD, 1.01f));
    replacement.clear();
    DynamicDevice layer;
    layer.curveTable.append(0.5f);
    replacement.append(layer);
    test_assert(!d.replaceStack(replacement));
    replacement[0].curveTable.append(inf);
    test_assert(!d.replaceStack(replacement));
    replacement[0].curveTable[1] = 1;
    test_assert(d.replaceStack(replacement));
    test_assert(!d.devices[0].hasDeviceValue);
    d.inputDeviceDatas(pressure(0.5f));
    test_assert(evaluate(d, 4) == 3);
    d.devices[0].curveTable[0] = -huge;
    d.devices[0].curveTable[1] = -huge;
    test_assert(d.configure(DeviceType::PRESSURE, Mix::LINEAR, 1));
    test_assert(evaluate(d, huge) == huge);
    d.devices[0].curveTable[0] = huge;
    d.devices[0].curveTable[1] = huge;
    test_assert(d.configure(DeviceType::PRESSURE, Mix::MULTIPLY, 1));
    test_assert(evaluate(d, huge) == huge);
    float result;
    test_assert(!d.evaluateChecked(nan, -huge, huge, result));
    test_assert(!d.evaluateChecked(1.0f, 2.0f, 1.0f, result));
    test_assert(!d.evaluateChecked(1.0f, 0.0f, inf, result));

    StructDef def;
    auto &ip = def.Int32("integer", "Integer", -1).Default(16777217);
    auto &bp = def.Bool("boolean", "Boolean", -1).Default(true);
    auto &fp = def.Float32("float", "Float", -1).Default(0.49f);
    ip.dynamics.configure(DeviceType::PRESSURE, Mix::MULTIPLY, 1);
    bp.dynamics.configure(DeviceType::PRESSURE, Mix::LINEAR, 1);
    StructProp props(&def);
    ctx = pressure(0.5f);
    test_assert(props.lookupValue<float>("integer", -1, &ctx) == 8388609.0f);
    ctx = pressure(0.49f);
    test_assert(props.lookupValue<float>("boolean", -1, &ctx) == 0);
    test_assert(props.lookupValue<bool>("float", false, &ctx));
    test_assert(ip.get() == 16777217 && bp.get() && fp.get() == 0.49f);
    fp.Default(nan);
    test_assert(props.lookupValue<float>("float", 123, &ctx) == 123);
    fp.Default(1);
    fp.dynamics.configure(DeviceType::PRESSURE, Mix::MULTIPLY, 1);
    fp.dynamics.devices[0].curveTable.append(huge);
    fp.dynamics.devices[0].curveTable.append(huge);
    test_assert(props.lookupValue<int>("float", 123, &ctx) == 123);
    test_assert(props.lookupValue<short>("float", 123, &ctx) == 123);
    fp.dynamics.devices[0].curveTable[0] = -huge;
    fp.dynamics.devices[0].curveTable[1] = -huge;
    test_assert(props.lookupValue<int>("float", 123, &ctx) == 123);
    test_assert(props.lookupValue<short>("float", 123, &ctx) == 123);
    fp.dynamics.devices.clear();
    fp.Default(-3.75f);
    test_assert(props.lookupValue<int>("float", 123, &ctx) == -3);
    ip.dynamics.devices[0].curveTable.append(0.2f);
    ip.dynamics.devices[0].curveTable.append(0.9f);
    ctx = pressure(0.1f);
    test_assert(props.lookupValue<int>("integer", -1, &ctx) == 4529849);

    struct Owner {
      int value;
    } owner{3};
    auto &bound = def.Int32("bound", "Bound", offsetof(Owner, value));
    bound.Default(7).Min(-10).Max(10).Step(2);
    int reads = 0, writes = 0;
    bound.getter = [&](int *value, void *who) {
      test_assert(who == &owner);
      reads++;
      return value;
    };
    bound.setter = [&](int *, void *, int &) { writes++; };
    bound.dynamics.configure(DeviceType::PRESSURE, Mix::MULTIPLY, 1);
    bound.dynamics.devices[0].curveTable.append(0);
    bound.dynamics.devices[0].curveTable.append(1);
    props.Owner(&owner);
    ctx = pressure(0.5f);
    test_assert(props.lookupValue<int>("bound", -999, &ctx) == 2);
    test_assert(owner.value == 3 && reads == 1 && writes == 0);
    auto *copy = reinterpret_cast<Int32Prop *>(
        cloneProperty(reinterpret_cast<Property *>(&bound)));
    test_assert(copy->name == litestl::util::string("bound") &&
                copy->ui_name == litestl::util::string("Bound"));
    test_assert(copy->min == -10 && copy->max == 10 && copy->step == 2);
    test_assert(*copy->internal_value() == 7 && copy->owner == nullptr);
    test_assert(copy->binding_offset == offsetof(Owner, value));
    test_assert(bool(copy->getter) && bool(copy->setter));
    test_assert(!copy->dynamics.devices[0].hasDeviceValue);
    copy->dynamics.devices[0].curveTable[1] = 0.25f;
    test_assert(bound.dynamics.devices[0].curveTable[1] == 1);
    copy->Owner(&owner);
    test_assert(copy->get() == 3 && reads == 2);
    int changed = 9;
    copy->set(changed);
    test_assert(writes == 1 && owner.value == 3);
    litestl::alloc::Delete(copy);
  }
  return test_end();
}
