#include "uniform_link.h"

#include "batch.h"
#include "command.h"
#include "pipeline.h"
#include "shader.h"

#include "litestl/math/matrix.h"
#include "litestl/math/vector.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cstdio>
#include <cstring>

namespace sculptcore::gpu {

using litestl::util::string;
using litestl::util::Vector;

namespace uniform_link_detail {

static uint32_t alignUp(uint32_t v, uint32_t a)
{
  if (a <= 1) {
    return v;
  }
  return (v + (a - 1)) & ~(a - 1);
}

static uint32_t baseSizeOf(GPUType t)
{
  switch (t) {
  case GPUType::FLOAT32:
  case GPUType::INT32:
  case GPUType::UINT32:
    return 4;
  case GPUType::FLOAT16:
  case GPUType::INT16:
  case GPUType::UINT16:
    return 2;
  case GPUType::INT8:
  case GPUType::UINT8:
    return 1;
  case GPUType::FLOAT64:
    return 8;
  default:
    return 4;
  }
}

FieldLayout std140For(int gpuType, int elemSize)
{
  GPUType t = GPUType(gpuType);
  uint32_t b = baseSizeOf(t);

  /* Conventions matching how the codebase declares its uniforms:
   *  elemSize 1     → scalar
   *  elemSize 2     → vec2
   *  elemSize 3     → vec3 (size 12, align 16)
   *  elemSize 4     → vec4
   *  elemSize 16    → mat4 (four vec4 columns, align 16, size 64) */
  switch (elemSize) {
  case 1:
    return {b, b};
  case 2:
    return {b * 2, b * 2};
  case 3:
    return {b * 4, b * 3};
  case 4:
    return {b * 4, b * 4};
  case 16:
    /* mat4 — four vec4 columns. Aligned to vec4, size = 4 * 16. */
    return {b * 4, b * 4 * 4};
  default:
    /* Unknown — treat as aligned-as-vec4 array of elemSize scalars. */
    return {b * 4, b * uint32_t(elemSize)};
  }
}

void computeStd140Layout(UniformBlockDef *block)
{
  block->fieldOffsets.clear();

  uint32_t offset = 0;
  for (auto *f : block->fields) {
    FieldLayout fl = std140For(int(f->type), f->elemSize);
    offset = alignUp(offset, fl.align);
    block->fieldOffsets.append(offset);
    offset += fl.size;
  }
  /* UBO total rounded up to 16 (vec4 alignment) — matches std140. */
  block->packedBytes = alignUp(offset, 16u);
}

/* Per-(GPUType, elemSize) memcpy from `defaultValue`. The pair determines the
 * concrete T that UniformDef<T> was instantiated with — keep this dispatch
 * table in sync with the instantiations registered in shader.cc. */
template <typename T>
static void copyDefault(const UniformDefBase *base, void *dst, size_t maxBytes)
{
  const auto *typed = static_cast<const UniformDef<T> *>(base);
  if (sizeof(T) <= maxBytes) {
    std::memcpy(dst, &typed->defaultValue, sizeof(T));
  }
}

void fillDefaults(const UniformBlockDef *block, void *data, size_t bytes)
{
  using namespace litestl::math;

  if (!data || bytes == 0) {
    return;
  }
  std::memset(data, 0, bytes);

  for (size_t i = 0; i < block->fields.size(); i++) {
    const UniformDefBase *f = block->fields[i];
    uint32_t off = block->fieldOffsets[i];
    if (off >= bytes) {
      continue;
    }
    void *dst = static_cast<uint8_t *>(data) + off;
    size_t maxBytes = bytes - off;

    if (f->type == GPUType::FLOAT32) {
      switch (f->elemSize) {
      case 1:
        copyDefault<float>(f, dst, maxBytes);
        break;
      case 2:
        copyDefault<float2>(f, dst, maxBytes);
        break;
      case 3:
        copyDefault<float3>(f, dst, maxBytes);
        break;
      case 4:
        copyDefault<float4>(f, dst, maxBytes);
        break;
      case 16:
        copyDefault<mat4>(f, dst, maxBytes);
        break;
      default:
        break;
      }
    }
    /* Other GPUTypes have no UniformDef<T> instantiations yet (see shader.cc).
     * When new types are added, extend both this switch and the union in
     * createUniformBindTypeUnion(). */
  }
}

} // namespace uniform_link_detail

/* Find a block by name in a Vector of UniformBlockDef* (shader.uniforms). */
static UniformBlockDef *findShaderBlock(ShaderDef *shader, const string &name)
{
  if (!shader) {
    return nullptr;
  }
  for (auto *b : shader->uniforms) {
    if (b->name == name) {
      return b;
    }
  }
  return nullptr;
}

/* Resolve a single instance against a shader. Stamps `set`, fills missing
 * field schema from the shader, computes layout, resizes data, fills
 * defaults. */
static LinkResult
resolveInstance(UniformBlockInstance *inst, ShaderDef *shader, uint32_t setIndex)
{
  LinkResult r;
  if (!inst || !inst->def) {
    r.ok = false;
    r.error = "uniform block instance is null";
    return r;
  }

  UniformBlockDef *layerDef = inst->def;
  UniformBlockDef *shaderDef = findShaderBlock(shader, layerDef->name);
  if (!shaderDef) {
    r.ok = false;
    r.error = string("shader '") + (shader ? shader->name.c_str() : "(null)") +
              "' has no block named '" + layerDef->name + "'";
    return r;
  }

  /* If the layer didn't supply its own fields (declared only by name),
   * inherit the shader's schema. The fields are *copied* — the layer's def
   * owns them, so we duplicate each UniformDef<T> via the GPUType/elemSize
   * dispatch (mirroring fillDefaults). */
  if (layerDef->fields.size() == 0 && shaderDef->fields.size() > 0) {
    using namespace litestl::math;
    for (auto *src : shaderDef->fields) {
      UniformDefBase *clone = nullptr;
      if (src->type == GPUType::FLOAT32) {
        switch (src->elemSize) {
        case 1:
          clone = litestl::alloc::New<UniformDef<float>>(
              "UniformDef",
              src->name,
              src->type,
              src->elemSize,
              static_cast<UniformDef<float> *>(src)->defaultValue);
          break;
        case 2:
          clone = litestl::alloc::New<UniformDef<float2>>(
              "UniformDef",
              src->name,
              src->type,
              src->elemSize,
              static_cast<UniformDef<float2> *>(src)->defaultValue);
          break;
        case 3:
          clone = litestl::alloc::New<UniformDef<float3>>(
              "UniformDef",
              src->name,
              src->type,
              src->elemSize,
              static_cast<UniformDef<float3> *>(src)->defaultValue);
          break;
        case 4:
          clone = litestl::alloc::New<UniformDef<float4>>(
              "UniformDef",
              src->name,
              src->type,
              src->elemSize,
              static_cast<UniformDef<float4> *>(src)->defaultValue);
          break;
        case 16:
          clone = litestl::alloc::New<UniformDef<mat4>>(
              "UniformDef",
              src->name,
              src->type,
              src->elemSize,
              static_cast<UniformDef<mat4> *>(src)->defaultValue);
          break;
        default:
          break;
        }
      }
      if (clone) {
        layerDef->fields.append(clone);
      }
    }
  } else if (layerDef->fields.size() != shaderDef->fields.size()) {
    r.ok = false;
    r.error = string("block '") + layerDef->name + "' field count mismatch: layer has " +
              std::to_string(layerDef->fields.size()).c_str() + ", shader expects " +
              std::to_string(shaderDef->fields.size()).c_str();
    return r;
  }

  /* Compute layout on the layer's def (now populated) and stamp the resolved
   * (set, binding). The shader's binding wins; set is dictated by layer. */
  uniform_link_detail::computeStd140Layout(layerDef);
  layerDef->set = setIndex;
  layerDef->binding = shaderDef->binding;

  inst->data.resize(layerDef->packedBytes);
  uniform_link_detail::fillDefaults(layerDef, inst->data.data(), inst->data.size());
  return r;
}

/* Track which shader blocks are covered. We key by (shader pointer, block
 * name) so the same name on different shaders is fine (e.g. line and mesh
 * shaders each have their own DefaultBlock). */
struct CoverageKey {
  ShaderDef *shader;
  string name;
  bool operator==(const CoverageKey &o) const
  {
    return shader == o.shader && name == o.name;
  }
};

static bool keyEq(const Vector<CoverageKey> &keys, ShaderDef *s, const string &n)
{
  for (const auto &k : keys) {
    if (k.shader == s && k.name == n) {
      return true;
    }
  }
  return false;
}

void linkShaderDef(ShaderDef *shader)
{
  if (!shader) {
    return;
  }
  for (auto *block : shader->uniforms) {
    if (!block) {
      continue;
    }
    uniform_link_detail::computeStd140Layout(block);
  }
}

LinkResult linkInstanceAgainstShader(UniformBlockInstance *inst,
                                     ShaderDef *shader,
                                     uint32_t setIndex)
{
  return resolveInstance(inst, shader, setIndex);
}

LinkResult linkCommand(DrawCommand &cmd)
{
  LinkResult r;
  if (!cmd.shader) {
    if (cmd.blocks.size() > 0) {
      r.ok = false;
      r.error = "command has uniform blocks but no shader";
    }
    return r;
  }
  for (auto *inst : cmd.blocks) {
    r = resolveInstance(inst, cmd.shader, /*setIndex=*/2);
    if (!r.ok) {
      return r;
    }
  }
  /* Validate the command's shader has every block it declares covered. */
  for (auto *sb : cmd.shader->uniforms) {
    bool covered = false;
    for (auto *inst : cmd.blocks) {
      if (inst && inst->def && inst->def->name == sb->name) {
        covered = true;
        break;
      }
    }
    if (!covered) {
      r.ok = false;
      r.error = string("shader '") + cmd.shader->name + "' block '" + sb->name +
                "' is not provided by any layer";
      return r;
    }
  }
  return r;
}

LinkResult linkPipeline(DrawPipeline &pipe)
{
  LinkResult r;
  Vector<CoverageKey> covered;

  /* Pipeline blocks resolve against each unique shader appearing in any
   * descendant command — the name must agree across all of them. We
   * implement that by binding each pipeline/batch block once per shader,
   * walking through all unique shaders below. */
  Vector<ShaderDef *> shaders;
  for (auto *batch : pipe.batches) {
    if (!batch) {
      continue;
    }
    for (auto *cmd : batch->commands) {
      if (cmd && cmd->shader && !shaders.contains(cmd->shader)) {
        shaders.append(cmd->shader);
      }
    }
  }

  /* Pipeline (set=0) blocks must resolve identically against every shader. */
  for (auto *inst : pipe.blocks) {
    if (!inst || !inst->def) {
      r.ok = false;
      r.error = "pipeline block instance is null";
      return r;
    }
    ShaderDef *firstHit = nullptr;
    for (auto *sh : shaders) {
      UniformBlockDef *sb = findShaderBlock(sh, inst->def->name);
      if (sb) {
        firstHit = sh;
        break;
      }
    }
    if (!firstHit) {
      r.ok = false;
      r.error = string("pipeline block '") + inst->def->name +
                "' not declared by any shader in the pipeline";
      return r;
    }
    r = resolveInstance(inst, firstHit, /*setIndex=*/0);
    if (!r.ok) {
      return r;
    }
    for (auto *sh : shaders) {
      if (UniformBlockDef *sb = findShaderBlock(sh, inst->def->name)) {
        if (keyEq(covered, sh, sb->name)) {
          r.ok = false;
          r.error = string("block '") + sb->name + "' covered twice for shader '" +
                    sh->name + "'";
          return r;
        }
        covered.append({sh, sb->name});
      }
    }
  }

  for (auto *batch : pipe.batches) {
    if (!batch) {
      continue;
    }
    /* Batch (set=1) blocks must resolve against every shader in the batch. */
    Vector<ShaderDef *> batchShaders;
    for (auto *cmd : batch->commands) {
      if (cmd && cmd->shader && !batchShaders.contains(cmd->shader)) {
        batchShaders.append(cmd->shader);
      }
    }
    for (auto *inst : batch->blocks) {
      if (!inst || !inst->def) {
        r.ok = false;
        r.error = "batch block instance is null";
        return r;
      }
      ShaderDef *firstHit = nullptr;
      for (auto *sh : batchShaders) {
        if (findShaderBlock(sh, inst->def->name)) {
          firstHit = sh;
          break;
        }
      }
      if (!firstHit) {
        r.ok = false;
        r.error = string("batch block '") + inst->def->name +
                  "' not declared by any shader in the batch";
        return r;
      }
      r = resolveInstance(inst, firstHit, /*setIndex=*/1);
      if (!r.ok) {
        return r;
      }
      for (auto *sh : batchShaders) {
        if (UniformBlockDef *sb = findShaderBlock(sh, inst->def->name)) {
          if (keyEq(covered, sh, sb->name)) {
            r.ok = false;
            r.error = string("block '") + sb->name +
                      "' covered twice (pipeline+batch) for shader '" + sh->name + "'";
            return r;
          }
          covered.append({sh, sb->name});
        }
      }
    }

    for (auto *cmd : batch->commands) {
      if (!cmd) {
        continue;
      }
      for (auto *inst : cmd->blocks) {
        if (!inst || !inst->def) {
          r.ok = false;
          r.error = "command block instance is null";
          return r;
        }
        if (!cmd->shader) {
          r.ok = false;
          r.error = "command has blocks but no shader";
          return r;
        }
        if (keyEq(covered, cmd->shader, inst->def->name)) {
          r.ok = false;
          r.error = string("block '") + inst->def->name +
                    "' on command duplicates an outer-layer coverage for shader '" +
                    cmd->shader->name + "'";
          return r;
        }
        r = resolveInstance(inst, cmd->shader, /*setIndex=*/2);
        if (!r.ok) {
          return r;
        }
        covered.append({cmd->shader, inst->def->name});
      }
    }
  }

  /* Every shader-declared block must be covered exactly once. */
  for (auto *sh : shaders) {
    for (auto *sb : sh->uniforms) {
      if (!keyEq(covered, sh, sb->name)) {
        r.ok = false;
        r.error = string("shader '") + sh->name + "' block '" + sb->name +
                  "' is not provided by any layer";
        return r;
      }
    }
  }

  return r;
}

} // namespace sculptcore::gpu
