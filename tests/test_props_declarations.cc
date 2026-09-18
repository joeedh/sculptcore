#include "props/prop_struct.h"
#include "test_util.h"

#include <cmath>
#include <limits>

test_init;
using namespace sculptcore::props;

static constexpr auto ok = PropError::ERROR_NONE;
static constexpr auto conflict = PropError::ERROR_SCHEMA_CONFLICT;

static ScalarDeclaration integer(const char *name = "count")
{
  return {name, Prop::INT32, true, 16777217, true, -2147483648.0, 2147483647.0, true};
}

static PropError add(StructDef &definition, const ScalarDeclaration &declaration)
{
  return definition.registerScalars({&declaration, 1}).error;
}

static void valuesAndBounds()
{
  StructDef definition;
  ScalarDeclaration declarations[] = {
      integer(),
      {"enabled", Prop::BOOL, true, 1, false, 0, 0, true},
      {"gain", Prop::FLOAT32, true, 0.1, true, 0.1, 0.5, true}};
  test_assert(definition.registerScalars(declarations).error == ok);
  StructProp properties(&definition);
  int sentinel = 991;
  properties.Owner(&sentinel);
  double value = 0;
  test_assert(properties.readScalar("count", Prop::INT32, value) == ok &&
              value == 16777217);
  test_assert(properties.setScalarLocal("count", Prop::INT32, -2147483648.0) == ok);
  test_assert(properties.readScalar("count", Prop::INT32, value) == ok &&
              value == -2147483648.0);
  test_assert(properties.setScalarLocal("count", Prop::INT32, 2147483647.0) == ok);
  test_assert(properties.setScalarLocal("count", Prop::INT32, 2147483648.0) ==
              PropError::ERROR_INVALID_VALUE);
  test_assert(sentinel == 991);
  test_assert(properties.readScalar("enabled", Prop::BOOL, value) == ok && value == 1);
  test_assert(properties.setScalarLocal("enabled", Prop::BOOL, 0.5) ==
              PropError::ERROR_INVALID_VALUE);
  test_assert(properties.setScalarLocal("gain", Prop::FLOAT32, 0.1) == ok);
  test_assert(properties.readScalar("gain", Prop::FLOAT32, value) == ok &&
              value == double(0.1f));
  test_assert(properties.setScalarLocal("gain", Prop::FLOAT32, 0.6) ==
              PropError::ERROR_INVALID_VALUE);
  auto *pointer = definition.lookupLocal("count");
  test_assert(definition.registerScalars(declarations).error == ok);
  test_assert(pointer == definition.lookupLocal("count"));
  test_assert(properties.readScalar("count", Prop::INT32, value) == ok &&
              value == 2147483647.0);
  declarations[0].name = "destroyed_input";
  declarations[0].defaultValue = -99;
  ScalarDeclaration retained;
  test_assert(definition.scalarDeclaration("count", retained) &&
              retained.defaultValue == 16777217);
  retained.defaultValue = 99;
  test_assert(definition.scalarDeclaration("count", retained) &&
              retained.defaultValue == 16777217);

  ScalarDeclaration fractional{"fractional", Prop::INT32, true, 2, true, 0.1, 2.9, true};
  test_assert(add(definition, fractional) == ok);
  test_assert(properties.setScalarLocal("fractional", Prop::INT32, 0) ==
              PropError::ERROR_INVALID_VALUE);
  test_assert(properties.setScalarLocal("fractional", Prop::INT32, 1) == ok);
  ScalarDeclaration trueOnly{"true_only", Prop::BOOL, true, 1, true, 0.1, 1.9, true};
  test_assert(add(definition, trueOnly) == ok);
  test_assert(properties.setScalarLocal("true_only", Prop::BOOL, 0) ==
              PropError::ERROR_INVALID_VALUE);
  ScalarDeclaration underflow{"tiny", Prop::FLOAT32, true, 1e-100, false, 0, 0, true};
  test_assert(add(definition, underflow) == ok);
  test_assert(properties.readScalar("tiny", Prop::FLOAT32, value) == ok && value == 0);
}

