#pragma once

#include "types.h"

#include "litestl/math/vector.h"

#include "litestl/util/compiler_util.h"
#include "litestl/util/map.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include "manager.h"
#include <algorithm>
#include <functional>
#include <type_traits>

#include "litestl/binding/binding.h"


using namespace litestl;
using namespace litestl::util;

namespace sculptcore::gpu {

struct VBO;

enum GPUBufferHint { HINT_STATIC = 0, HINT_DYNAMIC = 1 };

enum GPUBufferType {
  BUFFER_ARRAY = 0,
  BUFFER_INDEX = 1,
  BUFFER_TEXTURE = 2,
  BUFFER_UNIFORM = 3,
};
} // namespace sculptcore::gpu

namespace litestl::binding {
template <> struct Binder<sculptcore::gpu::GPUBufferType> {
  static const BindingBase *bind()
  {
    using namespace sculptcore::gpu;
    types::Enum *e =
        new types::Enum("sculptcore::gpu::GPUBufferType", sizeof(GPUBufferType));

    e->addItem("BUFFER_ARRAY", GPUBufferType::BUFFER_ARRAY);
    e->addItem("BUFFER_INDEX", GPUBufferType::BUFFER_INDEX);
    e->addItem("BUFFER_TEXTURE", GPUBufferType::BUFFER_TEXTURE);
    e->addItem("BUFFER_UNIFORM", GPUBufferType::BUFFER_UNIFORM);
    return e;
  }
};

template <> struct Binder<sculptcore::gpu::GPUBufferHint> {
  static const BindingBase *bind()
  {
    using namespace sculptcore::gpu;
    types::Enum *e =
        new types::Enum("sculptcore::gpu::GPUBufferHint", sizeof(GPUBufferHint));
    e->addItem("HINT_STATIC", GPUBufferHint::HINT_STATIC);
    e->addItem("HINT_DYNAMIC", GPUBufferHint::HINT_DYNAMIC);
    return e;
  }
};
} // namespace litestl::binding

namespace sculptcore::gpu {

struct Buffer {
  string name;
  GPUType type;
  GPUFetchMode mode;
  GPUBufferHint hint = HINT_DYNAMIC;
  GPUBufferType target = BUFFER_ARRAY;

  int size = 0, elemsize = 0;
  bool uploaded = false;

  VBO *owner_vbo = nullptr;

  void *data = nullptr;
  bool update_buffer = true;

  /** Dirty sub-range of `update_buffer`, in element (vert) units. `update_end < 0`
   * means the whole buffer (the default). Producers that touch only a slice call
   * markDirtyRange so consumers with partial-upload support (the TS WebGPU
   * executor) re-upload just that span; consumers without it upload the whole
   * buffer as before. Consumers reset to (0, -1) when they clear update_buffer. */
  int update_start = 0;
  int update_end = -1;

  /* When set, the backend allocates the VkBuffer with STORAGE usage in
   * addition to VERTEX/INDEX so a compute pass can write it directly. */
  bool gpu_storage = false;
  /* When set, the backend never memcpys host `data` into the VkBuffer — its
   * contents are produced GPU-side (e.g. a compute scatter). The VkBuffer is
   * still created/grown to match `size`. */
  bool gpu_owned = false;

  void resize(int newsize);

  static const binding::types::Struct<Buffer> *defineBindings();
  GPUManager &manager;

  Buffer(GPUManager &mgr,
         string name,
         GPUType type_,
         int elemsize_,
         GPUFetchMode mode_ = GPUFetchMode::FETCH_FLOAT,
         int elem_count = 0)
      : type(type_), elemsize(elemsize_), mode(mode_), name(name), manager(mgr)
  {
    manager.buffers.append(this);

    if (elem_count) {
      resize(elem_count);
    }
  }
  
