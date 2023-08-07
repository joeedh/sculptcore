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

enum GPUBufferHint { HINT_STATIC = GL_STATIC_DRAW, HINT_DYNAMIC = GL_DYNAMIC_DRAW };

enum GPUBufferType {
  BUFFER_ARRAY = GL_ARRAY_BUFFER,
  BUFFER_INDEX = GL_ELEMENT_ARRAY_BUFFER,
  BUFFER_TEXTURE = GL_TEXTURE_BUFFER,
  BUFFER_UNIFORM = GL_UNIFORM_BUFFER,
};

struct Buffer {
  GPUValueType type;
  GPUValueMode mode;
  GPUBufferHint hint = HINT_DYNAMIC;
  GPUBufferType target = BUFFER_ARRAY;

  int size = 0, elemsize;
  bool uploaded = false;

  void *data = nullptr;
  unsigned int gl_buffer;

  void resize(int newsize);

  Buffer()
  {
  }

  Buffer(GPUValueType type_,
         int elemsize_,
         GPUValueMode mode_ = GPU_FETCH_FLOAT,
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
      DEF_CASE(float, GPU_FLOAT)
      DEF_CASE(int, GPU_INT32)
      DEF_CASE(uint, GPU_UINT32)
      DEF_CASE(short, GPU_INT16)
      DEF_CASE(ushort, GPU_UINT16)
      DEF_CASE(char, GPU_INT8)
      DEF_CASE(uchar, GPU_UINT8)
    }
  }

  void upload();
  void release();

  ~Buffer();

  Buffer(const Buffer &b) = delete;
  Buffer &operator=(Buffer &&b)
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

  template <typename T> T *get_data()
  {
    return static_cast<T *>(data);
  }

private:
  template <typename T, typename List> int load_data_intern(List &list)
  {
    T *data = get_data();

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

  bool contains(stringref name)
  {
    return attrs.contains(name);
  }

  void add(stringref name, Buffer *buf)
  {
    attrs[name] = buf;
  }
};

} // namespace sculptcore::gpu
