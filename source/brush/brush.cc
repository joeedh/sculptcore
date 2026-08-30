#include "brush/props.h"

#include "litestl/util/function.h"
#include "props/prop_struct.h"

using litestl::util::function_ref;

namespace sculptcore::brush {
struct BrushCommand {
  props::StructProp props;
};
} // namespace sculptcore::brush