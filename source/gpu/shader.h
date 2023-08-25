#pragma once

#include "math/vector.h"

#include "util/alloc.h"
#include "util/boolvector.h"
#include "util/compiler_util.h"
#include "util/map.h"
#include "util/string.h"
#include "util/vector.h"

#include "gpu/types.h"

namespace sculptcore::gpu {
using sculptcore::util::string;
using sculptcore::util::stringref;

struct Uniform {
  Uniform() noexcept
  {
  }

  Uniform(const Uniform &b)
  {
    copy_intern(b);
  }

  Uniform(Uniform &&b)
  {
    move_intern(std::forward<Uniform>(b));
  }

  Uniform &operator=(Uniform &&b) noexcept
  {
    if (this == &b) {
      return *this;
    }

    this->~Uniform();
    move_intern(std::forward<Uniform>(b));

    return *this;
  }

  Uniform &operator=(const Uniform &b) noexcept
  {
    if (this == &b) {
      return *this;
    }

    this->~Uniform();
    copy_intern(b);

    return *this;
  }

  template <typename T> Uniform(string name, T value) : name_(name)
  {
    default_ = alloc::alloc("uniform default", sizeof(T));
    type_ = gpu_type_from<T>();

    T *val = static_cast<T *>(default_);
    *val = value;
  }

  ~Uniform()
  {
    if (default_) {
      alloc::release(default_);
      default_ = nullptr;
    }
  }

  int glLoc() const noexcept
  {
    return glLoc_;
  }
  GPUType type() const noexcept
  {
    return type_;
  }
  const string &name() const noexcept
  {
    return name_;
  }

private:
  void copy_intern(const Uniform &b) noexcept
  {
    type_ = b.type_;
    size_t size = gpu_sizeof(type_);
    default_ = alloc::alloc("uniform", size);

    memcpy(default_, b.default_, size);
    bound_ = false;
  }

  void move_intern(Uniform &&b) noexcept
  {
    name_ = std::move(name_);
    default_ = b.default_;
    type_ = b.type_;
    bound_ = b.bound_;
    glLoc_ = b.glLoc_;

    b.default_ = nullptr;
    b.type_ = GPUType::TYPE_INVALID;
    b.bound_ = false;
  }

  string name_;
  void *default_ = nullptr;
  GPUType type_ = GPUType::TYPE_INVALID;
  int glLoc_;
  bool bound_ = false;
};

struct ShaderDef {
  string vertexSource, fragmentSource;
  util::Vector<string> attrs;
  util::Vector<Uniform> uniforms;
  util::Map<string, string> defines;
};

struct Shader {
  Shader()
  {
  }

  Shader(const Shader &b)
      : vertexSource_(b.vertexSource_), fragmentSource_(b.fragmentSource_),
        uniforms_(b.uniforms_), attrs_(b.attrs_), defines_(b.defines_),
        is_final_shader_(b.is_final_shader_)
  {
  }

  Shader(Shader &&b)
  {
    vertexSource_ = std::move(b.vertexSource_);
    fragmentSource_ = std::move(b.fragmentSource_);
    attrs_ = std::move(b.attrs_);

  }

  Shader(const ShaderDef &def)
      : vertexSource_(def.vertexSource), fragmentSource_(def.fragmentSource)
  {
    attrs_.reserve(def.attrs.size());
    for (const string &ref : def.attrs) {
      attrs_[ref] = -1;
    }

    uniforms_.reserve(def.uniforms.size());
    for (const Uniform &uniform : def.uniforms) {
      uniforms_[uniform.name()] = uniform;
    }

    defines_.reserve(def.defines.size());
    for (auto &pair : def.defines) {
      defines_[pair.key] = pair.value;
    }
  }

private:
  string vertexSource_, fragmentSource_;
  util::Map<string, int> attrs_;
  util::Map<string, Uniform> uniforms_;
  util::Map<string, string> defines_;
  bool is_final_shader_ = false;
};

} // namespace sculptcore::gpu
