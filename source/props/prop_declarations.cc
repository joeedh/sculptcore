#include "prop_coerce.h"
#include "prop_struct.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sculptcore::props {

bool ScalarDeclaration::compatible(const ScalarDeclaration &other) const
{
  return name == other.name && type == other.type && hasDefault == other.hasDefault &&
         (!hasDefault || defaultValue == other.defaultValue) &&
         hasRange == other.hasRange &&
         (!hasRange || (rangeMin == other.rangeMin && rangeMax == other.rangeMax)) &&
         dynamic == other.dynamic;
}

PropError normalizeScalarDeclaration(const ScalarDeclaration &declaration,
                                     ScalarDomain &bounds)
{
  if (declaration.type != Prop::FLOAT32 && declaration.type != Prop::INT32 &&
      declaration.type != Prop::BOOL)
  {
    return PropError::ERROR_INVALID_TYPE;
  }
  if (declaration.name.size() == 0 ||
      (declaration.hasDefault && !std::isfinite(declaration.defaultValue)) ||
      (declaration.hasRange &&
       (!std::isfinite(declaration.rangeMin) || !std::isfinite(declaration.rangeMax) ||
        declaration.rangeMin > declaration.rangeMax)))
  {
    return PropError::ERROR_INVALID_VALUE;
  }
  PropError result = PropError::ERROR_INVALID_VALUE;
  numtype_dispatch(declaration.type, [&]<typename P>() {
    using T = typename P::value_type;
    const double storageMin = double(std::numeric_limits<T>::lowest());
    const double storageMax = double(std::numeric_limits<T>::max());
    double min =
        declaration.hasRange ? std::max(storageMin, declaration.rangeMin) : storageMin;
    double max =
        declaration.hasRange ? std::min(storageMax, declaration.rangeMax) : storageMax;
    if (min > max) {
      return;
    }
    if constexpr (std::is_same_v<T, float>) {
      float lower = float(min), upper = float(max);
      if (double(lower) < min) {
        lower = std::nextafter(lower, std::numeric_limits<float>::infinity());
      }
      if (double(upper) > max) {
        upper = std::nextafter(upper, -std::numeric_limits<float>::infinity());
      }
      min = double(lower);
      max = double(upper);
    } else {
      min = std::ceil(min);
      max = std::floor(max);
    }
    if (min > max) {
      return;
    }
    double initial = 0;
    if (declaration.hasDefault) {
      const double value = declaration.defaultValue;
      if (value < storageMin || value > storageMax) {
        return;
      }
      if constexpr (std::is_integral_v<T>) {
        if (std::trunc(value) != value) {
          return;
        }
      }
      initial = double(T(value));
      if (initial < min || initial > max) {
        return;
      }
    }
    bounds = {min, max, initial};
    result = PropError::ERROR_NONE;
  });
  return result;
}

namespace {

bool matchingStorage(Property *property,
                     const ScalarDeclaration &declaration,
                     const ScalarDomain &bounds)
{
  if (property->type != declaration.type) {
    return false;
  }
  bool matches = false;
  numtype_dispatch(property->type, [&]<typename P>() {
    auto *typed = static_cast<P *>(static_cast<detail::PropBaseType *>(property));
    matches = double(typed->min) == bounds.min && double(typed->max) == bounds.max;
  });
  return matches;
}

} // namespace

PropError validateScalarDeclaration(const ScalarDeclaration &declaration)
{
  ScalarDomain bounds;
  return normalizeScalarDeclaration(declaration, bounds);
}

bool StructDef::scalarDeclaration(util::string name, ScalarDeclaration &out)
{
  for (StructDef *current = this; current; current = current->parent) {
    if (const auto *declaration = current->scalarDeclarations_.lookup_ptr(name)) {
      out = *declaration;
      return true;
    }
    if (current->lookupLocal(name)) {
      break;
    }
  }
  return false;
}

PropError StructDef::validateScalarSchema(util::string name)
{
  Property *property = lookup(name);
  const ScalarDeclaration *first = nullptr;
  for (StructDef *current = this; current; current = current->parent) {
    if (const auto *declaration = current->scalarDeclarations_.lookup_ptr(name)) {
      ScalarDomain bounds;
      if (!property ||
          normalizeScalarDeclaration(*declaration, bounds) != PropError::ERROR_NONE ||
          !matchingStorage(property, *declaration, bounds) ||
          (first && !first->compatible(*declaration)))
      {
        return PropError::ERROR_SCHEMA_CONFLICT;
      }
      first = declaration;
    }
    if (current->lookupLocal(name)) {
      break;
    }
  }
  return PropError::ERROR_NONE;
}

ScalarRegistrationResult
StructDef::validateScalarDeclarations(std::span<const ScalarDeclaration> declarations)
{
  for (size_t index = 0; index < declarations.size(); index++) {
    const auto &declaration = declarations[index];
    ScalarDomain bounds;
    PropError error = normalizeScalarDeclaration(declaration, bounds);
    if (error != PropError::ERROR_NONE) {
      return {error, declaration.name};
    }
    for (size_t previous = 0; previous < index; previous++) {
      if (declarations[previous].name == declaration.name &&
          !declaration.compatible(declarations[previous]))
      {
        return {PropError::ERROR_SCHEMA_CONFLICT, declaration.name};
      }
    }
    for (StructDef *current = this; current; current = current->parent) {
      const auto *existing = current->scalarDeclarations_.lookup_ptr(declaration.name);
      if (existing && !declaration.compatible(*existing)) {
        return {PropError::ERROR_SCHEMA_CONFLICT, declaration.name};
      }
      if (current->lookupLocal(declaration.name)) {
        break;
      }
    }
    if (Property *property = lookup(declaration.name)) {
      if (!matchingStorage(property, declaration, bounds)) {
        return {PropError::ERROR_SCHEMA_CONFLICT, declaration.name};
      }
    } else if (bounds.initial < bounds.min || bounds.initial > bounds.max) {
      return {PropError::ERROR_INVALID_VALUE, declaration.name};
    }
  }

  return {};
}

ScalarRegistrationResult
StructDef::registerScalars(std::span<const ScalarDeclaration> declarations)
{
  // Preflight does not reserve state: raw edits may occur before publication.
  auto validation = validateScalarDeclarations(declarations);
  if (validation.error != PropError::ERROR_NONE) {
    return validation;
  }
  for (const auto &declaration : declarations) {
    if (scalarDeclarations_.lookup_ptr(declaration.name)) {
      continue;
    }
    if (!lookup(declaration.name)) {
      ScalarDomain bounds;
      normalizeScalarDeclaration(declaration, bounds);
      auto initialize = [&]<typename P>(P &property) {
        using T = typename P::value_type;
        property.Min(T(bounds.min)).Max(T(bounds.max)).Default(T(bounds.initial));
      };
      switch (declaration.type) {
      case Prop::FLOAT32:
        initialize(Float32(declaration.name, declaration.name, -1));
        break;
      case Prop::INT32:
        initialize(Int32(declaration.name, declaration.name, -1));
        break;
      case Prop::BOOL:
        initialize(Bool(declaration.name, declaration.name, -1));
        break;
      default:
        break;
      }
    }
    scalarDeclarations_[declaration.name] = declaration;
  }
  return {};
}

} // namespace sculptcore::props
