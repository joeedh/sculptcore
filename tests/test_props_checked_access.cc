#include "props/prop_struct.h"
#include "test_util.h"

#include <limits>

test_init;
using namespace sculptcore::props;

int main()
{
  {
    const auto ok = PropError::ERROR_NONE;
    const auto invalid = PropError::ERROR_INVALID_VALUE;
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    StructDef parent, child;
    parent.Int32("inherited", "Inherited", -1).Default(17);
    child.parent = &parent;
    auto &integer = child.Int32("integer", "Integer", -1).Default(3);
    auto &boolean = child.Bool("boolean", "Boolean", -1).Default(false);
    auto &floating =
        child.Float32("floating", "Floating", -1).Default(0.5f).Min(-1).Max(1);
    StructProp props(&child);
    double value = -999;
    test_assert(props.readScalar("inherited", Prop::INT32, value) == ok && value == 17);
    test_assert(props.setScalarLocal("inherited", Prop::INT32, 99) ==
                PropError::ERROR_NOT_EXISTS);
    test_assert(child.lookupLocal("inherited") == nullptr);
    test_assert(props.readScalar("inherited", Prop::INT32, value) == ok && value == 17);
    test_assert(props.setScalarLocal("integer", Prop::BOOL, 1) ==
                PropError::ERROR_INVALID_TYPE);
    test_assert(props.setScalarLocal("integer", Prop::INT32, 16777217) == ok);
    test_assert(integer.get() == 16777217);
    for (double rejected : {2147483648.0, -2147483649.0, 0.5, inf, nan}) {
      test_assert(props.setScalarLocal("integer", Prop::INT32, rejected) == invalid);
      test_assert(integer.get() == 16777217);
    }
    test_assert(props.setScalarLocal("integer", Prop::INT32, -2147483648.0) == ok);
    test_assert(props.readScalar("integer", Prop::INT32, value) == ok &&
                value == -2147483648.0);
    test_assert(props.setScalarLocal("boolean", Prop::BOOL, 1) == ok && boolean.get());
    for (double rejected : {-1.0, 2.0, 0.5, inf, nan}) {
      test_assert(props.setScalarLocal("boolean", Prop::BOOL, rejected) == invalid);
      test_assert(boolean.get());
    }
    test_assert(props.setScalarLocal("floating", Prop::FLOAT32, 0.375) == ok);
    test_assert(props.setScalarLocal("floating", Prop::FLOAT32, 2) == invalid);
    test_assert(floating.get() == 0.375f);
    floating.flag = PropFlag::READ_ONLY;
    test_assert(props.setScalarLocal("floating", Prop::FLOAT32, 0.5) ==
                PropError::ERROR_READ_ONLY);
    test_assert(floating.get() == 0.375f);
    value = 99;
    test_assert(props.readScalar("integer", Prop::FLOAT32, value) ==
                PropError::ERROR_INVALID_TYPE);
    test_assert(value == 99);
    floating.Default(nan);
    test_assert(props.readScalar("floating", Prop::FLOAT32, value) == invalid &&
                value == 99);
    floating.Default(0.5f).Min(1).Max(-1);
    test_assert(props.readScalar("floating", Prop::FLOAT32, value) == invalid &&
                value == 99);
    child.Float64("unsupported", "Unsupported", -1).Default(1);
    test_assert(props.setScalarLocal("unsupported", Prop::FLOAT64, 1) ==
                PropError::ERROR_INVALID_TYPE);
    test_assert(props.readScalar("unsupported", Prop::FLOAT64, value) ==
                PropError::ERROR_INVALID_TYPE);

    integer.Default(3);
    integer.dynamics.configure(
        DeviceType::PRESSURE, litestl::math::BasicMix::MULTIPLY, 1);
    DeviceInputCtx ctx;
    ctx.push(0, 0.5f);
    test_assert(props.evaluateScalar("integer", Prop::INT32, ctx, value) == ok &&
                value == 2);
    test_assert(integer.get() == 3);
    integer.dynamics.devices[0].mixFactor = -1;
    value = 99;
    test_assert(props.evaluateScalar("integer", Prop::INT32, ctx, value) ==
                PropError::ERROR_INVALID_DYNAMICS);
    test_assert(value == 99 && integer.get() == 3);

    struct Owner {
      int value = 5;
    } owner;
    auto &bound = child.Int32("bound", "Bound", offsetof(Owner, value)).Min(0).Max(10);
    int calls = 0;
    bound.setter = [&](int *target, void *who, int &replacement) {
      test_assert(who == &owner);
      calls++;
      *target = replacement;
    };
    props.Owner(&owner);
    test_assert(props.setScalarLocal("bound", Prop::INT32, 7) == ok);
    test_assert(owner.value == 7 && calls == 1);
    test_assert(props.setScalarLocal("bound", Prop::INT32, 11) == invalid);
    test_assert(owner.value == 7 && calls == 1);
    test_assert(props.readScalar("bound", Prop::INT32, value) == ok && value == 7);
    Owner parentOwner{17};
    auto &inheritedBound =
        parent.Int32("parent_bound", "Parent Bound", offsetof(Owner, value));
    inheritedBound.Owner(&parentOwner);
    value = 99;
    test_assert(props.readScalar("parent_bound", Prop::INT32, value) ==
                PropError::ERROR_INVALID_OWNER);
    test_assert(props.evaluateScalar("parent_bound", Prop::INT32, ctx, value) ==
                PropError::ERROR_INVALID_OWNER);
    test_assert(value == 99 && inheritedBound.owner == &parentOwner &&
                parentOwner.value == 17);
    int parentReads = 0;
    auto &inheritedGetter =
        parent.Int32("parent_getter", "Parent Getter", -1).Default(17);
    inheritedGetter.Owner(&parentOwner);
    inheritedGetter.getter = [&](int *target, void *who) {
      test_assert(who == &parentOwner);
      parentReads++;
      return target;
    };
    test_assert(props.readScalar("parent_getter", Prop::INT32, value) ==
                PropError::ERROR_INVALID_OWNER);
    test_assert(props.evaluateScalar("parent_getter", Prop::INT32, ctx, value) ==
                PropError::ERROR_INVALID_OWNER);
    test_assert(value == 99 && parentReads == 0 && inheritedGetter.owner == &parentOwner);
    props.Owner(static_cast<Owner *>(nullptr));
    test_assert(props.readScalar("parent_bound", Prop::INT32, value) ==
                PropError::ERROR_INVALID_OWNER);
    test_assert(value == 99 && inheritedBound.owner == &parentOwner);
    StructProp absent;
    test_assert(absent.setScalarLocal("missing", Prop::INT32, 1) ==
                PropError::ERROR_NOT_EXISTS);
    test_assert(absent.readScalar("missing", Prop::INT32, value) ==
                PropError::ERROR_NOT_EXISTS);
  }
  return test_end();
}
