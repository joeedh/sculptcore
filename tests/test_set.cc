#include "test_util.h"
#include "litestl/util/rand.h"
#include "litestl/util/set.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"
#include <cstdio>

test_init;

int main()
{
  using namespace litestl::util;

  {
    static constexpr int size = 512;

    Set<int, size + 10> set;
    Random rand;

    Vector<int> keys;

    for (int i = 0; i < size; i++) {
      int key = rand.get_int();

      set.add(key);
      keys.append(key);

      test_assert(set.contains(key));
    }

    for (int key : keys) {
      test_assert(set.contains(key));
    }

    for (int key : set) {
      test_assert(keys.contains(key));
    }

    const char *strings[] = {"sdfdsf", "bleh", "yay", "space123324", "hahah", "gagaga"};

    Set<string> strset;

    for (int i = 0; i < array_size(strings); i++) {
      strset.add(strings[i]);
    }

    test_assert(!strset["dsfdsf"]);

    for (const string &str : strset) {
      test_assert(strset[str]);
    }
  }

  return test_end();
}
