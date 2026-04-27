#include "vbo.h"
#include "litestl/binding/binding.h"

using namespace litestl;

namespace sculptcore::gpu {
Buffer::~Buffer()
{
  release();
}

binding::types::Struct<Buffer> *Buffer::defineBindings()
{
  using litestl::binding::types::Constructor;
  using litestl::binding::types::Struct;
  using namespace litestl::binding;
  Struct<Buffer> *st = new Struct<Buffer>("sculptcore::gpu::Buffer", sizeof(Buffer));

  BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
  BIND_STRUCT_CONSTRUCTOR(st, "main", litestl::util::string, GPUType, int, GPUFetchMode, int);

  BIND_STRUCT_MEMBER(st, type);
  BIND_STRUCT_MEMBER(st, size);
  BIND_STRUCT_MEMBER(st, elemsize);
  BIND_STRUCT_MEMBER(st, mode);
  BIND_STRUCT_MEMBER(st, data);
  BIND_STRUCT_MEMBER(st, update_buffer);

  return st;
}

void Buffer::release() {
  //
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
