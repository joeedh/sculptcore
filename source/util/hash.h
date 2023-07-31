#include "compiler_util.h"

namespace sculptcore::hash {
using HashInt = uint64_t;

template <typename T> inline HashInt hash(T *ptr)
{
  return reinterpret_cast<HashInt>(ptr);
}
inline HashInt hash(int i)
{
  return HashInt(i);
}

} // namespace sculptcore::hash
