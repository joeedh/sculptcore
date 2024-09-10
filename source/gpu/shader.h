#pragma once

#include "math/vector.h"

#include "util/alloc.h"
#include "util/boolvector.h"
#include "util/compiler_util.h"
#include "util/map.h"
#include "util/string.h"
#include "util/vector.h"

#include "gpu/types.h"
using namespace litestl;

namespace sculptcore::gpu {
using litestl::util::string;
using litestl::util::stringref;

struct Uniform {
  Uniform() noexcept
  {
  }

  Uniform(const Uniform &b)
  {
    type_ = b.type_;
    size_t size = gpu_sizeof(type_);
    default_ = alloc::alloc("uniform", size);

    memcpy(default_, b.default_, size);
    bound_ = false;
  }

  Uniform(Uniform &&b)
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

  DEFAULT_MOVE_ASSIGNMENT(Uniform)

  Uniform &operator=(const Uniform &b) noexcept
  {
    if (this == &b) {
      return *this;
    }

    this->~Uniform();

    /* Forward to copy constructor. */
    new (static_cast<void *>(this)) Uniform(b);

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
    uniforms_ = std::move(b.uniforms_);
    defines_ = std::move(b.defines_);
    is_final_shader_ = b.is_final_shader_;
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

  Shader createFinalShader()
  {
    Shader cpy = *this;

    for (auto &pair : defines_) {
      string line = "#define " + pair.key;
      if (pair.value.size() > 0) {
        line += " = " + pair.value;
      }
      line += "\n";

      vertexSource_ = line + vertexSource_;
      fragmentSource_ = line + fragmentSource_;
    }

    is_final_shader_ = true;

    return cpy;
  }

  util::StringKey createShaderKey() const
  {
    // using namespace sculptcore::util;
    using litestl::util::get_stringkey;

    char key[256];

    string ret;

    sprintf(key,
            "%d:%d:\n",
            int(get_stringkey(fragmentSource_)),
            int(get_stringkey(vertexSource_)));

    ret += key;

    auto int2str = [](int d) {
      char buf[256];
      sprintf(buf, "%d", d);
      return string(buf);
    };

    /* Client code might dynamically add more attributes. */
    for (auto &pair : attrs_) {
      ret += int2str(get_stringkey(pair.key)) + ":";
    }

    for (auto &pair : defines_) {
      ret += int2str(get_stringkey(pair.key)) + ":";
    }

    return get_stringkey(ret);
  }

private:
  string vertexSource_, fragmentSource_;
  util::Map<string, int> attrs_;
  util::Map<string, Uniform> uniforms_;
  util::Map<string, string> defines_;
  bool is_final_shader_ = false;
};

Shader *get_cached_shader(const Shader &shader);
void set_cached_shader(Shader *shader);

} // namespace sculptcore::gpu
