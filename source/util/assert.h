#pragma once

#include <cstdio>

/* TODO: implement assertions properly. */
namespace sculptcore::mesh {
static bool assert(bool state, const char *msg = "Assertion failed")
{
  if (!state) {
    fprintf(stderr, "Assertion failed: \"%s\"\n", msg);
    return false;
  }

  return true;
}
}; // namespace sculptcore::mesh
