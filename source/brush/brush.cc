#include "brush/props.h"

#include "props/prop_struct.h"
#include "util/function.h"

using sculptcore::util::function_ref;

namespace sculptcore::brush {
struct BrushCommand {
  props::StructProp props;
};
} // namespace sculptcore::brush