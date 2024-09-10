#include "brush/props.h"

#include "props/prop_struct.h"
#include "litestl/util/function.h"

using litestl::util::function_ref;

namespace sculptcore::brush {
struct BrushCommand {
  props::StructProp props;
};
} // namespace sculptcore::brush