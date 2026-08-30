/* Native WebGPU backend smoke / parity test.
 *
 * Drives source/webgpu's offscreen render path through wgpu-native: builds the
 * same deterministic scene the browser fixture renders, draws one frame to an
 * RGBA8 target, and writes a PNG. Asserts the render succeeded and that a
 * meaningful fraction of the image is non-background (i.e. geometry actually
 * rasterized). The PNG (build/native/tests/webgpu_native.png) is the
 * eyeball reference for cross-checking the Playwright browser golden. */

#include "test_util.h"

#include <cstdio>

test_init;

namespace sculptcore::webgpu {
bool webgpuRenderSceneToPNG(const char *path, int w, int h, int *outNonUniformPixels);
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  const int w = 512, h = 512;
  const char *path = "webgpu_native.png";
  int nonBg = 0;

  bool ok = sculptcore::webgpu::webgpuRenderSceneToPNG(path, w, h, &nonBg);
  test_assert(ok);

  // The framed spherified cube should cover part of the frame but not all of it,
  // leaving background visible at the corners. The bounds below are loose. They
  // catch the case of nothing having drawn or the whole frame being one color;
  // they do not check exact coverage.
  printf("webgpu native render: non-background pixels = %d / %d\n", nonBg, w * h);
  test_assert(nonBg > w * h / 100);      // >1% drawn
  test_assert(nonBg < w * h * 99 / 100); // background still visible

  return test_end();
}
