#include "util/map.h"
#include "util/vector.h"

extern "C" void test()
{
  using namespace sculptcore::util;

  Map<void *, int> testmap;

  testmap.insert(nullptr, 5);
}