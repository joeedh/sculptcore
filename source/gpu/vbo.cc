#include "vbo.h"
#include "litestl/binding/binding.h"
#include "manager.h"

using namespace litestl;

namespace sculptcore::gpu {
Buffer::~Buffer()
{
  /* Notify backends so they drop cache entries keyed by this pointer and defer
   * native-handle destruction to a safe point. This must run for EVERY deletion
   * path, not just GPUManager::destroyBuffer — producers (e.g. spatial GpuData)
   * free buffers with a raw alloc::Delete, and without this the backend keeps a
   * stale BufferEntry with a live VkBuffer (leak + double-free once the pointer
   * is reused). onBufferDestroyed is idempotent, so destroyBuffer's call is
   * harmless if it also fires. */
  for (auto *obs : manager.observers) {
    obs->onBufferDestroyed(this);
  }
  if (manager.buffers.contains(this)) {
    manager.buffers.remove(this);
  }
  release();

  /* release() only drops the GPU-side upload; the host-side attribute storage
   * (alloc'd in resize) is ours to free. A moved-from Buffer has data==null. */
  if (data) {
    alloc::release(data);
    data = nullptr;
  }
}

const binding::types::Struct<Buffer> *Buffer::defineBindings()
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

  BIND_STRUCT_METHOD(st, resize, MARGS("size"));
  BIND_STRUCT_METHOD(st, dirty, MARGS());

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

    data = alloc::alloc("gpu attribute", newsize * elemsize * gpu_sizeof(type));
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
direct Binder specialization for if the
generic pointer binding code in sculptcore\source\litestl\binding\binding_utils.h
doesn't work (declare it in a gpu header, e.g. vbo.h, so every TU agrees):

namespace litestl::binding {
template <> struct Binder<sculptcore::gpu::Buffer *> {
  static const BindingBase *bind()
  {
    return new types::Pointer(Bind<sculptcore::gpu::Buffer>());
  }
};
} // namespace litestl::binding
*/