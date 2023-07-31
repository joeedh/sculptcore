#include "test_util.h"
#include "util/map.h"
#include "util/rand.h"
#include "util/vector.h"
#include <cstdio>

test_init;

int main()
{
  using namespace sculptcore::util;

  {
    Map<int, int> imap;
    Random rand;

    const int size = 512;
    Vector<int> keys, values;
    
    for (int i = 0; i < size; i++) {
      int key = rand.get_int();
      int value = rand.get_int();

      imap.add(key, value);
      keys.append(key);
      values.append(value);

      test_assert(imap.contains(key));
      test_assert(imap.lookup(key) == value);
    }

    for (auto &pair : imap) {
      test_assert(keys.contains(pair.key));
      test_assert(values.contains(pair.value));
      
      keys.remove(pair.key);
    }

    test_assert(keys.size() == 0);
  }

  return test_end();
}
