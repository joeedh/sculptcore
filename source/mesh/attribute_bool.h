#pragma once

#include "attribute_base.h"
#include "attribute_enums.h"
#include "mesh_enums.h"
#include "litestl/util/alloc.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace litestl;
namespace sculptcore::mesh {
using util::string;

class PackedBoolAttrs {
public:
  struct Offset {
    short block, bit;
  };

  struct AttrInfo {
    string name;
    Offset offset;
  };

  PackedBoolAttrs()
  {
  }

  ~PackedBoolAttrs()
  {
    printf("~PackedBoolAttrs\n");

    if (blocks_) {
      alloc::release(static_cast<void *>(blocks_));
    }
  }

  PackedBoolAttrs(PackedBoolAttrs &&b)
  {
    cur_block_ = b.cur_block_;
    cur_bit_ = b.cur_bit_;
    size_ = b.size_;
    capacity_ = b.capacity_;
    blocksize_ = b.blocksize_;

    blocks_ = b.blocks_;
    attrs_ = std::move(b.attrs_);

    b.blocks_ = nullptr;
    b.size_ = 0;
  }

  PackedBoolAttrs(const PackedBoolAttrs &b)
  {
    cur_block_ = b.cur_block_;
    cur_bit_ = b.cur_bit_;
    size_ = b.size_;
    capacity_ = b.capacity_;
    blocksize_ = b.blocksize_;

    size_t totalsize = capacity_ * blocksize_;
    blocks_ = static_cast<uint8_t *>(alloc::alloc("PackedBoolAttrs", totalsize));
    memcpy(static_cast<void *>(blocks_), static_cast<void *>(b.blocks_), totalsize);

    attrs_ = b.attrs_;
  }

  PackedBoolAttrs &operator=(PackedBoolAttrs &&b)
  {
    if (this == &b) {
      return *this;
    }

    this->~PackedBoolAttrs();
    PackedBoolAttrs(std::forward<PackedBoolAttrs>(b));

    return *this;
  }

  uint8_t *operator[](int elem)
  {
    return blocks_ + elem * blocksize_;
  }

  int blocksize()
  {
    return blocksize_;
  }

  void set_default(int elem)
  {
    for (AttrInfo &info : attrs_) {
      set(elem, info.offset, false);
    }
  }

  Offset add(const string &name)
  {
    AttrInfo info;
    info.name = name;
    info.offset.block = cur_block_;
    info.offset.bit = 1 << (cur_bit_++);
    bool realloc = false;

    if (cur_bit_ == 8) {
      cur_bit_ = 0;
      cur_block_++;
      add_block();
    }

    attrs_.append(info);
    blocksize_ = cur_block_ + 1;

    return info.offset;
  }

  Offset lookup(const string &name)
  {
    for (AttrInfo &info : attrs_) {
      if (info.name == name) {
        return info.offset;
      }
    }

    return {-1, -1};
  }

  void resize(size_t newsize)
  {
    if (blocksize_ == 0) {
      return;
    }

    if (newsize >= capacity_) {
      capacity_ = (newsize * blocksize_ + 8) << 2;
      capacity_ -= capacity_ >> 1;

      uint8_t *old = blocks_;

      blocks_ =
          static_cast<uint8_t *>(alloc::alloc("PackedBoolAttrs", capacity_ * blocksize_));

      if (size_) {
        memcpy(
            static_cast<void *>(blocks_), static_cast<void *>(old), size_ * blocksize_);
        alloc::release(static_cast<void *>(old));
      }
    }

    size_ = newsize;
  }

  inline bool get(int idx, Offset &offset)
  {
    int block = idx * blocksize_;
    return blocks_[block] & offset.bit;
  }

  inline bool set(int idx, Offset &offset, bool state)
  {
    int block = idx * blocksize_;
    bool old = blocks_[block] & offset.bit;

    if (state) {
      blocks_[block] |= offset.bit;
    } else {
      blocks_[block] &= ~offset.bit;
    }

    return old;
  }

  size_t size()
  {
    return size_;
  }

private:
  void add_block()
  {
    if (capacity_ == 0) {
      blocksize_++;
      return;
    }

    uint8_t *old = blocks_;
    int new_blocksize = blocksize_ + 1;

    blocks_ = static_cast<uint8_t *>(
        alloc::alloc("PackedBool add_block", capacity_ * new_blocksize));

    for (int i = 0; i < capacity_; i++) {
      for (int j = 0; j < blocksize_; j++) {
        blocks_[i * new_blocksize + j] = old[i * blocksize_ + j];
      }

      blocks_[i * new_blocksize + blocksize_] = 0;
    }

    alloc::release(static_cast<void *>(old));
    blocksize_ = new_blocksize;
  }

  int blocksize_ = 0;
  uint8_t *blocks_ = nullptr;

  size_t size_ = 0;     /* logical size. */
  size_t capacity_ = 0; /* logical capacity. */

  util::Vector<AttrInfo> attrs_;
  int cur_block_ = 0, cur_bit_ = 0;
};

struct BoolAttrView : public AttrDataBase {
public:
  BoolAttrView(const string &name_, PackedBoolAttrs *data, PackedBoolAttrs::Offset offset)
      : data_(data), offset_(offset), AttrDataBase(AttrType::BOOL, name_)
  {
  }

  BoolAttrView(const BoolAttrView &b)
      : data_(b.data_), offset_(b.offset_), AttrDataBase(AttrType::BOOL, b.name)
  {
  }

  ~BoolAttrView()
  {
  }

  inline bool operator[](int idx)
  {
    return data_->get(idx, offset_);
  }

  inline bool get(int idx)
  {
    return data_->get(idx, offset_);
  }

  inline bool set(int idx, bool state)
  {
    return data_->set(idx, offset_, state);
  }

  size_t size()
  {
    return data_->size();
  }

private:
  PackedBoolAttrs::Offset offset_;
  PackedBoolAttrs *data_;
};

} // namespace sculptcore::mesh
