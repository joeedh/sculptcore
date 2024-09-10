#include "vbo.h"
#include "opengl.h"

using namespace litestl;

namespace sculptcore::gpu {
Buffer::~Buffer()
{
  release();
}

void Buffer::resize(int newsize)
{
  if (size == newsize) {
    return;
  }

  if (uploaded) {
    release();
  }

  if (newsize > size) {
    if (data) {
      alloc::release(data);
    }

    data = alloc::alloc("gpu attribute", size * elemsize * gpu_sizeof(type));
  }

  size = newsize;
}

void Buffer::release()
{
  if (uploaded) {
    GLuint buffers[1] = {gl_buffer};
    glDeleteBuffers(1, buffers);

    uploaded = false;
  }
}

void Buffer::check_upload()
{
  if (!uploaded) {
    upload();
  } else if (update_buffer) {
    glBindBuffer(target, gl_buffer);
    glBufferData(target, size, data, hint);
    update_buffer = false;
  }
}

void Buffer::upload()
{
  glCreateBuffers(1, &gl_buffer);
  glBindBuffer(target, gl_buffer);
  glBufferData(target, size, data, hint);
  update_buffer = false;
}

VBO::~VBO()
{
  for (auto &pair : attrs) {
    Buffer *buf = pair.value;

    if (buf->owner_vbo == this) {
      alloc::Delete<Buffer>(buf);
    }
  }
}

} // namespace sculptcore::gpu
