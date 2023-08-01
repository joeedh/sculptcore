#include "math/vector.h"
#include "test_util.h"
#include <cstdio>
#include <cstring>

test_init;

int main()
{
  using namespace sculptcore::math;

  {
    float3 vec1(1.0), vec2(2.0);

    vec1 -= vec2;

    vec1.print();
    printf(" ");
    vec2.print();
    printf("\n");
  }

  return test_end();
}
