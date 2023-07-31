#include "alloc.h"
#include "compiler_util.h"
#include "hash.h"
#include <span>
#include <type_traits>
#include <vector>

namespace sculptcore::util {
static size_t hashsizes[] = {
    3,         5,         7,         11,        17,        23,        29,
    37,        47,        59,        79,        101,       127,       163,
    211,       269,       337,       431,       541,       677,       853,
    1069,      1361,      1709,      2137,      2677,      3347,      4201,
    5261,      6577,      8231,      10289,     12889,     16127,     20161,
    25219,     31531,     39419,     49277,     61603,     77017,     96281,
    120371,    150473,    188107,    235159,    293957,    367453,    459317,
    574157,    717697,    897133,    1121423,   1401791,   1752239,   2190299,
    2737937,   3422429,   4278037,   5347553,   6684443,   8355563,   10444457,
    13055587,  16319519,  20399411,  25499291,  31874149,  39842687,  49803361,
    62254207,  77817767,  97272239,  121590311, 151987889, 189984863, 237481091,
    296851369, 371064217, 463830313, 536870909,
};

static size_t find_hashsize(int size)
{
  int prime = 0;
  while (hashsizes[prime + 1] < size) {
    prime++;
  }

  return prime;
}

template <typename Key, typename Value, int static_size = 16> class Map {
  const static int real_static_size = static_size * 3;

public:
  struct Pair {
    Key key;
    Value value;

    static constexpr bool is_simple()
    {
      return (std::is_integral_v<Key> || std::is_pointer_v<Key>)&&(
          std::is_integral_v<Value> || std::is_floating_point_v<Value> ||
          std::is_pointer_v<Value>);
    }
  };

  using key_type = Key;
  using value_type = Value;

  struct iterator {
    iterator(Map *map, int i) : map_(map), i_(i)
    {
      if (i_ == 0) {
        /* Find first item. */
        i_ = -1;
        operator++();
      }
    }

    iterator(const iterator &b) : map_(b.map_), i_(b.i_)
    {
    }

    bool operator==(const iterator &b)
    {
      return b.i_ == i_;
    }
    bool operator!=(const iterator &b)
    {
      return !operator==(b);
    }

    const Pair &operator*()
    {
      return map_->table_[i_];
    }

    iterator &operator++()
    {
      i_++;

      while (i_ < map_->table_.size() && !map_->used_[i_]) {
        i_++;
      }

      return *this;
    }

  private:
    int i_;
    Map *map_;
  };

  Map()
      : table_(static_storage_, hashsizes[find_hashsize(real_static_size)]),
        cur_size_(find_hashsize(real_static_size))
  {
    reserve_usedmap();
  }

  ~Map()
  {
    if (table_.data() == static_storage_) {
      return;
    }

    if constexpr (!Pair::is_simple()) {
      for (int i = 0; i < table_.size(); i++) {
        if (used_[i]) {
          table_[i].~Pair();
        }
      }
    }

    alloc::release(static_cast<void *>(table_.data()));
  }

  iterator begin()
  {
    return iterator(this, 0);
  }

  iterator end()
  {
    return iterator(this, table_.size());
  }

  size_t size()
  {
    return size_t(used_count_);
  }

  /* Does not check if key already exists. */
  void insert(const Key &key, const Value &value)
  {
    check_load();

    int i = find_pair<false, true>(key);
    add_finalize(i, key, value);
  }

  void insert(const Key &&key, const Value &&value)
  {
    check_load();

    int i = find_pair<false, true>(key);
    add_finalize(i, key, value);
  }

  bool add(const Key &key, const Value &value)
  {
    return add_intern<false>(key, value);
  }
  bool add(const Key &&key, const Value &&value)
  {
    return add_intern<false>(key, value);
  }

  bool add_overwrite(const Key &key, const Value &value)
  {
    return add_intern<true>(key, value);
  }
  bool add_overwrite(const Key &&key, const Value &&value)
  {
    return add_intern<true>(key, value);
  }

  bool contains(const Key &key) const
  {
    int i = find_pair<true, false>(key);
    return i != -1;
  }
  bool contains(const Key &&key) const
  {
    int i = find_pair<true, false>(key);
    return i != -1;
  }

  /* Undefined behavior if key is not in map, check existence with .contains(). */
  Value &lookup(const Key &key) const
  {
    int i = find_pair<true, false>(key);
    return table_[i].value;
  }

  bool remove(const Key &key)
  {
    int i = find_pair<true, false>(key);

    if (i == -1) {
      return false;
    }

    used_[i] = false;

    if constexpr (!Pair::is_simple()) {
      table_[i].~Pair();
    }

    return true;
  }

private:
  std::span<Pair> table_;
  Pair static_storage_[real_static_size];
  std::vector<bool> used_;
  int cur_size_ = 0;
  int used_count_ = 0;

  void reserve_usedmap()
  {
    hash::HashInt size = hash::HashInt(hashsizes[cur_size_]);
    used_.resize(size);

    used_.assign(size, false);
  }

  template <bool overwrite = false> bool add_intern(const Key &key, const Value &value)
  {
    check_load();

    int i = find_pair<true, true>(key);
    if (used_[i]) {
      if constexpr (overwrite) {
        add_finalize(i, key, value);
      }

      return false;
    }

    add_finalize(i, key, value);

    return true;
  }

  /* Handles rvalues. */
  template <typename KeyArg, typename ValueArg>
  void add_finalize(int i, const KeyArg key, const ValueArg value)
  {
    table_[i].key = key;
    table_[i].value = value;
    used_count_++;
    used_[i] = true;
  }

  template <bool check_key_equals = true, bool return_unused_cell = false>
  ATTR_NO_OPT int find_pair(const Key &key) const
  {
    const hash::HashInt size = hash::HashInt(table_.size());
    hash::HashInt hashval = hash::hash(key) % size;
    hash::HashInt h = hashval, probe = hashval;

    while (1) {
      if (!used_[h]) {
        if constexpr (return_unused_cell) {
          return h;
        } else {
          return -1;
        }
      }

      if constexpr (check_key_equals) {
        if (table_[h].key == key) {
          return h;
        }
      }

      probe++;
      hashval += probe;
      h = hashval % size;
    }
  }

  ATTR_NO_OPT bool check_load()
  {
    if (used_count_ > table_.size() / 3) {
      realloc_to_size(table_.size() * 3);
      return true;
    }

    return false;
  }

  ATTR_NO_OPT void realloc_to_size(size_t size)
  {
    size_t old_size = hashsizes[cur_size_];
    while (hashsizes[cur_size_] < size) {
      cur_size_++;
    }

    size_t newsize = hashsizes[cur_size_];

    printf("newsize: %d\n", int(newsize));

    std::span<Pair> old = table_;
    std::vector<bool> old_used = used_;

    table_ = std::span(static_cast<Pair *>(alloc::alloc("sculpecore::util::map table",
                                                        newsize * sizeof(Pair))),
                       newsize);

    used_count_ = 0;
    used_.resize(newsize);
    used_.assign(newsize, false);

    for (int i = 0; i < old.size(); i++) {
      if (old_used[i]) {
        insert(std::move(old[i].key), std::move(old[i].value));
      }
    }

    if (old.data() != static_storage_) {
      alloc::release(static_cast<void *>(old.data()));
    }
  }
};
} // namespace sculptcore::util