static void invalidBatches()
{
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  ScalarDeclaration rejected[] = {
      {"", Prop::INT32},
      {"bad", Prop::FLOAT64},
      {"bad", Prop::INT32, true, 2147483648.0},
      {"bad", Prop::INT32, true, -2147483649.0},
      {"bad", Prop::INT32, true, 0.1},
      {"bad", Prop::BOOL, true, 2},
      {"bad", Prop::BOOL, true, -1},
      {"bad", Prop::BOOL, true, 0.5},
      {"bad", Prop::FLOAT32, true, inf},
      {"bad", Prop::FLOAT32, true, nan},
      {"bad", Prop::FLOAT32, true, 1e100},
      {"bad", Prop::FLOAT32, false, 0, true, 1, -1},
      {"bad", Prop::FLOAT32, false, 0, true, nan, 1},
      {"bad", Prop::FLOAT32, false, 0, true, 0, inf},
      {"bad", Prop::FLOAT32, true, 0.1, true, 0.1, 0.1},
      {"bad", Prop::FLOAT32, true, 1, true, 1.00000001, 1.00000002},
      {"bad", Prop::INT32, true, 0, true, 0.1, 0.9},
      {"bad", Prop::BOOL, true, 0, true, 0.1, 0.9},
      {"bad", Prop::BOOL, true, 1, true, 2, 3},
      {"bad", Prop::INT32, false, 0, true, 5, 10},
  };
  for (const auto &bad : rejected) {
    StructDef definition;
    ScalarDeclaration batch[] = {integer("would_create"), bad};
    auto preflight = definition.validateScalarDeclarations(batch);
    auto publication = definition.registerScalars(batch);
    test_assert(preflight.error != ok && preflight.error == publication.error &&
                preflight.name == publication.name);
    test_assert(!definition.lookupLocal("would_create"));
    ScalarDeclaration retained;
    test_assert(!definition.scalarDeclaration("would_create", retained));
  }
  for (int field = 0; field < 6; field++) {
    ScalarDeclaration a = integer(), b = a;
    switch (field) {
    case 0:
      b.type = Prop::FLOAT32;
      b.defaultValue = 1;
      break;
    case 1:
      b.hasDefault = false;
      break;
    case 2:
      b.defaultValue = 16777218;
      break;
    case 3:
      b.hasRange = false;
      break;
    case 4:
      b.rangeMin = -99;
      break;
    case 5:
      b.dynamic = false;
      break;
    }
    for (bool reverse : {false, true}) {
      StructDef definition;
      test_assert(add(definition, reverse ? b : a) == ok);
      ScalarDeclaration batch[] = {integer("would_create"), reverse ? a : b};
      auto result = definition.registerScalars(batch);
      test_assert(result.error == conflict &&
                  result.name == litestl::util::string("count"));
      test_assert(!definition.lookupLocal("would_create"));
    }
  }
  for (bool reverse : {false, true}) {
    StructDef definition;
    ScalarDeclaration a{"same_bounds", Prop::INT32, true, 1, true, 0.1, 2.9, true};
    auto b = a;
    b.rangeMin = 0.2;
    b.rangeMax = 2.8;
    test_assert(add(definition, reverse ? b : a) == ok);
    test_assert(add(definition, reverse ? a : b) == conflict);
  }
  StructDef definition;
  auto a = integer(), b = a;
  b.dynamic = false;
  ScalarDeclaration duplicate[] = {a, b};
  test_assert(definition.registerScalars(duplicate).error == conflict);
  test_assert(!definition.lookupLocal("count"));
  duplicate[1] = a;
  test_assert(definition.registerScalars(duplicate).error == ok);
}

static void adoptionAndInheritance()
{
  StructDef parent, middle, child;
  middle.parent = &parent;
  child.parent = &middle;
  auto declaration = integer();
  test_assert(add(parent, declaration) == ok);
  auto *property = parent.lookupLocal("count");
  StructProp properties(&child);
  double value = -99;
  test_assert(add(child, declaration) == ok && !child.lookupLocal("count"));
  test_assert(properties.readScalar("count", Prop::INT32, value) == ok &&
              value == 16777217);
  test_assert(properties.setScalarLocal("count", Prop::INT32, 3) ==
              PropError::ERROR_NOT_EXISTS);
  test_assert(parent.lookupLocal("count") == property);
  auto changed = declaration;
  changed.dynamic = false;
  test_assert(add(middle, changed) == conflict);

  StructDef replacement;
  test_assert(add(replacement, changed) == ok);
  middle.parent = &replacement;
  test_assert(properties.readScalar("count", Prop::INT32, value) == conflict &&
              value == 16777217);
  test_assert(add(child, declaration) == conflict);
  middle.parent = &parent;

  int calls = 0, owner = 993;
  auto &local = middle.Int32("count", "Count", -1).Default(73);
  local.Owner(&owner);
  local.getter = [&](int *existing, void *) {
    calls++;
    return existing;
  };
  local.dynamics.configure(DeviceType::PRESSURE, litestl::math::BasicMix::MULTIPLY, 0.25);
  auto *devices = local.dynamics.devices.data();
  ScalarDeclaration retained;
  test_assert(!middle.scalarDeclaration("count", retained));
  ScalarDeclaration batch[] = {changed, {"bad", Prop::BOOL, true, 2}};
  test_assert(middle.registerScalars(batch).error != ok);
  test_assert(!middle.scalarDeclaration("count", retained));
  test_assert(add(middle, changed) == ok);
  test_assert(calls == 0 && local.owner == &owner && *local.internal_value() == 73);
  test_assert(local.dynamics.devices.data() == devices &&
              local.dynamics.devices.size() == 1);
  test_assert(local.dynamics.devices[0].mixFactor == 0.25f);
  test_assert(add(child, declaration) == conflict);
  test_assert(parent.lookupLocal("count") == property);

  StructDef adopted;
  auto &gain = adopted.Float32("gain", "Gain", -1).Default(0.75f);
  ScalarDeclaration noDefault{"gain", Prop::FLOAT32, false, 0, false, 0, 0, true};
  test_assert(add(adopted, noDefault) == ok && gain.get() == 0.75f);
  gain.Max(1);
  StructProp adoptedProps(&adopted);
  value = 99;
  test_assert(adoptedProps.readScalar("gain", Prop::FLOAT32, value) == conflict &&
              value == 99);
  test_assert(adoptedProps.setScalarLocal("gain", Prop::FLOAT32, 0.5) == conflict);
  test_assert(add(adopted, noDefault) == conflict && gain.get() == 0.75f);
  auto *old = adopted.lookupLocal("gain");
  adopted.Bool("gain", "Replaced", -1);
  litestl::alloc::Delete(old);
  test_assert(add(adopted, noDefault) == conflict);
  test_assert(adoptedProps.readScalar("gain", Prop::BOOL, value) == conflict);
}

