#include "test_util.h"
#include "util/alloc.h"
#include "util/rand.h"
#include "util/set.h"
#include "util/string.h"
#include "util/vector.h"
#include <cstdio>

#include "util/memory.h"

test_init;

struct Test {
  Test()
  {
    printf("Allocate Test\n");
  }

  ~Test()
  {
    printf("Deallocate Test\n");
  }
};

int main()
{
  using namespace sculptcore;
  using sculptcore::util::shared_ptr;
  using sculptcore::util::weak_ptr;

  weak_ptr<Test> weak;

  {
    Test *test = alloc::New<Test>("Test");

    shared_ptr<Test> ptr{test};
    shared_ptr<Test> ptr2 = ptr;

    weak = ptr2;

    printf("exists1: %d\n", int(!weak.expired()));
    test_assert(!weak.expired());
  }

  test_assert(weak.expired());
  printf("exists2: %d\n", int(!weak.expired()));

  return test_end();
}
