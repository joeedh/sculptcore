#include "brush_preparation.h"
#include "brush_command.h"
#include "brush_program.h"
#include "brushes/extra.h"
#include "mesh/mesh.h"
#include "props/prop_coerce.h"

#include <cstring>
#include <limits>

namespace sculptcore::brush {
bool validGrabFrame(const Brush &brush)
{
  for (int axis = 0; axis < 3; axis++)
    if (!std::isfinite(brush.grabFrom[axis]) || !std::isfinite(brush.grabTo[axis]))
      return false;
  return true;
}

bool resolvedAttributesSupported(std::span<const BrushAttrManifestEntry> attributes,
                                 bool enhance,
                                 bool meshDomain)
{
  for (const auto &entry : attributes) {
    if (meshDomain && entry.materialize &&
        (entry.domain == AttrElemDomain::Vertex ||
         entry.domain == AttrElemDomain::Face) &&
        (entry.type == mesh::AttrType::FLOAT || entry.type == mesh::AttrType::FLOAT2 ||
         entry.type == mesh::AttrType::FLOAT3 || entry.type == mesh::AttrType::FLOAT4 ||
         entry.type == mesh::AttrType::INT || entry.type == mesh::AttrType::BOOL))
      continue;
    if (enhance && entry.boundName == util::string(".brush.enhance.disp") &&
        entry.type == mesh::AttrType::FLOAT3 && entry.domain == AttrElemDomain::Vertex &&
        entry.materialize && !entry.kernelWrites && entry.use == 0)
      continue;
    if (entry.boundName != util::string(".boundary.vert.class") ||
        entry.type != mesh::AttrType::INT || entry.domain != AttrElemDomain::Vertex ||
        entry.kernelWrites || entry.use != 0)
      return false;
  }
  return true;
}

props::ScalarRegistrationResult
validatePreparedAttributes(mesh::Mesh &m,
                           std::span<const BrushAttrManifestEntry> attributes,
                           std::span<const BrushAttrLayerOverride> overrides,
                           util::Vector<BrushAttrManifestEntry> &planned)
{
  using props::PropError;
  for (size_t i = 0; i < overrides.size(); i++) {
    const auto &override = overrides[i];
    if (override.attrIdx < 0 || size_t(override.attrIdx) >= attributes.size())
      return {PropError::ERROR_INVALID_VALUE, "attribute selection"};
    for (size_t j = 0; j < i; j++)
      if (overrides[j].attrIdx == override.attrIdx)
        return {PropError::ERROR_INVALID_VALUE, "duplicate attribute selection"};
  }
  for (size_t i = 0; i < attributes.size(); i++) {
    const auto &entry = attributes[i];
    if (entry.domain != AttrElemDomain::Vertex && entry.domain != AttrElemDomain::Face)
      return {PropError::ERROR_INVALID_VALUE, entry.handle};
    auto &group = entry.domain == AttrElemDomain::Vertex ? m.v.attrs : m.f.attrs;
    const mesh::AttrRef *target = nullptr;
    int selected = -1;
    bool explicitTarget = false;
    for (const auto &override : overrides)
      if (override.attrIdx == int(i)) {
        explicitTarget = true;
        selected = override.layerIndex;
      }
    if (explicitTarget) {
      if (entry.boundName.size() || !entry.use || selected < 0 ||
          size_t(selected) >= group.attrs.size())
        return {PropError::ERROR_INVALID_VALUE, entry.handle};
      target = &group.attrs[selected];
    } else {
      const auto &name = entry.boundName.size() ? entry.boundName : entry.handle;
      for (const auto &prior : planned) {
        const auto &priorName = prior.boundName.size() ? prior.boundName : prior.handle;
        if (prior.domain == entry.domain && priorName == name && prior.type != entry.type)
          return {PropError::ERROR_SCHEMA_CONFLICT, entry.handle};
      }
      planned.append(entry);
      for (const auto &attr : group.attrs)
        if (attr.name == name) {
          if (attr.type != entry.type)
            return {PropError::ERROR_INVALID_TYPE, entry.handle};
          target = &attr;
        }
    }
    if (target && (target->type != entry.type || !target->data ||
                   (target->flag & mesh::AttrFlag::TOPO) ||
                   (entry.kernelWrites && (target->flag & mesh::AttrFlag::NOCOPY))))
      return {PropError::ERROR_INVALID_VALUE, entry.handle};
  }
  return {};
}

std::span<const props::ScalarDeclaration> executorSettingDeclarations()
{
  using props::Prop;
  constexpr double maxFloat = std::numeric_limits<float>::max();
  static const props::ScalarDeclaration settings[] = {
      {"automask_cavity", Prop::BOOL, false, 0, true, 0, 1, false},
      {"cavity_factor", Prop::FLOAT32, false, 0, true, -maxFloat, maxFloat, false},
      {"cavity_blur_steps", Prop::INT32, false, 0, true, 0, INT32_MAX - 1, false},
      {"cavity_inverted", Prop::BOOL, false, 0, true, 0, 1, false},
      {"cavity_use_curve", Prop::BOOL, false, 0, true, 0, 1, false},
      {"automask_view_normal", Prop::BOOL, false, 0, true, 0, 1, false},
      {"cull_backfaces", Prop::BOOL, false, 0, true, 0, 1, false},
      {"view_normal_limit", Prop::FLOAT32, false, 0, true, -maxFloat, maxFloat, false},
      {"view_normal_falloff", Prop::FLOAT32, false, 0, true, -maxFloat, maxFloat, false},
      {"enhance_rings", Prop::INT32, false, 0, true, INT32_MIN, INT32_MAX, false},
      {"enhance_inner", Prop::INT32, false, 0, true, INT32_MIN, INT32_MAX, false},
  };
  return settings;
}

bool executorSetting(const util::string &name)
{
  for (const auto &setting : executorSettingDeclarations()) {
    if (setting.name == name)
      return true;
  }
  return false;
}

namespace {
using props::Prop;
using props::PropError;
using props::ScalarDeclaration;

bool scalar(Prop type)
{
  return type == Prop::FLOAT32 || type == Prop::INT32 || type == Prop::BOOL;
}

int nativeMember(const util::string &name)
{
  auto members = Brush::builtinPropDescriptorSpan();
  for (size_t i = 0; i < members.size(); i++) {
    if (std::strcmp(name.c_str(), members[i].name) == 0) {
      return int(i);
    }
  }
  return -1;
}

int commonMember(const util::string &name)
{
  for (int id = 0; id <= int(BrushProp::Invert); id++) {
    if (std::strcmp(name.c_str(), brushPropName(id)) == 0) {
      return id;
    }
  }
  return -1;
}

const BrushUniformManifestEntry *
findUniform(std::span<const BrushUniformManifestEntry> uniforms, const util::string &name)
{
  for (const auto &entry : uniforms) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

ScalarDeclaration declarationOf(const BrushUniformManifestEntry &entry)
{
  return {entry.name,
          entry.scalarType,
          entry.hasDefault,
          entry.def,
          entry.hasRange,
          entry.rangeMin,
          entry.rangeMax,
          entry.dynamic};
}

// Every NumBase has a device stack, including types unsupported by execution.
const props::Dynamics *dynamicsOf(props::Property *property)
{
  auto *base = static_cast<props::detail::PropBaseType *>(property);
#define DYNAMICS_CASE(type, cls)                                                         \
  case Prop::type:                                                                       \
    return &static_cast<props::cls *>(base)->dynamics
  switch (property->type) {
    DYNAMICS_CASE(FLOAT32, Float32Prop);
    DYNAMICS_CASE(FLOAT64, Float64Prop);
    DYNAMICS_CASE(INT32, Int32Prop);
    DYNAMICS_CASE(INT64, Int64Prop);
    DYNAMICS_CASE(UINT32, Uint32Prop);
    DYNAMICS_CASE(UINT64, Uint64Prop);
    DYNAMICS_CASE(INT16, Int16Prop);
    DYNAMICS_CASE(UINT16, Uint16Prop);
    DYNAMICS_CASE(INT8, Int8Prop);
    DYNAMICS_CASE(UINT8, Uint8Prop);
    DYNAMICS_CASE(BOOL, BoolProp);
    DYNAMICS_CASE(VEC2F, Vec2Prop);
    DYNAMICS_CASE(VEC3F, Vec3Prop);
    DYNAMICS_CASE(VEC4F, Vec4Prop);
  default:
    return nullptr;
  }
#undef DYNAMICS_CASE
}

PropError validateTarget(const BrushUniformManifestEntry &entry)
{
  if (entry.status || entry.isFloat != (entry.scalarType == Prop::FLOAT32)) {
    return PropError::ERROR_SCHEMA_CONFLICT;
  }
  int member = nativeMember(entry.name);
  if (member >= 0) {
    const auto &native = Brush::builtinPropDescriptorSpan()[member];
    if (entry.storeSlot != -1 || (entry.dynamic && !native.dynamic)) {
      return PropError::ERROR_SCHEMA_CONFLICT;
    }
    if (!scalar(entry.scalarType)) {
      // Nonscalar native uniforms stay raw; this helper prepares scalar values only.
      return entry.scalarType == Prop::INVALID_TYPE && !scalar(native.type) &&
                     !entry.dynamic && !entry.hasDefault && !entry.hasRange
                 ? PropError::ERROR_NONE
                 : PropError::ERROR_INVALID_TYPE;
    }
    return native.type == entry.scalarType ? PropError::ERROR_NONE
                                           : PropError::ERROR_INVALID_TYPE;
  }
  auto *extra = extraNamedUniformDescriptor(entry.scalarType, entry.storeSlot);
  return scalar(entry.scalarType) && extra &&
                 std::strcmp(entry.name.c_str(), extra->name) == 0 &&
                 entry.scalarType == extra->type && entry.dynamic == extra->dynamic
             ? PropError::ERROR_NONE
             : PropError::ERROR_SCHEMA_CONFLICT;
}

PropError prepareValue(Brush &brush,
                       PreparedBrushScalar &result,
                       const ScalarDeclaration *declaration,
                       bool common,
                       const props::DeviceInputCtx &inputs,
                       const BrushScalarOverride *override,
                       ScalarSource source,
                       const BrushDynamicsOverride *stackOverride)
{
  auto &definition = *brush.props.struct_def;
  auto *property = definition.lookup(result.name);
  if (property && definition.lookupLocal(result.name) != property) {
    return PropError::ERROR_INVALID_OWNER;
  }
  if (common && !property) {
    return PropError::ERROR_NOT_EXISTS;
  }
  if (property && property->type != result.type) {
    return PropError::ERROR_INVALID_TYPE;
  }
  if (property && property->name != result.name) {
    return PropError::ERROR_SCHEMA_CONFLICT;
  }
  if (definition.validateScalarSchema(result.name) != PropError::ERROR_NONE) {
    return PropError::ERROR_SCHEMA_CONFLICT;
  }
  props::ScalarDomain domain;
  if (declaration) {
    auto error = props::normalizeScalarDeclaration(*declaration, domain);
    if (error != PropError::ERROR_NONE) {
      return error;
    }
  }
  PropError error = PropError::ERROR_INVALID_VALUE;
  props::numtype_dispatch(result.type, [&]<typename P>() {
    using T = typename P::value_type;
    auto *typed =
        property ? static_cast<P *>(static_cast<props::detail::PropBaseType *>(property))
                 : nullptr;
    if (!declaration) {
      if (!typed) {
        error = PropError::ERROR_NOT_EXISTS;
        return;
      }
      domain = {double(typed->min), double(typed->max), 0};
    }
    if (typed && !result.dynamic && typed->dynamics.devices.size() != 0) {
      error = PropError::ERROR_INVALID_DYNAMICS;
      return;
    }
    T base{};
    if (!common && source == ScalarSource::RawStandalone && typed &&
        typed->dynamics.devices.size())
    {
      error = PropError::ERROR_INVALID_DYNAMICS;
      return;
    }
    if (override && (override->type != result.type ||
                     !named_uniform_detail::convert(override->value, base)))
    {
      error = override->type != result.type ? PropError::ERROR_INVALID_TYPE
                                            : PropError::ERROR_INVALID_VALUE;
      return;
    }
    if (common || (result.dynamic && source == ScalarSource::Authored)) {
      if (typed) {
        if (typed->getter || (typed->binding_offset != -1 && brush.props.owner)) {
          error = PropError::ERROR_INVALID_OWNER;
          return;
        }
        if (!override)
          base = *typed->internal_value();
      } else if (!override) {
        base = T(domain.initial);
      }
    } else if (!override && result.nativeMember >= 0) {
      base = *static_cast<T *>(
          Brush::builtinPropDescriptorSpan()[result.nativeMember].address(brush));
    } else if (!override) {
      const auto &store = [&]() -> const NamedUniformStore<T> & {
        if constexpr (std::is_same_v<T, float>)
          return brush.namedFloats;
        else if constexpr (std::is_same_v<T, int32_t>)
          return brush.namedInts;
        else
          return brush.namedBools;
      }();
      base = store.initialized(result.storeSlot) ? store.get(result.storeSlot)
                                                 : T(domain.initial);
    }
    if constexpr (std::is_same_v<T, float>) {
      float defaultValue;
      if (declaration && declaration->hasDefault &&
          !named_uniform_detail::convert(declaration->defaultValue, defaultValue))
        return;
      if (!named_uniform_detail::supportedFloat(base))
        return;
    }
    if (!std::isfinite(double(base)) || !std::isfinite(domain.min) ||
        !std::isfinite(domain.max) || domain.min > domain.max ||
        double(base) < domain.min || double(base) > domain.max)
    {
      return;
    }
    T evaluated = base;
    const props::Dynamics *dynamics = stackOverride ? &stackOverride->dynamics
                                      : typed       ? &typed->dynamics
                                                    : nullptr;
    if (dynamics && result.dynamic &&
        !dynamics->evaluateChecked(base, T(domain.min), T(domain.max), inputs, evaluated))
    {
      error = PropError::ERROR_INVALID_DYNAMICS;
      return;
    }
    result.value = double(evaluated);
    error = PropError::ERROR_NONE;
  });
  return error;
}
} // namespace

bool resolvedFalloffSupported(const Brush &brush)
{
  switch (brush.falloff_shape) {
  case FalloffShape::Spherical:
  case FalloffShape::Cube:
    break;
  case FalloffShape::Linear:
  case FalloffShape::Box:
  case FalloffShape::RoundedBox:
    for (int axis = 0; axis < 3; axis++) {
      if (!std::isfinite(brush.falloff_dir[axis]))
        return false;
      if (brush.falloff_shape != FalloffShape::Linear &&
          (!std::isfinite(brush.falloff_extent[axis]) ||
           brush.falloff_extent[axis] <= 0 ||
           !std::isfinite(1.0f / brush.falloff_extent[axis])))
        return false;
    }
    if (!std::isfinite(brush.falloff_dir.lengthSqr()))
      return false;
    if (brush.falloff_shape == FalloffShape::RoundedBox &&
        (!std::isfinite(brush.falloff_roundness) || brush.falloff_roundness < 0.0f ||
         brush.falloff_roundness > 1.0f))
      return false;
    break;
  default:
    return false;
  }
  if (brush.falloff_kind == FalloffKind::Smoothstep ||
      brush.falloff_kind == FalloffKind::Linear ||
      brush.falloff_kind == FalloffKind::Gaussian)
    return true;
  if (brush.falloff_kind != FalloffKind::Curve)
    return false;
  for (float value : brush.falloff_curve)
    if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
      return false;
  return true;
}

bool resolvedFalloffNormalSupported(const Brush &brush, float x, float y, float z)
{
  if (brush.falloff_shape != FalloffShape::Box && brush.falloff_shape != FalloffShape::RoundedBox)
    return true;
  const float lengthSquared = x * x + y * y + z * z;
  return std::isfinite(lengthSquared) && lengthSquared > 1e-20f;
}

props::ScalarRegistrationResult PreparedBrushScalars::publish(Brush &brush) const
{
  if (brush_ != &brush || definition_ != brush.props.struct_def || !definition_) {
    return {PropError::ERROR_INVALID_OWNER, "brush"};
  }
  auto result = brush.props.struct_def->registerScalars(declarations());
  if (result.error != PropError::ERROR_NONE) {
    return result;
  }
  // All recoverable checks precede this barrier. These setters only write
  // working caches, including readonly authored sources, never their values.
  ensureExtraUniformDefaults(brush);
  applyWorking(brush);
  return {};
}

void PreparedBrushScalars::applyWorking(Brush &brush) const
{
  brush.cavity_curve = cavityCurve_;
  brush.unboundedExtent = unboundedExtent_;
  for (const auto &value : values_) {
    props::numtype_dispatch(value.type, [&]<typename P>() {
      using T = typename P::value_type;
      if (value.nativeMember >= 0) {
        *static_cast<T *>(Brush::builtinPropDescriptorSpan()[value.nativeMember].address(
            brush)) = T(value.value);
      } else if constexpr (std::is_same_v<T, float>) {
        brush.setEvaluatedNamedFloat(value.storeSlot, T(value.value));
      } else if constexpr (std::is_same_v<T, int32_t>) {
        brush.setEvaluatedNamedInt(value.storeSlot, T(value.value));
      } else if constexpr (std::is_same_v<T, bool>) {
        brush.setEvaluatedNamedBool(value.storeSlot, T(value.value));
      }
    });
  }
}

props::ScalarRegistrationResult
prepareBrushScalars(Brush &brush,
                    std::span<const BrushUniformManifestEntry> uniforms,
                    const props::DeviceInputCtx &inputs,
                    PreparedBrushScalars &output,
                    std::span<const BrushScalarOverride> overrides,
                    std::span<const BrushUniformManifestEntry> programUniforms,
                    ScalarSource source,
                    std::span<const BrushDynamicsOverride> dynamicsOverrides,
                    std::span<const float> cavityCurveOverride)
{
  if (!brush.props.struct_def)
    return {PropError::ERROR_INVALID_OWNER, "brush"};
  PreparedBrushScalars candidate;
  candidate.brush_ = &brush;
  candidate.definition_ = brush.props.struct_def;
  candidate.unboundedExtent_ = brush.unboundedExtent;
  static_assert(Brush::kCavityCurveLutSize == 256);
  if (!cavityCurveOverride.empty() && cavityCurveOverride.size() != 256)
    return {PropError::ERROR_INVALID_VALUE, "cavity curve"};
  for (size_t i = 0; i < 256; i++) {
    const float value =
        cavityCurveOverride.empty() ? brush.cavity_curve[i] : cavityCurveOverride[i];
    if (!std::isfinite(value))
      return {PropError::ERROR_INVALID_VALUE, "cavity curve"};
    candidate.cavityCurve_[i] = value;
  }
  for (size_t index = 0; index < uniforms.size(); index++) {
    const auto &entry = uniforms[index];
    for (size_t prior = 0; prior < index; prior++) {
      if (entry.name == uniforms[prior].name)
        return {PropError::ERROR_SCHEMA_CONFLICT, entry.name};
    }
    auto error = validateTarget(entry);
    if (error != PropError::ERROR_NONE)
      return {error, entry.name};
    if (executorSetting(entry.name)) {
      for (const auto &setting : executorSettingDeclarations()) {
        if (setting.name != entry.name)
          continue;
        if (entry.scalarType != setting.type || entry.dynamic || entry.hasDefault ||
            (entry.hasRange &&
             (entry.rangeMin != setting.rangeMin || entry.rangeMax != setting.rangeMax)))
          return {PropError::ERROR_SCHEMA_CONFLICT, entry.name};
      }
    } else if (scalar(entry.scalarType)) {
      candidate.declarations_.append(declarationOf(entry));
    }
  }
  auto validation =
      brush.props.struct_def->validateScalarDeclarations(candidate.declarations());
  if (validation.error != PropError::ERROR_NONE)
    return validation;

  auto append =
      [&](PreparedBrushScalar value, const ScalarDeclaration *declaration, bool common) {
        const BrushScalarOverride *override = nullptr;
        for (const auto &item : overrides) {
          if (item.name == value.name) {
            if (override)
              return PropError::ERROR_SCHEMA_CONFLICT;
            override = &item;
          }
        }
        const BrushDynamicsOverride *stackOverride = nullptr;
        for (const auto &item : dynamicsOverrides) {
          if (item.name == value.name) {
            if (stackOverride)
              return PropError::ERROR_SCHEMA_CONFLICT;
            if (item.type != value.type)
              return PropError::ERROR_INVALID_TYPE;
            if (!value.dynamic || source != ScalarSource::Authored ||
                !props::Dynamics::validStack(item.dynamics.devices))
              return PropError::ERROR_INVALID_DYNAMICS;
            stackOverride = &item;
          }
        }
        auto error = prepareValue(
            brush, value, declaration, common, inputs, override, source, stackOverride);
        if (error == PropError::ERROR_NONE)
          candidate.values_.append(std::move(value));
        return error;
      };
  for (int id = 0; id <= int(BrushProp::Invert); id++) {
    util::string name(brushPropName(id));
    ScalarDeclaration declaration;
    bool declared = false;
    if (auto *entry = findUniform(uniforms, name)) {
      declaration = declarationOf(*entry);
      declared = true;
    } else if (auto *entry = findUniform(programUniforms, name)) {
      declaration = declarationOf(*entry);
      declared = true;
    } else {
      declared = brush.props.struct_def->scalarDeclaration(name, declaration);
    }
    const Prop type = id == int(BrushProp::Invert) ? Prop::BOOL : Prop::FLOAT32;
    if (declared && declaration.type != type)
      return {PropError::ERROR_INVALID_TYPE, name};
    auto error =
        append({name, type, 0, !declared || declaration.dynamic, nativeMember(name), -1},
               declared ? &declaration : nullptr,
               true);
    if (error != PropError::ERROR_NONE)
      return {error, name};
  }
  for (const auto &setting : executorSettingDeclarations()) {
    if ((setting.name == util::string("enhance_rings") ||
         setting.name == util::string("enhance_inner")) &&
        !findUniform(uniforms, setting.name))
      continue;
    auto error =
        append({setting.name, setting.type, 0, false, nativeMember(setting.name), -1},
               &setting,
               false);
    if (error != PropError::ERROR_NONE)
      return {error, setting.name};
  }
  for (const auto &entry : uniforms) {
    if (!scalar(entry.scalarType) || commonMember(entry.name) >= 0 ||
        executorSetting(entry.name))
      continue;
    auto declaration = declarationOf(entry);
    auto error = append({entry.name,
                         entry.scalarType,
                         0,
                         entry.dynamic,
                         nativeMember(entry.name),
                         entry.storeSlot},
                        &declaration,
                        false);
    if (error != PropError::ERROR_NONE)
      return {error, entry.name};
  }
  for (const auto &override : overrides) {
    bool found = false;
    for (const auto &value : candidate.values_) {
      found |= value.name == override.name;
    }
    if (!found)
      return {PropError::ERROR_NOT_EXISTS, override.name};
  }
  for (const auto &item : dynamicsOverrides) {
    bool found = false;
    for (const auto &value : candidate.values_)
      found |= value.name == item.name;
    if (!found)
      return {PropError::ERROR_NOT_EXISTS, item.name};
  }
  for (auto *property : brush.props.struct_def->properties()) {
    auto *dynamics = dynamicsOf(property);
    if (!dynamics || dynamics->devices.size() == 0)
      continue;
    bool eligible = false;
    bool active = false;
    for (const auto &value : candidate.values_) {
      if (value.name == property->name && value.type == property->type &&
          brush.props.struct_def->lookupLocal(value.name) == property)
      {
        eligible = value.dynamic;
        active = true;
        break;
      }
    }
    if (!active) {
      bool belongsToProgram = false;
      for (const auto &entry : programUniforms) {
        if (entry.name == property->name && entry.scalarType == property->type &&
            brush.props.struct_def->lookupLocal(entry.name) == property)
        {
          belongsToProgram = true;
          break;
        }
      }
      // Every owning stage is prepared before execution; report its own errors there.
      if (belongsToProgram)
        continue;
    }
    if (!eligible || !props::Dynamics::validStack(dynamics->devices)) {
      return {PropError::ERROR_INVALID_DYNAMICS, property->name};
    }
  }
  for (const auto &value : candidate.values_)
    if (value.name == util::string("unboundedExtent"))
      candidate.unboundedExtent_ = float(value.value);
  output = std::move(candidate);
  return {};
}

props::ScalarRegistrationResult
validateUnboundedSupport(const PreparedBrushScalars &stage, float radius)
{
  using props::PropError;
  if (!named_uniform_detail::supportedFloat(radius) || !std::isfinite(radius) ||
      radius < 0 || (radius > 0 && !std::isfinite(1.0f / radius)))
    return {PropError::ERROR_INVALID_VALUE, "radius"};
  if (radius == 0)
    return {};
  const Brush *brush = stage.sourceBrush();
  if (!brush)
    return {PropError::ERROR_INVALID_OWNER, "brush"};
  for (int axis = 0; axis < 3; axis++)
    if (!named_uniform_detail::supportedFloat(brush->grabFrom[axis]) ||
        !named_uniform_detail::supportedFloat(brush->grabTo[axis]) ||
        !std::isfinite(brush->grabFrom[axis]) || !std::isfinite(brush->grabTo[axis]))
      return {PropError::ERROR_INVALID_VALUE, "unbounded frame"};
  const float extent = stage.unboundedExtent();
  if (!named_uniform_detail::supportedFloat(extent) || !std::isfinite(extent))
    return {PropError::ERROR_INVALID_VALUE, "unboundedExtent"};
  if (extent <= 0)
    return {};
  const float cutoff = radius * extent;
  const float width = 0.2f * cutoff;
  if (!std::isfinite(cutoff) || cutoff <= 0 || !std::isfinite(width) || width <= 0)
    return {PropError::ERROR_INVALID_VALUE, "unbounded cutoff"};
  return {};
}
} // namespace sculptcore::brush
