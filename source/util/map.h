#include "alloc.h"
#include "hash.h"
#include <span>
#include <type_traits>
#include <vector>

namespace sculptcore::util {
static size_t hashsizes[] = {
    1,       2,       5,       7,        19,       53,        137,       347,
    877,     2203,    5519,    13799,    34499,    86249,     215653,    539141,
    1347877, 3369697, 8424287, 21060731, 52651829, 131629573, 329073937, 536870909};

static size_t find_hashsize(int size)
{
  int prime = 0;
  while (hashsizes[prime + 1] < size) {
    prime++;
  }

  return prime;
}

template <typename Key, typename Value, int static_size = 4> class Map {
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

  Map()
      : table_(static_storage_, find_hashsize(static_size * 3)),
        cur_size_(find_hashsize(static_size * 3))
  {
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

  bool add_overwrite(const Key &key, const Value &value)
  {
    return add_intern<true>(key, value);
  }

  bool contains(const Key &key) const
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
      table_[i] = {};
    }

    return true;
  }

private:
  std::span<Pair> table_;
  Pair static_storage_[static_size * 3];
  std::vector<bool> used_;
  int cur_size_ = 0;
  int used_count_ = 0;

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
  int find_pair(const Key &key)
  {
    hash::HashInt hashval = hash::hash(key);
    hash::HashInt size = hash::HashInt(hashsizes[cur_size_]);
    hash::HashInt h = hashval % size;

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

      h = ((h << 1) + hashval + 1) % size;
    }
  }

  bool check_load()
  {
    if (used_count_ > table_.size() >> 1) {
      realloc_to_size(table_.size() << 1);
      return true;
    }

    return false;
  }

  void realloc_to_size(size_t size)
  {
    size_t old_size = hashsizes[cur_size_];
    while (hashsizes[cur_size_] < size) {
      cur_size_++;
    }

    std::span<Pair> old = table_;
    size_t newsize = hashsizes[cur_size_];

    if constexpr (Pair::is_simple()) {
      table_ = std::span(static_cast<Pair *>(alloc::alloc("sculpecore::util::map table",
                                                          newsize * sizeof(Pair))),
                         newsize);
    } else {
      table_ = std::span(alloc::NewArray<Pair>("sculptcore::util::map table", newsize),
                         newsize);
    }

    std::vector old_used = used_;

    used_.resize(newsize);
    for (int i = 0; i < used_.size(); i++) {
      used_[i] = false;
    }

    for (int i = 0; i < old.size(); i++) {
      if (old_used[i]) {
        insert(std::move(old[i].key), std::move(old[i].value));
      }
    }

    if (old.data() != static_storage_) {
      if constexpr (Pair::is_simple()) {
        alloc::release(static_cast<void *>(old.data()));
      } else {
        alloc::DeleteArray<Pair>(old.data());
      }
    }
  }
};
} // namespace sculptcore::util
