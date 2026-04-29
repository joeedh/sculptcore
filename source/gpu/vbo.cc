#include "vbo.h"
#include "binding/binding_types.h"
#include "litestl/binding/binding.h"
#include "manager.h"

using namespace litestl;

namespace sculptcore::gpu {
Buffer::~Buffer()
{
  if (manager.buffers.contains(this)) {
    manager.buffers.remove(this);
  }
  release();
}

binding::types::Struct<Buffer> *Buffer::defineBindings()
{
  using litestl::binding::types::Constructor;
  using litestl::binding::types::Struct;
  using namespace litestl::binding;
  Struct<Buffer> *st = new Struct<Buffer>("sculptcore::gpu::Buffer", sizeof(Buffer));

  /* No constructor: Buffer requires GPUManager& which would create a cycle
   * through GPUManager::buffers. Use GPUManager::createBuffer() from JS. */

  BIND_STRUCT_MEMBER(st, name);
  BIND_STRUCT_MEMBER(st, type);
  BIND_STRUCT_MEMBER(st, size);
  BIND_STRUCT_MEMBER(st, elemsize);
  BIND_STRUCT_MEMBER(st, mode);
  BIND_STRUCT_MEMBER(st, target);
  BIND_STRUCT_MEMBER(st, data);
  BIND_STRUCT_MEMBER(st, update_buffer);

  BIND_STRUCT_METHOD(st, resize);
  BIND_STRUCT_METHOD(st, dirty);

  return st;
}

void Buffer::release()
{
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

/*
direct binding Bind function for if the
generic pointer binding code in sculptcore\source\litestl\binding\binding_utils.h 
doesn't work:

namespace litestl::binding {
template <std::same_as<sculptcore::gpu::Buffer *> T> const BindingBase *Bind()
{
  return new types::Pointer(Bind<sculptcore::gpu::Buffer>());
}
} // namespace litestl::binding
*/