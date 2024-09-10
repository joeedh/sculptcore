#include "litestl/util/compiler_util.h"
#include "litestl/util/string.h"

#include "props/prop_enums.h"

namespace sculptcore::brush {
using namespace litestl;

template <util::StrLiteral name_, props::Prop type_> struct StdProp {
  util::string name = name_;
  props::Prop type = type_;
};

template <util::StrLiteral name_, util::StrLiteral struct_name_> struct StdPropStruct {
  util::string name = name_;
  util::string struct_name = struct_name_;
};

using sculptcore::props::Prop;

struct StdPropsBase {
  StdProp<"strength", Prop::FLOAT32> strength;
  StdProp<"radius", Prop::FLOAT32> radius;
  StdPropStruct<"falloff", "FalloffCurve"> falloff;
};

struct StdProp2 {
  util::const_string name;
  Prop type;
}

static constexpr BaseProps[] = {
    {"strength", Prop::FLOAT32}, //

};

constexpr bool validate_prop(util::const_string name)
{
  int num = sizeof(BaseProps) / sizeof(*BaseProps);
  for (int i = 0; i < num; i++) {
    if (name == BaseProps[i].name) {
      return true;
    }
  }

  return false;
}

struct BrushProps {
  template <util::StrLiteral name, typename T> T lookup_prop()
  {
    static_assert(validate_prop(name), "Unknown brush property");
    return T(0);
  }
};

} // namespace sculptcore::brush
