#include "cmath"
#include "math/vector.h"

namespace sculptcore::math {
template <typename T> struct Quat : public Vec<T, 4> {
  Quat()
  {
  }

  Quat(T *data)
  {
    this[0] = data[0];
    this[1] = data[1];
    this[2] = data[2];
    this[3] = data[3];
  }

  operator T *()
  {
    return &this[0];
  }
};
}; // namespace sculptcore::math
