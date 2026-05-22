#pragma once
#include "binding/binding_types.h"
#include "litestl/binding/binding_struct.h"
#include "litestl/util/alloc.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"
#include "types.h"

namespace sculptcore::gpu {
struct ShaderDef;
struct Buffer;
struct DrawBatch;
struct DrawCommand;
enum class GPUCmdType;

using litestl::util::Vector;

/** Interface a backend implements to learn when GPU-side resources owned by
 *  a GPUManager are destroyed. Lets the backend invalidate its per-resource
 *  caches before the pointer is freed and (for Vulkan) defer destruction of
 *  the native handle until it is no longer referenced by an in-flight or
 *  recording command buffer. */
struct GPUResourceObserver {
  virtual ~GPUResourceObserver() = default;
  virtual void onBufferDestroyed(Buffer *buffer) = 0;
};

struct GPUManager {
  Vector<ShaderDef *> shaders;
  Vector<Buffer *> buffers;
  Vector<DrawBatch *> batches;
  Vector<DrawCommand *> commands;
  Vector<GPUResourceObserver *> observers;

  GPUManager() = default;
  ~GPUManager();

  void addObserver(GPUResourceObserver *o) { observers.append(o); }
  void removeObserver(GPUResourceObserver *o)
  {
    if (observers.contains(o)) {
      observers.remove(o);
    }
  }

  Buffer *
  createBuffer(litestl::util::string name, GPUType type, int elemsize, int elemCount);
  DrawBatch *createBatch();
  DrawCommand *createCommand(DrawBatch *batch,
                             GPUCmdType type,
                             ShaderDef *shader,
                             int start,
                             int end,
                             int primCount);

  void destroyBuffer(Buffer *buffer);
  void destroyCommand(DrawCommand *cmd, bool destroy_buffers);
  void destroyBatch(DrawBatch *batch, bool destroy_commands, bool destroy_buffers);

  /** used by move constructors/operators in resource classes */
  template <typename T> void transferOwnership(Vector<T *> &vec, T *dst, T *src)
  {
    if (vec.contains(src)) {
      vec.remove(src);
    }
    if (!vec.contains(dst)) {
      vec.append(dst);
    }
  }

  static litestl::binding::types::Struct<GPUManager> *defineBindings();
};
} // namespace sculptcore::gpu
