#include "brush/brush_executor.h"
#include "debug/scene.h"
#include "debug/script.h"
#include "test_util.h"

#include <cstring>

test_init;
using namespace sculptcore;
using namespace sculptcore::brush;

extern "C" int MeshStroke_dabBatchProgram(CommandExecutor *,
                                          spatial::SpatialTree *,
                                          mesh::Mesh *,
                                          Brush *,
                                          BrushProgram *,
                                          int,
                                          const float *,
                                          float,
                                          int,
                                          float,
                                          int,
                                          float,
                                          const float *,
                                          int);

int main()
{
  for (bool reverse : {false, true}) {
    for (bool nonAccum : {false, true}) {
      Brush brush;
      CommandExecutor executor(nullptr, &brush);
      executor.setNonAccum(nonAccum);
      brush.planeSide = 7;
      brush.wingAngle = 0.25f;
      brush.activeGroup = 19;
      brush.mixMode = 3;
      for (int index = 0; index < SculptBrushesBuiltinCount; index++) {
        int tool = reverse ? SculptBrushesBuiltinCount - 1 - index : index;
        auto command = executor.createCommand(SculptBrushes(tool));
        auto result = command.registerProps(*brush.props.struct_def);
        test_assert(result.error == props::PropError::ERROR_NONE);
        Vector<props::ScalarDeclaration> declarations;
        command.appendScalarDeclarations(declarations);
        for (size_t i = 0; i < declarations.size(); i++) {
          for (size_t j = 0; j < i; j++) {
            test_assert(declarations[i].name != declarations[j].name);
          }
        }
        command.loadUniformProps(brush, &brush.deviceInputCtx);
      }
      test_assert(brush.planeSide == 7 && brush.wingAngle == 0.25f &&
                  brush.activeGroup == 19 && brush.mixMode == 3);
      auto command = executor.createCommand(SculptBrushes::KELVINLET);
      test_assert(command.uniforms[0].scalarType == props::Prop::FLOAT32 &&
                  command.uniforms[0].hasDefault);
      test_assert(command.uniforms[1].def == 0.4 &&
                  command.uniforms[1].rangeMax == 0.499);
      test_assert(!command.uniforms[2].hasDefault);
    }
  }
  {
    Brush brush;
    brush.props.struct_def->Bool("nu", "Conflict", -1);
    CommandExecutor executor(nullptr, &brush);
    test_assert(executor.queryUniformManifest(int(SculptBrushes::KELVINLET)) == -1);
    test_assert(!executor.lastUniformValidationOk() && !executor.queriedUniformEntry(0));
    test_assert(!brush.props.struct_def->has("mu"));
  }
#ifdef SCULPTCORE_EXTRA_BRUSHES
  {
    Brush brush;
    brush.props.struct_def->Bool("projection", "Conflict", -1);
    CommandExecutor executor(nullptr, &brush);
    BrushProgram program;
    program.addCommand(SculptBrushesBuiltinCount);
    program.addCommand(int(SculptBrushes::BSMOOTH));
    test_assert(!executor.prepareProgramDeclarations(&program));
    test_assert(brush.namedFloats.size() == 0);
    fprintf(stderr, "extra-kernel failed registration preserves working slots\n");
  }
#endif
  {
    Brush brush;
    brush.props.struct_def->Float32("gain", "Gain", -1).Min(1).Max(2).Default(1.5f);
    CommandExecutor executor(nullptr, &brush);
    auto command = executor.createCommand(SculptBrushes::DRAW);
    command.uniforms.append(BrushUniformManifestEntry{
        "gain", true, true, 0, true, 1, 2, -1, props::Prop::FLOAT32, false});
    test_assert(command.registerProps(*brush.props.struct_def).error ==
                props::PropError::ERROR_NONE);
    test_assert(executor.validateUniformDynamics(command).ok);
    test_assert(brush.props.lookupFloat("gain", -1) == 1.5f);
  }
  for (bool reverse : {false, true}) {
    debug_app::Scene scene(256, 256, true);
    auto setup = debug_app::script::run(
        scene,
        "make_cube subdivs=4 size=0.5\nbuild_spatial leaf_limit=64 depth_limit=8\n"
        "set_brush_tool tool=draw\nset_brush radius=0.25 strength=1.0\nset_backend "
        "backend=cpp\n",
        ".");
    test_assert(setup.ok);
    auto &brush = scene.brush;
    brush.props.struct_def->Bool("projection", "Conflict", -1);
    CommandExecutor executor(scene.tree, &brush);
    executor.meshLog = &scene.meshLog;
    int undoStep = scene.meshLog.lastStepId();
    double undoBytes = scene.meshLog.totalMemSize();
    BrushProgram program;
    program.addCommand(int(reverse ? SculptBrushes::BSMOOTH : SculptBrushes::KELVINLET));
    program.addCommand(int(reverse ? SculptBrushes::KELVINLET : SculptBrushes::BSMOOTH));
    program.setCommandFloat(0, 0, 0.125f);
    Vector<float3> before;
    for (int i = 0; i < scene.mesh->v.count; i++) {
      before.append(scene.mesh->v.co[i]);
    }
    int edgeCount = scene.mesh->e.count, faceCount = scene.mesh->f.count;
    int sampleCount = brush.strokePathCount;
    float strength = brush.strength;
    const float batchDab[] = {0, 0, 0.25f, 0, 0, 1, 0.25f};
    test_assert(MeshStroke_dabBatchProgram(&executor,
                                           scene.tree,
                                           scene.mesh,
                                           &brush,
                                           &program,
                                           1,
                                           batchDab,
                                           0.123f,
                                           1,
                                           0.25f,
                                           1,
                                           1,
                                           nullptr,
                                           0) == -1);
    test_assert(brush.strength == strength);
    dyntopo::DynTopoParams topology;
    for (float3 origin : {float3(99), float3(0, 0, 0.25f)}) {
      test_assert(executor.applyDab(
                      &program, origin, float3(0, 0, 1), 0.25f, &topology, 17) == -1);
      executor.clearIsFirstOfStep();
    }
    test_assert(!brush.props.struct_def->has("mu"));
    test_assert(!brush.props.struct_def->has("nu"));
    test_assert(brush.strokePathCount == sampleCount &&
                program.commands[0].floatOverrides[0].value == 0.125f);
    test_assert(scene.mesh->v.count == before.size() &&
                scene.mesh->e.count == edgeCount && scene.mesh->f.count == faceCount);
    for (int i = 0; i < scene.mesh->v.count; i++) {
      test_assert(
          std::memcmp(&scene.mesh->v.co[i][0], &before[i][0], 3 * sizeof(float)) == 0);
    }
    test_assert(scene.meshLog.lastStepId() == undoStep &&
                scene.meshLog.totalMemSize() == undoBytes);
    BrushProgram changed;
    changed.addCommand(int(SculptBrushes::DRAW));
    test_assert(executor.prepareProgramDeclarations(&changed));
    test_assert(executor.lastUniformValidationOk());
    changed.addCommand(int(SculptBrushes::BSMOOTH));
    test_assert(!executor.prepareProgramDeclarations(&changed));
  }
  return test_end();
}
