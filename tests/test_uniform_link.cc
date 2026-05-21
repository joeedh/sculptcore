#include "test_util.h"

#include "gpu/batch.h"
#include "gpu/command.h"
#include "gpu/manager.h"
#include "gpu/pipeline.h"
#include "gpu/shader.h"
#include "gpu/uniform_link.h"

#include "litestl/math/matrix.h"
#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/string.h"

#include <cstdio>
#include <cstring>

test_init;

using namespace sculptcore::gpu;
using namespace litestl::math;
using litestl::alloc::New;
using litestl::util::Vector;

/* Build a UniformBlockDef matching the spatial DefaultBlock — drawMatrix
 * (mat4) + uColor (vec4) for the line variant. */
static UniformBlockDef *makeLineDefaultBlock()
{
  Vector<UniformDefBase *> fields;
  fields.append(New<UniformDef<mat4>>(
      "UniformDef", "drawMatrix", GPUType::FLOAT32, 16, mat4().identity()));
  fields.append(New<UniformDef<float4>>(
      "UniformDef", "uColor", GPUType::FLOAT32, 4, float4(0.5f, 0.5f, 0.5f, 1.0f)));
  return New<UniformBlockDef>("UniformBlockDef",
                              litestl::util::string("DefaultBlock"),
                              std::move(fields));
}

static UniformBlockDef *makeMeshDefaultBlock()
{
  Vector<UniformDefBase *> fields;
  fields.append(New<UniformDef<mat4>>(
      "UniformDef", "drawMatrix", GPUType::FLOAT32, 16, mat4().identity()));
  fields.append(New<UniformDef<mat4>>(
      "UniformDef", "normalMatrix", GPUType::FLOAT32, 16, mat4().identity()));
  fields.append(New<UniformDef<float4>>(
      "UniformDef", "uColor", GPUType::FLOAT32, 4, float4(1.0f, 1.0f, 1.0f, 1.0f)));
  return New<UniformBlockDef>("UniformBlockDef",
                              litestl::util::string("DefaultBlock"),
                              std::move(fields));
}

static ShaderDef makeShader(const char *name, UniformBlockDef *block)
{
  Vector<UniformBlockDef *> blocks;
  blocks.append(block);
  return ShaderDef(name,
                   "/* test */",
                   /*attrs=*/{},
                   std::move(blocks),
                   /*defines=*/{});
}

static void test_std140_layout()
{
  using namespace sculptcore::gpu::uniform_link_detail;

  /* scalar */
  FieldLayout fl = std140For(int(GPUType::FLOAT32), 1);
  test_assert(fl.align == 4 && fl.size == 4);

  /* vec2 */
  fl = std140For(int(GPUType::FLOAT32), 2);
  test_assert(fl.align == 8 && fl.size == 8);

  /* vec3: align 16, size 12 */
  fl = std140For(int(GPUType::FLOAT32), 3);
  test_assert(fl.align == 16 && fl.size == 12);

  /* vec4 */
  fl = std140For(int(GPUType::FLOAT32), 4);
  test_assert(fl.align == 16 && fl.size == 16);

  /* mat4: align 16, size 64 */
  fl = std140For(int(GPUType::FLOAT32), 16);
  test_assert(fl.align == 16 && fl.size == 64);
}

static void test_layout_on_block()
{
  using namespace sculptcore::gpu::uniform_link_detail;

  UniformBlockDef *block = makeMeshDefaultBlock();
  computeStd140Layout(block);

  /* drawMatrix @ 0, normalMatrix @ 64, uColor @ 128 — total 144 → padded to 144 (already 16-aligned). */
  test_assert(block->fieldOffsets.size() == 3);
  test_assert(block->fieldOffsets[0] == 0);
  test_assert(block->fieldOffsets[1] == 64);
  test_assert(block->fieldOffsets[2] == 128);
  test_assert(block->packedBytes == 144);

  litestl::alloc::Delete(block);
}

