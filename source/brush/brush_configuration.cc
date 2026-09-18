#include "brush/brush.h"
#include "props/prop_coerce.h"

namespace sculptcore::brush {
using props::DynamicDevice;
using props::Dynamics;
using props::Prop;
using props::PropError;

PropError Brush::accessStaticScalar(props::Property *property,
                                    double &value,
                                    bool write,
                                    bool &handled)
{
  handled = false;
  props::ScalarDeclaration declaration;
  if (!props.struct_def->scalarDeclaration(property->name, declaration) ||
      declaration.dynamic)
  {
    return PropError::ERROR_NONE;
  }
  // Common fields are always caches loaded from authored properties, even when
  // a kernel declares them static (which disables device dynamics only).
  for (int id = 0; id <= int(BrushProp::Invert); id++) {
    if (property->name == util::string(brushPropName(id))) {
      return PropError::ERROR_NONE;
    }
  }
  void *address = nullptr;
  int slot = -1;
  for (const auto &member : builtinPropDescriptorSpan()) {
    if (property->name == util::string(member.name)) {
      handled = true;
      if (member.type != property->type) {
        return PropError::ERROR_SCHEMA_CONFLICT;
      }
      address = member.address(*this);
      break;
    }
  }
  if (!handled) {
    auto *extra = extraNamedUniformDescriptor(property->name, slot);
    if (!extra) {
      return PropError::ERROR_NONE; // Ordinary unbound static properties still use
                                    // property storage.
    }
    handled = true;
    if (extra->type != property->type || extra->dynamic) {
      return PropError::ERROR_SCHEMA_CONFLICT;
    }
  }
  auto access = [&]<typename T>(NamedUniformStore<T> &store) {
    using P = std::conditional_t<std::is_same_v<T, float>,
                                 props::Float32Prop,
                                 std::conditional_t<std::is_same_v<T, int32_t>,
                                                    props::Int32Prop,
                                                    props::BoolProp>>;
    auto *typed = static_cast<P *>(static_cast<props::detail::PropBaseType *>(property));
    T candidate{};
    if (write) {
      if (!named_uniform_detail::convert(value, candidate)) {
        return PropError::ERROR_INVALID_VALUE;
      }
    } else {
      if (!address && !store.initialized(slot)) {
        return PropError::ERROR_NOT_EXISTS;
      }
      candidate = address ? *static_cast<T *>(address) : store.get(slot);
    }
    if (!std::isfinite(double(candidate)) || !std::isfinite(double(typed->min)) ||
        !std::isfinite(double(typed->max)) || typed->min > typed->max ||
        candidate < typed->min || candidate > typed->max)
    {
      return PropError::ERROR_INVALID_VALUE;
    }
    if (write) {
      if (address) {
        *static_cast<T *>(address) = candidate;
      } else {
        store.set(slot, candidate);
      }
    } else {
      value = double(candidate);
    }
    return PropError::ERROR_NONE;
  };
  switch (property->type) {
  case Prop::FLOAT32:
    return access(namedFloats);
  case Prop::INT32:
    return access(namedInts);
  case Prop::BOOL:
    return access(namedBools);
  default:
    return PropError::ERROR_INVALID_TYPE;
  }
}

PropError Brush::checkedScalarTarget(util::string name,
                                     int scalarType,
                                     bool writable,
                                     bool dynamic,
                                     props::Property *&property)
{
  property = nullptr;
  if (scalarType != int(Prop::FLOAT32) && scalarType != int(Prop::INT32) &&
      scalarType != int(Prop::BOOL))
  {
    return PropError::ERROR_INVALID_TYPE;
  }
  if (!props.struct_def) {
    return PropError::ERROR_INVALID_OWNER;
  }
  auto *local = props.struct_def->lookupLocal(name);
  if (!local) {
    return props.struct_def->lookup(name) ? PropError::ERROR_INVALID_OWNER
                                          : PropError::ERROR_NOT_EXISTS;
  }
  if (int(local->type) != scalarType) {
    return PropError::ERROR_INVALID_TYPE;
  }
  if (props.struct_def->validateScalarSchema(name) != PropError::ERROR_NONE) {
    return PropError::ERROR_SCHEMA_CONFLICT;
  }
  if (writable && (int(local->flag) & int(props::PropFlag::READ_ONLY))) {
    return PropError::ERROR_READ_ONLY;
  }
  if (dynamic) {
    props::ScalarDeclaration declaration;
    bool eligible = false;
    if (props.struct_def->scalarDeclaration(name, declaration)) {
      eligible = declaration.dynamic;
    } else {
      for (int id = 0; id <= int(BrushProp::Invert); id++) {
        if (name == util::string(brushPropName(id))) {
          eligible = scalarType ==
                     int(id == int(BrushProp::Invert) ? Prop::BOOL : Prop::FLOAT32);
          break;
        }
      }
    }
    if (!eligible) {
      return PropError::ERROR_INVALID_DYNAMICS;
    }
  }
  property = local;
  return PropError::ERROR_NONE;
}

PropError Brush::checkedDynamicsTarget(util::string name,
                                       int scalarType,
                                       bool repair,
                                       Dynamics *&dynamics)
{
  dynamics = nullptr;
  props::Property *property = nullptr;
  auto error = checkedScalarTarget(name, scalarType, true, true, property);
  if (error != PropError::ERROR_NONE) {
    return error;
  }
  props::numtype_dispatch(property->type, [&]<typename P>() {
    dynamics =
        &static_cast<P *>(static_cast<props::detail::PropBaseType *>(property))->dynamics;
  });
  if (!repair && !Dynamics::validStack(dynamics->devices, true)) {
    dynamics = nullptr;
    return PropError::ERROR_INVALID_DYNAMICS;
  }
  return PropError::ERROR_NONE;
}

BrushScalarResult
Brush::readScalarChecked(util::string name, int scalarType, bool evaluate)
{
  BrushScalarResult result;
  props::Property *property = nullptr;
  auto error = checkedScalarTarget(name, scalarType, false, evaluate, property);
  if (error == PropError::ERROR_NONE) {
    bool handled;
    error = accessStaticScalar(property, result.value, false, handled);
    if (!handled && error == PropError::ERROR_NONE) {
      error =
          evaluate
              ? props.evaluateScalar(name, Prop(scalarType), deviceInputCtx, result.value)
              : props.readScalar(name, Prop(scalarType), result.value);
    }
  }
  result.status = int(error);
  return result;
}

int Brush::writeScalarChecked(util::string name, int scalarType, double value)
{
  props::Property *property = nullptr;
  auto error = checkedScalarTarget(name, scalarType, true, false, property);
  if (error != PropError::ERROR_NONE) {
    return int(error);
  }
  if (scalarType == int(Prop::FLOAT32)) {
    float converted;
    if (!named_uniform_detail::convert(value, converted))
      return int(PropError::ERROR_INVALID_VALUE);
  }
  if (configurationGeneration_ == UINT64_MAX) {
    return int(PropError::ERROR_INVALID_VALUE);
  }
  bool handled;
  error = accessStaticScalar(property, value, true, handled);
  if (!handled && error == PropError::ERROR_NONE) {
    error = props.setScalarLocal(name, Prop(scalarType), value);
  }
  if (error == PropError::ERROR_NONE) {
    configurationGeneration_++;
  }
  return int(error);
}

int Brush::commitDynamics(Dynamics &target, const Dynamics &candidate)
{
  bool same = target.devices.size() == candidate.devices.size();
  for (size_t i = 0; same && i < target.devices.size(); i++) {
    same = target.devices[i].sameConfiguration(candidate.devices[i]);
  }
  if (same) {
    return 0;
  }
  if (configurationGeneration_ == UINT64_MAX) {
    return int(PropError::ERROR_INVALID_VALUE);
  }
  target = candidate;
  configurationGeneration_++;
  return 0;
}

static int deviceIndex(const Dynamics &dynamics, int device)
{
  if (device < int(props::DeviceType::PRESSURE) || device > int(props::DeviceType::SPEED))
  {
    return -1;
  }
  for (size_t i = 0; i < dynamics.devices.size(); i++) {
    if (int(dynamics.devices[i].type) == device) {
      return int(i);
    }
  }
  return -1;
}

int Brush::configureDynamicChecked(
    util::string name, int scalarType, int device, int mode, float factor)
{
  Dynamics *target;
  auto error = checkedDynamicsTarget(name, scalarType, false, target);
  if (error != PropError::ERROR_NONE) {
    return int(error);
  }
  Dynamics candidate = *target;
  if (!candidate.configure(props::DeviceType(device), math::BasicMix(mode), factor)) {
    return int(PropError::ERROR_INVALID_DYNAMICS);
  }
  return commitDynamics(*target, candidate);
}

int Brush::enableDynamicChecked(util::string name,
                                int scalarType,
                                int device,
                                int enabled)
{
  if (enabled != 0 && enabled != 1) {
    return int(PropError::ERROR_INVALID_VALUE);
  }
  Dynamics *target;
  auto error = checkedDynamicsTarget(name, scalarType, false, target);
  if (error != PropError::ERROR_NONE) {
    return int(error);
  }
  int index = deviceIndex(*target, device);
  if (index < 0) {
    return int(PropError::ERROR_NOT_EXISTS);
  }
  Dynamics candidate = *target;
  auto &flag = candidate.devices[index].flag;
  flag = props::DynamicFlags(enabled ? int(flag) & ~int(props::DynamicFlags::DISABLED)
                                     : int(flag) | int(props::DynamicFlags::DISABLED));
  return commitDynamics(*target, candidate);
}

int Brush::moveDynamicChecked(util::string name, int scalarType, int device, int index)
{
  Dynamics *target;
  auto error = checkedDynamicsTarget(name, scalarType, false, target);
  if (error != PropError::ERROR_NONE) {
    return int(error);
  }
  int from = deviceIndex(*target, device);
  if (from < 0) {
    return int(PropError::ERROR_NOT_EXISTS);
  }
  if (index < 0 || index >= int(target->devices.size())) {
    return int(PropError::ERROR_INVALID_VALUE);
  }
  Dynamics candidate = *target;
  auto layer = candidate.devices[from];
  for (int i = from; i != index; i += from < index ? 1 : -1) {
    candidate.devices[i] = candidate.devices[i + (from < index ? 1 : -1)];
  }
  candidate.devices[index] = std::move(layer);
  return commitDynamics(*target, candidate);
}

int Brush::clearDynamicsChecked(util::string name, int scalarType)
{
  Dynamics *target;
  auto error = checkedDynamicsTarget(name, scalarType, true, target);
  if (error != PropError::ERROR_NONE) {
    return int(error);
  }
  return commitDynamics(*target, Dynamics{});
}

int Brush::replaceDynamicTableChecked(util::string name,
                                      int scalarType,
                                      int device,
                                      const util::Vector<float> &samples)
{
  Dynamics *target;
  auto error = checkedDynamicsTarget(name, scalarType, false, target);
  if (error != PropError::ERROR_NONE) {
    return int(error);
  }
  int index = deviceIndex(*target, device);
  if (index < 0) {
    return int(PropError::ERROR_NOT_EXISTS);
  }
  Dynamics candidate = *target;
  if (!candidate.devices[index].setCurveTable(samples)) {
    return int(PropError::ERROR_INVALID_DYNAMICS);
  }
  return commitDynamics(*target, candidate);
}

int Brush::setDynamicSampleChecked(
    util::string name, int scalarType, int device, int index, int count, float value)
{
  Dynamics *target;
  auto error = checkedDynamicsTarget(name, scalarType, false, target);
  if (error != PropError::ERROR_NONE) {
    return int(error);
  }
  int layer = deviceIndex(*target, device);
  if (layer < 0) {
    return int(PropError::ERROR_NOT_EXISTS);
  }
  Dynamics candidate = *target;
  if (!candidate.devices[layer].setCurveSample(index, count, value)) {
    return int(PropError::ERROR_INVALID_DYNAMICS);
  }
  return commitDynamics(*target, candidate);
}

bool Brush::replaceFalloffCurveChecked(util::Vector<float> &samples)
{
  if (samples.size() != kFalloffCurveSize) {
    return false;
  }
  for (float value : samples) {
    if (!std::isfinite(value)) {
      return false;
    }
  }
  std::copy(samples.begin(), samples.end(), falloff_curve.begin());
  return true;
}

bool Brush::replaceCavityCurveChecked(util::Vector<float> &samples)
{
  if (samples.size() != kCavityCurveLutSize) {
    return false;
  }
  for (float value : samples) {
    if (!std::isfinite(value)) {
      return false;
    }
  }
  std::copy(samples.begin(), samples.end(), cavity_curve.begin());
  return true;
}

int Brush::replaceDynamicsChecked(util::string name,
                                  int scalarType,
                                  const util::Vector<int> &devices,
                                  const util::Vector<int> &modes,
                                  const util::Vector<float> &factors,
                                  const util::Vector<int> &enabled,
                                  const util::Vector<int> &offsets,
                                  const util::Vector<float> &samples)
{
  if (devices.size() > 4) {
    return int(PropError::ERROR_INVALID_DYNAMICS);
  }
  util::Vector<int> kinds;
  util::Vector<double> parameters;
  for (size_t i = 0; i < devices.size(); i++) {
    kinds.append(0);
    parameters.append(0);
    parameters.append(0);
    parameters.append(0);
  }
  return replaceResponseDynamicsChecked(name,
                                        scalarType,
                                        devices,
                                        modes,
                                        factors,
                                        enabled,
                                        offsets,
                                        samples,
                                        kinds,
                                        parameters);
}

int Brush::replaceResponseDynamicsChecked(util::string name,
                                          int scalarType,
                                          const util::Vector<int> &devices,
                                          const util::Vector<int> &modes,
                                          const util::Vector<float> &factors,
                                          const util::Vector<int> &enabled,
                                          const util::Vector<int> &offsets,
                                          const util::Vector<float> &samples,
                                          const util::Vector<int> &kinds,
                                          const util::Vector<double> &parameters)
{
  Dynamics *target;
  auto error = checkedDynamicsTarget(name, scalarType, true, target);
  if (error != PropError::ERROR_NONE) {
    return int(error);
  }
  Dynamics candidate;
  int status = decodeResponseDynamics(
      devices, modes, factors, enabled, offsets, samples, kinds, parameters, candidate);
  return status ? status : commitDynamics(*target, candidate);
}

int decodeResponseDynamics(const util::Vector<int> &devices,
                           const util::Vector<int> &modes,
                           const util::Vector<float> &factors,
                           const util::Vector<int> &enabled,
                           const util::Vector<int> &offsets,
                           const util::Vector<float> &samples,
                           const util::Vector<int> &kinds,
                           const util::Vector<double> &parameters,
                           props::Dynamics &output)
{
  size_t count = devices.size();
  if (count > 4 || kinds.size() != count || parameters.size() != count * 3 ||
      modes.size() != count || factors.size() != count || enabled.size() != count ||
      offsets.size() != count + 1 || offsets[0] != 0 ||
      samples.size() > 4 * props::kDeviceCurveSampleLimit ||
      offsets[count] != int(samples.size()))
  {
    return int(PropError::ERROR_INVALID_DYNAMICS);
  }
  Dynamics candidate;
  for (size_t i = 0; i < count; i++) {
    if (offsets[i] < 0 || offsets[i + 1] < offsets[i] ||
        offsets[i + 1] > int(samples.size()) || (enabled[i] != 0 && enabled[i] != 1))
    {
      return int(PropError::ERROR_INVALID_DYNAMICS);
    }
    int length = offsets[i + 1] - offsets[i];
    if (length == 1 || length > props::kDeviceCurveSampleLimit) {
      return int(PropError::ERROR_INVALID_DYNAMICS);
    }
    DynamicDevice layer;
    layer.responseKind = kinds[i];
    layer.responseThreshold = parameters[i * 3];
    layer.responseLow = parameters[i * 3 + 1];
    layer.responseHigh = parameters[i * 3 + 2];
    layer.type = props::DeviceType(devices[i]);
    layer.mixMode = math::BasicMix(modes[i]);
    layer.mixFactor = factors[i];
    layer.flag = enabled[i] ? props::DynamicFlags::NONE : props::DynamicFlags::DISABLED;
    for (int j = offsets[i]; j < offsets[i + 1]; j++) {
      layer.curveTable.append(samples[j]);
    }
    candidate.devices.append(std::move(layer));
  }
  if (!Dynamics::validStack(candidate.devices)) {
    return int(PropError::ERROR_INVALID_DYNAMICS);
  }
  output = std::move(candidate);
  return int(PropError::ERROR_NONE);
}

static int legacyType(Brush &brush, const util::string &name)
{
  auto *property =
      brush.props.struct_def ? brush.props.struct_def->lookup(name) : nullptr;
  return property ? int(property->type) : int(Prop::INVALID_TYPE);
}

static void reportConfiguration(const util::string &name, int error)
{
  if (error) {
    fprintf(stderr, "brush property '%s': configuration error %d\n", name.c_str(), error);
  }
}

void Brush::clearPropDynamicsByName(util::string name)
{
  reportConfiguration(name, clearDynamicsChecked(name, legacyType(*this, name)));
}

void Brush::addPropDynamicByName(util::string name, int device, int mode, float factor)
{
  reportConfiguration(
      name, configureDynamicChecked(name, legacyType(*this, name), device, mode, factor));
}

void Brush::setPropDynamicSampleByName(
    util::string name, int device, int index, int count, float value)
{
  reportConfiguration(name,
                      setDynamicSampleChecked(
                          name, legacyType(*this, name), device, index, count, value));
}
} // namespace sculptcore::brush
