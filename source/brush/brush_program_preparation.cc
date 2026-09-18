#include "brush_program_preparation.h"
#include "brush_command.h"
#include "brush_configuration.h"
#include "props/prop_coerce.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace sculptcore::brush {
using props::Prop;
using props::PropError;

props::PropError BrushProgram::setCommandScalarChecked(int idx,
                                                       util::string name,
                                                       props::Prop type,
                                                       double value)
{
  if (idx < 0 || size_t(idx) >= commands.size() || !name.size() ||
      std::strlen(name.c_str()) != name.size())
    return PropError::ERROR_NOT_EXISTS;
  if (type != Prop::FLOAT32 && type != Prop::INT32 && type != Prop::BOOL)
    return PropError::ERROR_INVALID_TYPE;
  bool valid = false;
  props::numtype_dispatch(type, [&]<typename P>() {
    typename P::value_type typed;
    valid = named_uniform_detail::convert(value, typed);
  });
  if (!valid)
    return PropError::ERROR_INVALID_VALUE;
  for (auto &item : commands[idx].scalarOverrides) {
    if (item.name == name) {
      item = {name, type, value};
      return PropError::ERROR_NONE;
    }
  }
  commands[idx].scalarOverrides.append({name, type, value});
  return PropError::ERROR_NONE;
}

static PropError commandStackTarget(const BrushProgram &program,
                                    int idx,
                                    const util::string &name,
                                    int scalarType)
{
  if (idx < 0 || size_t(idx) >= program.commands.size() || !name.size() ||
      std::strlen(name.c_str()) != name.size())
    return PropError::ERROR_NOT_EXISTS;
  if (scalarType != int(Prop::FLOAT32) && scalarType != int(Prop::INT32) &&
      scalarType != int(Prop::BOOL))
    return PropError::ERROR_INVALID_TYPE;
  return PropError::ERROR_NONE;
}

int BrushProgram::replaceCommandResponseDynamicsChecked(int idx,
                                                        util::string name,
                                                        int scalarType,
                                                        util::Vector<int> &devices,
                                                        util::Vector<int> &modes,
                                                        util::Vector<float> &factors,
                                                        util::Vector<int> &enabled,
                                                        util::Vector<int> &offsets,
                                                        util::Vector<float> &samples,
                                                        util::Vector<int> &kinds,
                                                        util::Vector<double> &parameters)
{
  auto error = commandStackTarget(*this, idx, name, scalarType);
  if (error != PropError::ERROR_NONE)
    return int(error);
  BrushDynamicsOverride candidate{name, Prop(scalarType), {}};
  int status = decodeResponseDynamics(devices,
                                      modes,
                                      factors,
                                      enabled,
                                      offsets,
                                      samples,
                                      kinds,
                                      parameters,
                                      candidate.dynamics);
  if (status)
    return status;
  for (auto &item : commands[idx].dynamicsOverrides) {
    if (item.name == name) {
      item = std::move(candidate);
      return 0;
    }
  }
  commands[idx].dynamicsOverrides.append(std::move(candidate));
  return 0;
}

int BrushProgram::removeCommandDynamicsChecked(int idx, util::string name, int scalarType)
{
  auto error = commandStackTarget(*this, idx, name, scalarType);
  if (error != PropError::ERROR_NONE)
    return int(error);
  Vector<BrushDynamicsOverride> candidate;
  for (const auto &item : commands[idx].dynamicsOverrides) {
    if (item.name == name) {
      if (item.type != Prop(scalarType))
        return int(PropError::ERROR_INVALID_TYPE);
    } else {
      candidate.append(item);
    }
  }
  commands[idx].dynamicsOverrides = std::move(candidate);
  return 0;
}

