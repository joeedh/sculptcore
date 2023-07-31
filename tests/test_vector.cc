#include "test_util.h"
#include "util/alloc.h"
#include "util/vector.h"
#include <cstdio>

test_init;

int main()
{
  using namespace sculptcore::util;

  {
    Vector<int, 32> list;

    const int size = 2048;

    for (int i = 0; i < size; i++) {
      list.append(i);
    }

    for (int i = 0; i < size; i++) {
      test_assert(list[i] == i);
    }

    int i = 0;
    for (int item : list) {
      test_assert(item == i);
      i++;
    }

    list.remove(5);
    list.append_once(5);
    test_assert(list.size() == size - 1);
  }

  return test_end();
}
