#pragma once
#include "../props/prop_struct.h"
#include "props.h"
#include "../props/prop_dynamics.h"
#include "litestl/util/compiler_util.h"

namespace sculptcore::brush {
using litestl::util::StrLiteral;

struct Brush {
  props::StructProp props;
  props::DeviceInputCtx deviceInputCtx;

  Brush() : props(&structDef_)
  {
    structDef_.Float32("strength", "strength");
    structDef_.Float32("radius", "radius");
    structDef_.Bool("invert", "invert");
  }

  template <size_t N> float getFloat(StrLiteral<N> key)
  {
  }

  float strength()
  {
    return props.lookupValue<float>("strength", 1.0);
  }

  float radius()
  {
    return props.lookupValue<float>("radius", 1.0);
  }

  bool invert()
  {
    return props.lookupValue<bool>("invert", false);
  }

private:
  props::StructDef structDef_;
};
} // namespace sculptcore::brush
