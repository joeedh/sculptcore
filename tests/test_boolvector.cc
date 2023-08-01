#include "test_util.h"
#include "util/alloc.h"
#include "util/boolvector.h"
#include <cstdio>

test_init;

int main()
{
  using namespace sculptcore::util;

  {
    volatile BoolVector<int, 32> list;

  }

  return test_end();
}
