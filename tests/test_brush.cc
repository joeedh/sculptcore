
#include "test_util.h"
#include <cstdio>
#include <cstring>

#include "brush/props.h"

test_init;

int main()
{
  using namespace sculptcore::brush;

#if 1
  {
    static_assert(validate_prop("strength"));
    static_assert(!validate_prop("sdfsf"));

    BrushProps bp;
    float f = bp.lookup_prop<"strength", float>();
  }
#endif

  return test_end();
}
