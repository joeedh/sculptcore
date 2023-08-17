namespace sculptcore::io {

template <typename T> struct register_class {
  template <typename... Args> register_class(const char *script, Args...)
  {
  }
};
} // namespace sculptcore::io
