#pragma once

#include "compiler_util.h"
#include "string.h"

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

/** NOT cryptographically secure! */
inline HashInt hash(const char *str)
{
  const char *c = str;
  HashInt h = 0;

  /* TODO: check this. */
  for (; *c; c++) {
    h = ((h + *c) * (*c) + 23423432) & ((1 << 19) - 1);
  }

  return h;
}
inline HashInt hash(const util::string &str)
{
  return hash(str.c_str());
}

} // namespace sculptcore::hash
