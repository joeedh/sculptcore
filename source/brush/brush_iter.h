#include "math/vector.h"

using namespace sculptcore::math;

namespace sculptcore::brush {
struct BrushIterFull {
  float2 &co;
  float2 &no;
  float &mask;

  BrushIterFull(float2 &co_, float2 &no_, float &mask_) : co(co_), no(no_), mask(mask_)
  {
  }
}
} // namespace sculptcore::brush