static void test_link_command_default_fill()
{
  /* Wire a shader → command and check that defaults populate the blob. */
  ShaderDef shader = makeShader("TestLine", makeLineDefaultBlock());

  DrawCommand cmd;
  cmd.shader = &shader;
  UniformBlockDef *layerDef =
      New<UniformBlockDef>("UniformBlockDef", litestl::util::string("DefaultBlock"));
  UniformBlockInstance *inst =
      New<UniformBlockInstance>("UniformBlockInstance", layerDef);
  cmd.blocks.append(inst);

  LinkResult r = linkCommand(cmd);
  if (!r.ok) {
    fprintf(stderr, "link failed: %s\n", r.error.c_str());
  }
  test_assert(r.ok);
  test_assert(layerDef->set == 2);
  test_assert(layerDef->packedBytes == 80); /* mat4 + vec4 = 64 + 16 */
  test_assert(layerDef->fieldOffsets.size() == 2);
  test_assert(layerDef->fieldOffsets[0] == 0);
  test_assert(layerDef->fieldOffsets[1] == 64);
  test_assert(inst->data.size() == 80);

  /* uColor default is (0.5, 0.5, 0.5, 1.0). */
  float defColor[4];
  std::memcpy(defColor, inst->data.data() + 64, sizeof(defColor));
  test_assert(defColor[0] == 0.5f);
  test_assert(defColor[1] == 0.5f);
  test_assert(defColor[2] == 0.5f);
  test_assert(defColor[3] == 1.0f);

  /* Producer overwrite via set(). */
  float4 newColor(0.25f, 0.5f, 0.75f, 1.0f);
  bool wrote = inst->set("uColor", newColor);
  test_assert(wrote);
  std::memcpy(defColor, inst->data.data() + 64, sizeof(defColor));
  test_assert(defColor[0] == 0.25f);
  test_assert(defColor[2] == 0.75f);

  /* Self-cleanup: cmd destructor frees inst → layerDef → fields. */
}

static void test_link_missing_block()
{
  ShaderDef shader = makeShader("TestLine", makeLineDefaultBlock());

  DrawCommand cmd;
  cmd.shader = &shader;
  /* No instance for DefaultBlock → link should fail. */
  LinkResult r = linkCommand(cmd);
  test_assert(!r.ok);
}

static void test_link_wrong_name()
{
  ShaderDef shader = makeShader("TestLine", makeLineDefaultBlock());

  DrawCommand cmd;
  cmd.shader = &shader;
  UniformBlockDef *bad =
      New<UniformBlockDef>("UniformBlockDef", litestl::util::string("WrongName"));
  cmd.blocks.append(New<UniformBlockInstance>("UniformBlockInstance", bad));

  LinkResult r = linkCommand(cmd);
  test_assert(!r.ok);
}

static void test_link_pipeline_set_assignment()
{
  /* Two shaders sharing a DefaultBlock. Put the block on the *pipeline*, not
   * the command, and verify set=0. */
  UniformBlockDef *sharedShaderBlock1 = makeLineDefaultBlock();
  UniformBlockDef *sharedShaderBlock2 = makeLineDefaultBlock();
  ShaderDef line = makeShader("TestLine", sharedShaderBlock1);
  ShaderDef mesh = makeShader("TestLine2", sharedShaderBlock2);

  litestl::util::Vector<UniformBlockDef *> _; /* silence unused warning */
  (void)_;

  /* Build a pipeline with one batch and two commands. */
  GPUManager gpu;
  DrawBatch *batch = gpu.createBatch();
  DrawCommand *c1 = gpu.createCommand(batch, GPUCmdType::DRAW_LINES, &line, 0, 0, 0);
  DrawCommand *c2 = gpu.createCommand(batch, GPUCmdType::DRAW_LINES, &mesh, 0, 0, 0);
  (void)c1;
  (void)c2;

  DrawPipeline pipe;
  pipe.batches.append(batch);

  UniformBlockDef *layerDef =
      New<UniformBlockDef>("UniformBlockDef", litestl::util::string("DefaultBlock"));
  UniformBlockInstance *inst =
      New<UniformBlockInstance>("UniformBlockInstance", layerDef);
  pipe.blocks.append(inst);

  LinkResult r = linkPipeline(pipe);
  if (!r.ok) {
    fprintf(stderr, "pipeline link failed: %s\n", r.error.c_str());
  }
  test_assert(r.ok);
  test_assert(layerDef->set == 0);
}

int main()
{
  test_std140_layout();
  test_layout_on_block();
  test_link_command_default_fill();
  test_link_missing_block();
  test_link_wrong_name();
  test_link_pipeline_set_assignment();
  return test_end();
}
