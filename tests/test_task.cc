
#include "test_util.h"
#include "util/index_range.h"
#include "util/rand.h"
#include "util/set.h"
#include "util/string.h"
#include "util/task.h"
#include "util/vector.h"
#include <cstdio>

test_init;

int main()
{
  using namespace sculptcore;
  using namespace sculptcore::util;

  {
    int size = 1 << 25;

    task::parallel_for(
        IndexRange(size),
        [](IndexRange range) {
          volatile int d = 0;

          for (int i : range) {
            d += i >> 16;
          }

          printf("%d:\n", d);
        },
        1 << 16);
  }
  return test_end();
}