int BrushProgram::replaceCommandCavityCurveChecked(int idx, Vector<float> &samples)
{
  if (idx < 0 || size_t(idx) >= commands.size())
    return int(PropError::ERROR_NOT_EXISTS);
  if (samples.size() != Brush::kCavityCurveLutSize)
    return int(PropError::ERROR_INVALID_VALUE);
  for (float sample : samples) {
    if (!std::isfinite(sample))
      return int(PropError::ERROR_INVALID_VALUE);
  }
  Vector<float> candidate(samples);
  commands[idx].cavityCurveOverride = std::move(candidate);
  return 0;
}

int BrushProgram::removeCommandCavityCurveChecked(int idx)
{
  if (idx < 0 || size_t(idx) >= commands.size())
    return int(PropError::ERROR_NOT_EXISTS);
  commands[idx].cavityCurveOverride.clear();
  return 0;
}

props::ScalarRegistrationResult programError(size_t index,
                                             props::ScalarRegistrationResult error)
{
  char prefix[64];
  std::snprintf(prefix, sizeof(prefix), "command[%zu].", index);
  error.name = util::string(prefix) + error.name;
  return error;
}

props::ScalarRegistrationResult prepareProgramScalars(
    Brush &brush,
    const BrushProgram &program,
    std::span<const std::span<const BrushUniformManifestEntry>> manifests,
    const props::DeviceInputCtx &inputs,
    PreparedProgramScalars &output)
{
  if (!brush.props.struct_def || manifests.size() != program.commands.size())
    return {PropError::ERROR_INVALID_OWNER, "program"};
  util::Vector<props::ScalarDeclaration> declarations;
  util::Vector<BrushUniformManifestEntry> scope;
  for (size_t i = 0; i < manifests.size(); i++) {
    for (const auto &entry : manifests[i]) {
      if (!executorSetting(entry.name) &&
          (entry.scalarType == Prop::FLOAT32 || entry.scalarType == Prop::INT32 ||
           entry.scalarType == Prop::BOOL))
      {
        declarations.append({entry.name,
                             entry.scalarType,
                             entry.hasDefault,
                             entry.def,
                             entry.hasRange,
                             entry.rangeMin,
                             entry.rangeMax,
                             entry.dynamic});
      }
      bool found = false;
      for (const auto &known : scope)
        found |= known.name == entry.name;
      if (!found)
        scope.append(entry);
    }
    auto result = brush.props.struct_def->validateScalarDeclarations(
        {declarations.data(), declarations.size()});
    if (result.error != PropError::ERROR_NONE)
      return programError(i, result);
  }
  PreparedProgramScalars candidate;
  for (size_t i = 0; i < manifests.size(); i++) {
    const auto &command = program.commands[i];
    util::Vector<BrushScalarOverride> overrides;
    for (const auto &item : command.floatOverrides) {
      if (!item.name.size() && (item.propId < 0 || item.propId > int(BrushProp::Invert)))
        return programError(i, {PropError::ERROR_NOT_EXISTS, "common property ID"});
      overrides.append(
          {item.name.size() ? item.name : util::string(brushPropName(item.propId)),
           Prop::FLOAT32,
           double(item.value)});
    }
    for (const auto &item : command.scalarOverrides)
      overrides.append(item);
    if (command.overrideInvert)
      overrides.append({"invert", Prop::BOOL, command.invertValue ? 1.0 : 0.0});
    for (const auto &item : overrides) {
      if (!item.name.size() || std::strlen(item.name.c_str()) != item.name.size())
        return programError(i, {PropError::ERROR_NOT_EXISTS, "property name"});
    }
    PreparedBrushScalars stage;
    auto result = prepareBrushScalars(
        brush,
        manifests[i],
        inputs,
        stage,
        {overrides.data(), overrides.size()},
        {scope.data(), scope.size()},
        ScalarSource::Authored,
        {command.dynamicsOverrides.size() ? &command.dynamicsOverrides[0] : nullptr,
         command.dynamicsOverrides.size()},
        {command.cavityCurveOverride.size() ? &command.cavityCurveOverride[0] : nullptr,
         command.cavityCurveOverride.size()});
    if (result.error != PropError::ERROR_NONE)
      return programError(i, result);
    float radius = -1;
    for (const auto &value : stage.values())
      if (value.name == util::string("radius"))
        radius = float(value.value);
    if (!std::isfinite(radius) || radius < 0 ||
        (radius > 0 && !std::isfinite(1.0f / radius)))
      return programError(i, {PropError::ERROR_INVALID_VALUE, "radius"});
    candidate.radius = std::max(candidate.radius, radius);
    candidate.radii.append(radius);
    candidate.stages.append(std::move(stage));
  }
  output = std::move(candidate);
  return {};
}

