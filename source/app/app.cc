#include "util/vector.h"
#include "window/window.h"
#include <cstdio>

#include "gpu/opengl.h"
#include "gpu/vbo.h"

#include "app.h"

using namespace sculptcore;
using namespace sculptcore::math;

int main(int argc, const char **argv)
{
  window::Window win(float2(1200, 800));

  win.start();

  return 0;
}
