#pragma once

namespace sculptcore::alloc {

void *alloc(const char *tag, size_t size);
void *release(void *mem);

template <typename T, typename... Args> inline T *New(const char *tag, Args... args)
{
  void *mem = alloc(tag, sizeof(T));

  return new (mem) T(&args...);
}

template <typename T, typename... Args>
inline T *NewArray(const char *tag, size_t size, Args... args)
{
  if (size == 0) {
    return nullptr;
  }

  void *mem = alloc(tag, sizeof(T) * size);
  T *elem = static_cast<T *>(mem);

  for (int i = 0; i < size; i++) {
    new (elem) T(&args...);
  }

  return static_cast<T *>(elem);
}

template <typename T> inline void Delete(T *arg)
{
  if (arg) {
    arg->~T();
    release(static_cast<void *>(arg));
  }
}

template <typename T> inline void DeleteArray(T *arg, size_t size)
{
  if (arg) {
    for (int i = 0; i < size; i++) {
      arg[i].~T();
    }

    release(static_cast<void *>(arg));
  }
}
void print_blocks();

}; // namespace sculptcore::alloc
