#include "gl_screenshot.h"

#include "glew/GL/glew.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sculptcore::opengl {

bool captureToPNG(const char *path, int width, int height)
{
  if (width <= 0 || height <= 0) {
    return false;
  }
  size_t row_bytes = size_t(width) * 4;
  size_t total = row_bytes * size_t(height);
  unsigned char *pixels = static_cast<unsigned char *>(std::malloc(total));
  if (!pixels) {
    return false;
  }

  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

  /* glReadPixels returns bottom-up; flip in place so PNG is top-down. */
  unsigned char *tmp = static_cast<unsigned char *>(std::malloc(row_bytes));
  if (tmp) {
    for (int y = 0; y < height / 2; y++) {
      unsigned char *a = pixels + size_t(y) * row_bytes;
      unsigned char *b = pixels + size_t(height - 1 - y) * row_bytes;
      std::memcpy(tmp, a, row_bytes);
      std::memcpy(a, b, row_bytes);
      std::memcpy(b, tmp, row_bytes);
    }
    std::free(tmp);
  }

  int ok = stbi_write_png(path, width, height, 4, pixels, int(row_bytes));
  std::free(pixels);
  if (!ok) {
    fprintf(stderr, "captureToPNG: stbi_write_png failed for '%s'\n", path);
    return false;
  }
  return true;
}

} // namespace sculptcore::opengl
