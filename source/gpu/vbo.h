#pragma once

#include "types.h"

#include "math/vector.h"

#include "util/compiler_util.h"
#include "util/map.h"
#include "util/string.h"
#include "util/vector.h"

#include "opengl.h"

#include <functional>
#include <type_traits>

namespace sculptcore::gpu {
using namespace sculptcore::util;

struct VBO;

enum GPUBufferHint { HINT_STATIC = GL_STATIC_DRAW, HINT_DYNAMIC = GL_DYNAMIC_DRAW };

enum GPUBufferType {
  BUFFER_ARRAY = GL_ARRAY_BUFFER,
  BUFFER_INDEX = GL_ELEMENT_ARRAY_BUFFER,
  BUFFER_TEXTURE = GL_TEXTURE_BUFFER,
  BUFFER_UNIFORM = GL_UNIFORM_BUFFER,
};

struct Buffer {
  GPUType type;
  GPUFetchMode mode;
  GPUBufferHint hint = HINT_DYNAMIC;
  GPUBufferType target = BUFFER_ARRAY;

  int size = 0, elemsize;
  bool uploaded = false;

  VBO *owner_vbo = nullptr;

  void *data = nullptr;
  unsigned int gl_buffer;
  bool update_buffer = true;

  void resize(int newsize);

  Buffer()
  {
  }

  Buffer(GPUType type_,
         int elemsize_,
         GPUFetchMode mode_ = GPUFetchMode::FETCH_FLOAT,
         int elem_count = 0)
      : type(type_), elemsize(elemsize_), mode(mode_)
  {
    if (elem_count) {
      resize(elem_count);
    }
  }

  template <typename List> int load_data(List &list)
  {
    /* For ushort, uint, uchar. */
    using namespace sculptcore::util;
    /* For math vector types. */

    using namespace sculptcore::math;
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
    update_buffer = true;
  }

  ~Buffer();

  Buffer(const Buffer &b) = delete;
  Buffer(Buffer &&b)
  {
    type = b.type;
    hint = b.hint;
    size = b.size;
    uploaded = b.uploaded;
    data = b.data;

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
      update_buffer = true;
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

    buf = alloc::New<Buffer>(
        "Buffer", gpu_type_from<T>(), gpu_type_elems<T>(), GPUFetchMode::FETCH_FLOAT, size);
    buf->owner_vbo = this;
    buf->target = target;

    attrs[name] = buf;

    return buf;
  }
};

} // namespace sculptcore::gpu
