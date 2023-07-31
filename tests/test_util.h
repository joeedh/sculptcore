#include "util/alloc.h"

#define test_init int retval = 0

#define test_assert(expr)                                                                \
  (!(expr) ? (retval = 0, fprintf(stderr, "%s failed\n", #expr), fflush(stderr)) : 0)

extern int retval;
static bool test_end()
{
  return retval || sculptcore::alloc::print_blocks();
}
