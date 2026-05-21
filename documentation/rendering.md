# GPU rendering — batches, shaders, uniform blocks

The `source/gpu/` module is a thin, backend-agnostic description of "what
to draw." It owns no GPU handles. A backend (currently
`source/vulkan/`) walks these descriptions and translates them into
real API calls. The same description is also reflected through
`litestl::binding` so JS / TypeScript consumers see the same surface
(`typescript/sculptcore/gpu/*`).

This document covers the runtime shape of a frame: who owns what, how
shaders declare uniforms, how the three-layer block system stamps
descriptor-set / binding numbers, and how the Vulkan backend uploads
them.

---

## Object model

```
GPUManager
 └── shaders:   ShaderDef *           (schema — what a pipeline reads)
 └── buffers:   Buffer *              (VBO / IBO bytes; uploaded lazily)
 └── batches:   DrawBatch *
 │    └── commands: DrawCommand *
 │    │    └── attrs: Buffer *
 │    │    └── blocks: UniformBlockInstance *   (set = 2, per-draw)
 │    └── blocks:   UniformBlockInstance *      (set = 1, per-batch)
 └── commands: DrawCommand *

DrawPipeline (optional, transient)
 └── batches: DrawBatch *
 └── blocks:  UniformBlockInstance *            (set = 0, per-pass)
```

- `GPUManager` is the long-lived registry. Producers call
  `createBuffer / createBatch / createCommand`. Destructors self-remove
  from the manager via back-pointers — `alloc::Delete(cmd)` is safe
  from anywhere.
- `DrawBatch` groups commands that share state. The Vulkan backend
  issues `vkCmdBindPipeline` per command, so batching is currently a
  logical grouping, not a perf optimization — but it's where per-batch
  uniforms live, so a batch of 1000 shadowed instances writes its
  light/material UBO once and gets it on every child draw.
- `DrawCommand` is one `vkCmdDraw` (or one `gl.drawArrays` equivalent).
  It carries the shader pointer, vertex buffers in attribute order,
  and the vertex range.
- `DrawPipeline` is the rare top-level object: a single render pass
  collecting batches that share per-pass state (view matrix, time,
  globals). Most callers don't construct one — they pass batches
  directly to the backend.

Ownership: `blocks` are owned heap-allocated `UniformBlockInstance *`s,
deleted by the layer's destructor.

---

## Shaders

`ShaderDef` (`source/gpu/shader.h`) is the GPU-program schema. It pins
together the source(s), vertex attributes, and uniform blocks the
program reads.

```cpp
struct ShaderDef {
  string name;
  string wgslSource;            // canonical source — WebGPU backend reads this
  const uint32_t *spirv;        // native-only; emitted by tools/wgsl-to-spirv.mjs
  size_t spirvSize;

  util::Vector<AttrDef> attrs;              // vertex layout, in `location` order
  util::Vector<UniformBlockDef *> uniforms; // UBO schemas — one per binding
  util::Map<string, string> defines;        // shader-define key/value pairs
};
```

Shaders are constructed once at module init and live in static storage
(see `spatial::makeBasicLineShader` / `makeBasicMeshShader`). The
`ShaderDef` is move-only; everything beyond it goes through pointers.

WGSL is the canonical authoring format. `tools/wgsl-to-spirv.mjs` runs
at build time, emitting both a WGSL string header (for the WebGPU
backend on WASM) and a SPIR-V `uint32_t[]` (for the native Vulkan
backend). Don't hand-write SPIR-V or hand-edit the generated headers.

---

## Uniform blocks

Three GPU APIs, three names for the same concept:
- Vulkan: a descriptor at `(set, binding)` of type `UNIFORM_BUFFER`.
- WebGPU: a bind-group entry of type `buffer`.
- OpenGL: a UBO bound to a binding index.

`gpu/shader.h` models all three uniformly as **named blocks**:

```cpp
struct UniformBlockDef {
  string name;                              // link key (e.g. "DefaultBlock")
  util::Vector<UniformDefBase *> fields;    // schema — owned

  uint32_t set, binding;                    // resolved by the link pass
  uint32_t packedBytes;                     // std140 size, after link
  util::Vector<uint32_t> fieldOffsets;      // parallel to `fields`, std140 offsets
  bool preferPushConstant;                  // backend hint
};

struct UniformBlockInstance {
  UniformBlockDef *def;                     // owned by the instance
  util::Vector<uint8_t> data;               // packed CPU blob, std140-laid-out

  template<typename T> bool set(stringref fieldName, const T &value);
};
```