ScopedBrushWorkingValues::ScopedBrushWorkingValues(
    Brush &brush, std::span<const PreparedBrushScalars> stages, bool executorOnly)
    : brush_(brush), executorOnly_(executorOnly), floatSize_(brush.namedFloats.size()),
      intSize_(brush.namedInts.size()), boolSize_(brush.namedBools.size()),
      cavityCurve_(brush.cavity_curve), unboundedExtent_(brush.unboundedExtent)
{
  for (const auto &stage : stages) {
    for (const auto &value : stage.values()) {
      if (executorOnly && !executorSetting(value.name))
        continue;
      bool found = false;
      for (const auto &known : saved_)
        found |= known.type == value.type && known.member == value.nativeMember &&
                 known.slot == value.storeSlot;
      if (found)
        continue;
      Saved saved{value.type, value.nativeMember, value.storeSlot};
      props::numtype_dispatch(value.type, [&]<typename P>() {
        using T = typename P::value_type;
        static_assert(sizeof(T) <= 4);
        if (value.nativeMember >= 0) {
          std::memcpy(
              saved.bytes.data(),
              Brush::builtinPropDescriptorSpan()[value.nativeMember].address(brush),
              sizeof(T));
        } else {
          const auto &store = [&]() -> const NamedUniformStore<T> & {
            if constexpr (std::is_same_v<T, float>)
              return brush.namedFloats;
            else if constexpr (std::is_same_v<T, int32_t>)
              return brush.namedInts;
            else
              return brush.namedBools;
          }();
          if (size_t(value.storeSlot) < store.size())
            std::memcpy(saved.bytes.data(), &store.values_[value.storeSlot], sizeof(T));
          saved.initialized = store.initialized(value.storeSlot);
        }
      });
      saved_.append(saved);
    }
  }
}

ScopedBrushWorkingValues::~ScopedBrushWorkingValues()
{
  brush_.cavity_curve = cavityCurve_;
  brush_.unboundedExtent = unboundedExtent_;
  for (const auto &saved : saved_) {
    props::numtype_dispatch(saved.type, [&]<typename P>() {
      using T = typename P::value_type;
      if (saved.member >= 0) {
        std::memcpy(Brush::builtinPropDescriptorSpan()[saved.member].address(brush_),
                    saved.bytes.data(),
                    sizeof(T));
      } else {
        auto &store = [&]() -> NamedUniformStore<T> & {
          if constexpr (std::is_same_v<T, float>)
            return brush_.namedFloats;
          else if constexpr (std::is_same_v<T, int32_t>)
            return brush_.namedInts;
          else
            return brush_.namedBools;
        }();
        if (size_t(saved.slot) < store.size()) {
          std::memcpy(&store.values_[saved.slot], saved.bytes.data(), sizeof(T));
          store.initialized_[saved.slot] = saved.initialized;
        }
      }
    });
  }
  if (executorOnly_)
    return;
  brush_.namedFloats.values_.resize(floatSize_);
  brush_.namedFloats.initialized_.resize(floatSize_);
  brush_.namedInts.values_.resize(intSize_);
  brush_.namedInts.initialized_.resize(intSize_);
  brush_.namedBools.values_.resize(boolSize_);
  brush_.namedBools.initialized_.resize(boolSize_);
}

} // namespace sculptcore::brush
