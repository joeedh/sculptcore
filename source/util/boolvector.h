#pragma once

#include "alloc.h"
#include "compiler_util.h"
#include <cstdint>

namespace sculptcore::util {
template <int static_size = 32> class BoolVector {
  using BlockInt = uint32_t;
  static constexpr int block_size = 32;
  static constexpr int block_shift = 5;
  static constexpr int block_mask = 31;

public:
  BoolVector()
  {
    vector_ = static_storage_;
    size_ = static_size;
    vector_size_ = static_size >> block_shift;

    for (int i = 0; i < vector_size_; i++) {
      vector_[i] = 0;
    }
  }

  ~BoolVector()
  {
    if (vector_ != static_storage_) {
      alloc::release(static_cast<void *>(vector_));
    }
  }

  void reset()
  {
    used_ = 0;
  }

  void clear()
  {
    for (int i = 0; i < vector_size_; i++) {
      vector_[i] = BlockInt(0);
    }
  }

  const bool operator[](int index)
  {
    int bit = 1 << (index & block_mask);
    return vector_[index >> block_shift] & bit;
  }

  void set(int index, bool val)
  {
    int bit = 1 << (index & block_mask);

    if (val) {
      vector_[index >> block_shift] |= bit;
    } else {
      vector_[index >> block_shift] &= ~bit;
    }
  }

  int size()
  {
    return used_;
  }

  void resize(int newsize)
  {
    if (newsize <= size_) {
      used_ = newsize;
      return;
    }

    realloc((newsize * 2) >> block_shift);
  }

  void append(bool val)
  {
    resize(used_ + 1);
    set(used_ - 1, val);
  }

private:
  BlockInt *vector_;
  BlockInt static_storage_[static_size >> block_shift];
  int size_ = 0, vector_size_ = 0;
  int used_ = 0;

  void realloc(int new_vec_size)
  {
    BlockInt *old = vector_;

    vector_ = static_cast<BlockInt *>(
        alloc::alloc("BitVector data", sizeof(BlockInt) * new_vec_size));

    int i = 0;
    for (i = 0; i < vector_size_; i++) {
      vector_[i] = old[i];
    }
    for (; i < new_vec_size; i++) {
      vector_[i] = 0;
    }

    vector_size_ = new_vec_size;
    size_ = new_vec_size << block_shift;

    if (old != static_storage_) {
      alloc::release(static_cast<void *>(old));
    }
  }
};
} // namespace sculptcore::util