A `UniformBlockDef` is **both** a schema (declared on `ShaderDef`) and a
descriptor of one provided instance (declared on a layer). Layer
instances start out with only `name` set; the link pass fills in
`fields` (cloned from the shader's schema) plus `(set, binding,
packedBytes, fieldOffsets)` and sizes `data`.

### Cadence by layer

A block declared on `DrawPipeline::blocks` gets `set = 0`. Same name on
`DrawBatch::blocks` → `set = 1`. On `DrawCommand::blocks` → `set = 2`.

Pick the cadence by asking "how often does this value change?":

| Layer        | Set | Example uses                                  |
|--------------|-----|-----------------------------------------------|
| `DrawPipeline` | 0 | View / projection matrix, time, light list    |
| `DrawBatch`    | 1 | Material parameters, per-mesh transform       |
| `DrawCommand`  | 2 | Per-draw color, instance offset, push consts  |

The split lets the backend update each set independently — set 0 is
written once per frame, set 1 once per batch, set 2 per draw. The
Vulkan backend's eventual descriptor-cache will key on this.

The shader can override the set assignment: if `UniformBlockDef::set`
is anything other than `~0u` when the shader is constructed, the link
pass preserves it. (See the spatial shaders, which pin
`set=0, binding=0` to match their WGSL `@group(0) @binding(0)`
declarations.) `binding` always comes from the shader-side schema.

### std140 packing

Field offsets follow std140. The packer
(`uniform_link_detail::std140For`) handles:

| Field            | `elemSize` | align | size |
|------------------|------------|-------|------|
| scalar (f32/i32) | 1          | 4     | 4    |
| vec2             | 2          | 8     | 8    |
| vec3             | 3          | 16    | 12   |
| vec4             | 4          | 16    | 16   |
| mat4             | 16         | 16    | 64   |

`packedBytes` is rounded up to 16. Arrays are not in the table yet —
add to `std140For` when needed.

`UniformDef<T>` carries a `defaultValue`. After link, the instance's
`data` is pre-filled with each field's default; producers only have to
write the values that *change* from defaults.

### Writing values

```cpp
inst->set("drawMatrix", viewProj);    // memcpys at fieldOffsets[i]
inst->set("uColor", float4(...));
```

`set()` does a linear scan of `def->fields` by name (≤16 fields per
block in practice; a hash map would not pay off). Returns `false` if
the field doesn't exist or the link pass hasn't run.

---

## The link pass

`source/gpu/uniform_link.{h,cc}` resolves a `DrawPipeline` (or a single
`DrawCommand`) against its shaders:

```cpp
LinkResult linkPipeline(DrawPipeline &pipe);
LinkResult linkCommand(DrawCommand &cmd);
void       linkShaderDef(ShaderDef *shader);
LinkResult linkInstanceAgainstShader(UniformBlockInstance *inst,
                                     ShaderDef *shader, uint32_t setIndex);
