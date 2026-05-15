#pragma once
#include "../props/prop_dynamics.h"
#include "../props/prop_struct.h"
#include "litestl/binding/binding.h"
#include "litestl/util/compiler_util.h"

#include "props.h"

namespace sculptcore::brush {
using litestl::util::StrLiteral;

struct Brush {
  props::StructProp props;
  props::DeviceInputCtx deviceInputCtx;

  float strength = 1;
  float radius = 1;
  bool invert = false;

  static litestl::binding::types::Struct<Brush> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<Brush> *st =
        new types::Struct<Brush>("sculptcore::brush::Brush", sizeof(Brush));

    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);

    BIND_STRUCT_MEMBER(st, strength);
    BIND_STRUCT_MEMBER(st, radius);
    BIND_STRUCT_MEMBER(st, invert);
    BIND_STRUCT_MEMBER(st, props);
    BIND_STRUCT_METHOD(st, loadProps, MARGS());
    BIND_STRUCT_METHOD(st, writeProps, MARGS());

    return st;
  }

  Brush() : props(&structDef_)
  {
    structDef_.Float32("strength", "strength");
    structDef_.Float32("radius", "radius");
    structDef_.Bool("invert", "invert");
  }

  void loadProps()
  {
    strength = props.lookupValue<float>("strength", 1.0);
    radius = props.lookupValue<float>("radius", 1.0);
    invert = props.lookupValue<bool>("invert", false);
  }

  void writeProps()
  {
    props.setValue<float>("strength", strength);
    props.setValue<float>("radius", radius);
    props.setValue<bool>("invert", invert);
  }

private:
  props::StructDef structDef_;
};
} // namespace sculptcore::brush