  template <typename List> int load_data(List &list)
  {
    /* For ushort, uint, uchar. */
    using namespace litestl::util;
    /* For math vector types. */

    using namespace litestl::math;
#ifdef DEF_CASE
#undef DEF_CASE
#endif

#define DEF_CASE(type, gputype)                                                          \
  case gputype: {                                                                        \
    switch (elemsize) {                                                                  \
    case 1:                                                                              \
      return load_data_intern<type>(list);                                               \
    case 2:                                                                              \
      return load_data_intern<type##2>(list);                                            \
    case 3:                                                                              \
      return load_data_intern<type##3>(list);                                            \
    case 4:                                                                              \
      return load_data_intern<type##4>(list);                                            \
    }                                                                                    \
    break;                                                                               \
  }

    switch (type) {
      DEF_CASE(float, GPUType::FLOAT32)
      DEF_CASE(int, GPUType::INT32)
      DEF_CASE(uint, GPUType::UINT32)
      DEF_CASE(short, GPUType::INT16)
      DEF_CASE(ushort, GPUType::UINT16)
      DEF_CASE(char, GPUType::INT8)
      DEF_CASE(uchar, GPUType::UINT8)
    }
  }

  void check_upload();
  void upload();
  void release();

  void dirty()
  {
    markDirtyAll();
  }

  /** Flag the whole buffer for re-upload (also widens any pending sub-range). */
  void markDirtyAll()
  {
    update_buffer = true;
    update_start = 0;
    update_end = -1;
  }

  /** Flag `[start, end)` (element units) for re-upload. Unions with a pending
   * dirty range; a pending whole-buffer flag stays whole. */
  void markDirtyRange(int start, int end)
  {
    if (update_buffer) {
      if (update_end < 0) {
        return; /* already whole-buffer */
      }
      update_start = std::min(update_start, start);
      update_end = std::max(update_end, end);
      return;
    }
    update_buffer = true;
    update_start = start;
    update_end = end;
  }

  ~Buffer();

  Buffer(const Buffer &b) = delete;

  Buffer(Buffer &&b) : manager(b.manager)
  {
    type = b.type;
    hint = b.hint;
    size = b.size;
    uploaded = b.uploaded;
    data = b.data;

    // transfer ownership within GPUManager from b to this
    manager.transferOwnership(manager.buffers, this, &b);

    b.data = nullptr;
    b.uploaded = false;
    b.size = 0;
  }

  Buffer &operator=(Buffer &&b)
  {
    if (this == &b) {
      return *this;
    }

    this->~Buffer();

    Buffer(std::forward<Buffer>(b));
    return *this;
  }

  template <typename T> T *get_data(bool mark_update = false)
  {
    if (mark_update) {
      markDirtyAll();
    }

    return static_cast<T *>(data);
  }

private:
  template <typename T, typename List> int load_data_intern(List &list)
  {
    T *data = get_data<T>();

    if constexpr (!is_simple<T>()) {
      for (int i = 0; i < size; i++) {
        for (int j = 0; j < elemsize; j++) {
          data[i * elemsize + j] = list[i][j];
        }
      }
    } else {
      for (int i = 0; i < size; i++) {
        data[i] = list[i];
      }
    }

    return 0;
  }
};

struct VBO {
  Map<string, Buffer *> attrs;
  int elem_count = 0;

  VBO(const VBO &b) = delete;
  VBO(int elem_count_) : elem_count(elem_count_)
  {
  }

  ~VBO();

  bool contains(stringref name)
  {
    return attrs.contains(name);
  }

  Buffer *find(stringref name)
  {
    Buffer **ret = attrs.lookup_ptr(name);

    if (ret) {
      return *ret;
    }

    return nullptr;
  }

  void add(stringref name, Buffer *buf)
  {
    attrs[name] = buf;
  }

  template <typename T>
  Buffer *ensure(stringref name, int size, GPUBufferType target = BUFFER_INDEX)
  {
    Buffer *buf = find(name);
    if (buf) {
      return buf;
    }

    buf = alloc::New<Buffer>("Buffer",
                             gpu_type_from<T>(),
                             gpu_type_elems<T>(),
                             GPUFetchMode::FETCH_FLOAT,
                             size);
    buf->owner_vbo = this;
    buf->target = target;

    attrs[name] = buf;

    return buf;
  }
};

} // namespace sculptcore::gpu