```

What it does, per instance:

1. Find a `UniformBlockDef` named `inst->def->name` on the shader.
2. If the layer instance has no `fields` (declared by name only),
   clone the shader's field schema onto it.
3. Compute std140 layout (`packedBytes`, `fieldOffsets`).
4. Stamp `(set, binding)` — set from the layer cadence (0/1/2), binding
   from the shader.
5. Resize `inst->data` to `packedBytes` and pre-fill with field
   defaults.

**Validation:** every block declared by every shader in the pipeline
must be covered by **exactly one** layer's instance. Missing → link
error. Duplicate (two layers claim the same block) → link error. The
error text names the shader, block, and which side was wrong.

`linkShaderDef` is a one-shot helper for backends that need
`packedBytes` on the shader's own schema without running the full link
pass — useful when constructing a `ShaderDef` (the spatial shaders
call it at the end of their factory).

`linkInstanceAgainstShader` resolves one instance in isolation — used
by the Vulkan backend's backward-compat shim.

---

## Frame flow (Vulkan backend)

Per-frame from the debug app's `Scene::renderHeadless`:

```cpp
backend->beginFrame(target, r, g, b, a);
tree->update(&gpu);                              // builds DrawBatches
backend->draw(tree->getDrawBatch(), uniforms);   // issues commands
backend->endFrame();
```

Inside `VulkanBackend::draw(batch, u)`:

1. For each `DrawCommand` in the batch, call `issue(batch, cmd, u)`.
2. `issue()` resolves the pipeline (one Vulkan pipeline per shader,
   cached on `ShaderDef *`) — `ensurePipeline()` lazily compiles
   SPIR-V, builds a `VkDescriptorSetLayout` from `shader->uniforms[0]`,
   allocates a UBO sized to `block->packedBytes`, allocates a
   descriptor set.
3. Look up the block instance for the shader's first block by name —
   first on `cmd->blocks`, then on `batch->blocks`. If found, memcpy
   `inst->data` into the mapped UBO.
4. If neither layer provided an instance, fall back to
   `writeFromDrawUniforms()` — the **backward-compat shim** that
   populates the UBO from the legacy `DrawUniforms { drawMatrix,
   normalMatrix, uColor }` by matching field names.
5. `vkCmdBindPipeline`, `vkCmdBindDescriptorSets`,
   `vkCmdBindVertexBuffers`, `vkCmdDraw`.

The shim is transitional. Once every caller of `backend->draw(...)`
attaches its own `UniformBlockInstance` to the batch (or command),
`DrawUniforms` and `writeFromDrawUniforms` can be deleted.

### Resource caches

`VulkanBackend` keeps two caches keyed off frontend pointers:

- `buffer_cache_: Map<Buffer *, BufferEntry>` — VBO/IBO + host-visible
  memory, resized lazily when the frontend `Buffer` grows.
- `pipeline_cache_: Map<ShaderDef *, PipelineEntry>` — shader module,
  descriptor set layout, pipeline layout, graphics pipeline,
  per-pipeline UBO, descriptor set.

Both are flushed by `VulkanBackend::invalidate()`, which `vkDeviceWaitIdle`s
first and is called from the destructor or when the swapchain is
recreated.

---

## Adding a new uniform block

End-to-end recipe for "add a per-batch material struct to the mesh
shader":

1. **WGSL:** declare the new UBO in `basic_mesh.wgsl`. Pick an unused
   `(set, binding)` pair (currently the only block is at
   `@group(0) @binding(0)`; the next would be `@group(1) @binding(0)`).

2. **SPIR-V:** rerun the WGSL-to-SPIR-V tool. Build will refuse to
   pick up the new shader source otherwise.

3. **Shader schema:** in `spatial_shaders.cc`, build a second
   `UniformBlockDef` named e.g. `"Material"` with the fields, set
   `block->set = 1; block->binding = 0;`, and add it to the
   `ShaderDef`'s `uniforms` vector. Call `linkShaderDef(&def)` after
   construction so `packedBytes` is computed.

4. **Provide an instance:** in whatever code builds the batch
   (`spatial/spatial.cc` / sculpt overlay / etc.), allocate a
   `UniformBlockDef` with the same name and add a
   `UniformBlockInstance` to `batch->blocks`.

5. **Write values:** `inst->set("roughness", 0.4f);` etc. The link
   pass runs lazily (or call `linkPipeline` / `linkCommand`
   explicitly); after that, `inst->data` is std140-laid-out and ready
   to upload.

6. **Backend:** the current Vulkan backend's `ensurePipeline` only
   wires the shader's *first* block. Generalising it to all
   `shader->uniforms[i]` is the next backend task — see the
   `PipelineEntry` cache and the descriptor-set-layout construction in
   `vk_backend.cc`.

---

## Tests

`tests/test_uniform_link.cc` is the canonical reference for the link
pass and std140 packing. Covers:

- `std140For` align/size table for scalar / vec2 / vec3 / vec4 / mat4
- `computeStd140Layout` offsets and `packedBytes` on a mat4+mat4+vec4 block
- Single-command link, default fill, `set("uColor", ...)` overwrite
- Missing-block link error, wrong-block-name link error
- Pipeline-level set assignment (`set == 0` for `pipe.blocks`)

When extending the link pass (new field types, push-constant routing,
cadence overrides), add the regression case here first.

`test_debug_script.cc` exercises the full Vulkan path — a green smoke
test that any breakage to the render pipeline shows up in CI.

---

## Glossary

- **Block** — a named group of uniform fields, mapped to one UBO
  (or one push-constant range) on the GPU. Both the shader-side
  schema and the layer-side instance use the `UniformBlockDef` /
  `UniformBlockInstance` types.
- **Layer** — `DrawPipeline`, `DrawBatch`, or `DrawCommand` — i.e.
  one of the three update cadences a block can live at.
- **Link** — the resolution step that matches layer blocks to shader
  blocks by name, computes std140 offsets, and stamps
  `(set, binding)`. Must succeed before a draw can be issued.
- **DefaultBlock** — by convention, the single block read by the
  spatial shaders. New shaders should pick more specific names
  (`"View"`, `"Material"`, `"Instance"`).
