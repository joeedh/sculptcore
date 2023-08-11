#include "math/matrix.h"
#include "test_util.h"
#include "util/function.h"
#include "util/rand.h"
#include <cstdio>

test_init;

using namespace sculptcore;

int test(util::function_ref<int(int)> cb)
{
  return cb(-1);
}

int main()
{

  {
    test([](int val) { return val + 1; });
  }

  return test_end();
}