static void preflightWithoutPublication()
{
  StructDef parent, child;
  child.parent = &parent;
  int owner = 913, calls = 0;
  auto &value = parent.Int32("count", "Count", -1).Default(73);
  value.Owner(&owner);
  value.getter = [&](int *existing, void *) {
    calls++;
    return existing;
  };
  value.dynamics.configure(DeviceType::PRESSURE, litestl::math::BasicMix::MULTIPLY, 0.25);
  auto *devices = value.dynamics.devices.data();
  ScalarDeclaration batch[] = {integer(), integer("pending")};
  ScalarDeclaration retained;
  test_assert(child.validateScalarDeclarations({}).error == ok);
  test_assert(child.validateScalarDeclarations(batch).error == ok);
  test_assert(!parent.scalarDeclaration("count", retained));
  test_assert(!child.scalarDeclaration("count", retained));
  test_assert(!child.scalarDeclaration("pending", retained));
  test_assert(!parent.lookupLocal("pending") && !child.lookupLocal("pending"));
  test_assert(!child.lookupLocal("count") &&
              static_cast<void *>(parent.lookupLocal("count")) == &value);
  test_assert(calls == 0 && value.owner == &owner && *value.internal_value() == 73);
  test_assert(value.dynamics.devices.data() == devices &&
              value.dynamics.devices.size() == 1 &&
              value.dynamics.devices[0].mixFactor == 0.25f);

  // A later raw schema edit must invalidate publication after successful preflight.
  value.Max(100);
  auto result = child.registerScalars(batch);
  test_assert(result.error == conflict && result.name == litestl::util::string("count"));
  test_assert(!child.lookupLocal("pending") &&
              !child.scalarDeclaration("pending", retained));
  test_assert(!child.scalarDeclaration("count", retained));
  value.Max(std::numeric_limits<int>::max());
  // Preflight never retained the old dynamic flag, so a different declaration works.
  batch[0].dynamic = false;
  test_assert(child.validateScalarDeclarations(batch).error == ok);
  test_assert(child.registerScalars(batch).error == ok);
  test_assert(child.scalarDeclaration("count", retained) && !retained.dynamic);
  test_assert(child.lookupLocal("pending") && !child.lookupLocal("count"));
  test_assert(calls == 0 && value.owner == &owner && *value.internal_value() == 73);
}

static void normalizedDomains()
{
  ScalarDomain domain;
  auto declaration = integer();
  test_assert(normalizeScalarDeclaration(declaration, domain) == ok);
  test_assert(domain.min == -2147483648.0 && domain.max == 2147483647.0 &&
              domain.initial == 16777217);
  declaration.rangeMin = 0.1;
  declaration.rangeMax = 2.9;
  declaration.defaultValue = 2;
  test_assert(normalizeScalarDeclaration(declaration, domain) == ok);
  test_assert(domain.min == 1 && domain.max == 2 && domain.initial == 2);
  declaration.defaultValue = 2.5;
  test_assert(normalizeScalarDeclaration(declaration, domain) != ok);
  test_assert(domain.min == 1 && domain.max == 2 && domain.initial == 2);
  declaration = {"gain", Prop::FLOAT32, true, 0.1, true, 0.1, 0.5, true};
  test_assert(normalizeScalarDeclaration(declaration, domain) == ok);
  test_assert(domain.min == double(0.1f) && domain.max == 0.5 &&
              domain.initial == double(0.1f));
  declaration = {"enabled", Prop::BOOL, false, 0, false, 0, 0, true};
  test_assert(normalizeScalarDeclaration(declaration, domain) == ok);
  test_assert(domain.min == 0 && domain.max == 1 && domain.initial == 0);
}

int main()
{
  normalizedDomains();
  preflightWithoutPublication();
  valuesAndBounds();
  invalidBatches();
  adoptionAndInheritance();
  return test_end();
}
