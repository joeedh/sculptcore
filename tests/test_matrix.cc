#include "test_util.h"
#include "util/rand.h"
#include "util/set.h"
#include "util/string.h"
#include "util/vector.h"
#include "math/matrix.h"
#include <cstdio>

test_init;

int main()
{
  using namespace sculptcore::math;

  {
    mat4 m4;
    mat3 m3;
    float3 v;

    m4.identity();
    m3.identity();

    v = m4 * float3(1.0f);
  }

  return test_end();
}
