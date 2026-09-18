#pragma once

#include "brush/brush_program_preparation.h"

static void cavityPreparation()
{
  Brush brush;
  brush.cavity_factor = 1.25f;
  brush.cavity_blur_steps = 3;
  brush.cavity_inverted = true;
  auto &property = brush.props.struct_def->Float32("cavity_factor", "Cavity", -1);
  property.min = 10;
  property.max = 11;
  *property.internal_value() = 10.5f;
  PreparedBrushScalars prepared;
  auto prepare = [&]() {
    return prepareBrushScalars(brush, {}, brush.deviceInputCtx, prepared).error;
  };
  test_assert(prepare() == PropError::ERROR_NONE);
  auto value = [&](const char *name) {
    for (const auto &item : prepared.values())
      if (item.name == string(name))
        return item.value;
    return -999.0;
  };
  test_assert(value("cavity_factor") == 1.25 && value("cavity_blur_steps") == 3 &&
              value("cavity_inverted") == 1);
  test_assert(prepared.declarations().empty());
  props::ScalarDeclaration declaration;
  test_assert(!brush.props.struct_def->scalarDeclaration("automask_cavity", declaration));
  property.dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
  test_assert(prepare() == PropError::ERROR_INVALID_DYNAMICS);
  property.dynamics.devices.clear();
  brush.cavity_blur_steps = -1;
  test_assert(prepare() == PropError::ERROR_INVALID_VALUE);
  brush.cavity_blur_steps = INT32_MAX;
  test_assert(prepare() == PropError::ERROR_INVALID_VALUE);
  brush.cavity_blur_steps = 3;
  brush.cavity_curve[7] = std::numeric_limits<float>::quiet_NaN();
  test_assert(!brush.automask_cavity && prepare() == PropError::ERROR_INVALID_VALUE);
  brush.cavity_curve[7] = .125f;

  BrushProgram program;
  program.addCommand(int(SculptBrushes::DRAW));
  program.addCommand(int(SculptBrushes::DRAW));
  test_assert(program.setCommandScalarChecked(
                  0, "automask_cavity", props::Prop::BOOL, 1) == PropError::ERROR_NONE);
  test_assert(
      program.setCommandScalarChecked(0, "cavity_factor", props::Prop::FLOAT32, 2.5) ==
      PropError::ERROR_NONE);
  test_assert(
      program.setCommandScalarChecked(0, "cavity_blur_steps", props::Prop::INT32, 2) ==
      PropError::ERROR_NONE);
  Vector<float> lut;
  lut.resize(256);
  for (int i = 0; i < 256; i++)
    lut[i] = float(i) / 255;
  test_assert(program.replaceCommandCavityCurveChecked(0, lut) ==
              int(PropError::ERROR_NONE));
  lut[7] = .99f;
  test_assert(program.commands[0].cavityCurveOverride[7] == 7.0f / 255);
  lut[8] = std::numeric_limits<float>::infinity();
  test_assert(program.replaceCommandCavityCurveChecked(0, lut) !=
              int(PropError::ERROR_NONE));
  test_assert(program.commands[0].cavityCurveOverride[7] == 7.0f / 255);
  test_assert(program.removeCommandCavityCurveChecked(-1) != int(PropError::ERROR_NONE));
  std::span<const BrushUniformManifestEntry> manifests[2] = {};
  PreparedProgramScalars stages;
  test_assert(
      prepareProgramScalars(brush, program, manifests, brush.deviceInputCtx, stages)
          .error == PropError::ERROR_NONE);
  test_assert(stages.stages[0].cavityCurve()[7] == 7.0f / 255 &&
              stages.stages[1].cavityCurve()[7] == .125f);
  test_assert(program.removeCommandCavityCurveChecked(0) == int(PropError::ERROR_NONE));
  test_assert(
      prepareProgramScalars(brush, program, manifests, brush.deviceInputCtx, stages)
          .error == PropError::ERROR_NONE);
  test_assert(stages.stages[0].cavityCurve()[7] == .125f);
  program.commands[1].cavityCurveOverride.append(1);
  test_assert(
      prepareProgramScalars(brush, program, manifests, brush.deviceInputCtx, stages)
          .error == PropError::ERROR_INVALID_VALUE);
  test_assert(stages.stages[0].cavityCurve()[7] == .125f);
  fprintf(stderr, "prepared cavity authoritative settings and LUT candidates passed\n");
}

template <class Executor, class Source, class Co>
static void
cavityExecutionCases(Executor &ex, Brush &brush, Source source, Co co, int count)
{
  const float3 center(0, 0, 1), normal(0, 0, 1);
  BrushProgram program;
  program.addCommand(int(SculptBrushes::DRAW));
  program.addCommand(int(SculptBrushes::DRAW));
  // A disabled large first command must not claim first contact for the small cavity
  // command.
  program.setCommandFloat(0, int(BrushProp::Radius), 3);
  program.setCommandFloat(0, int(BrushProp::Strength), 0);
  program.setCommandFloat(1, int(BrushProp::Radius), .9f);
  program.setCommandFloat(1, int(BrushProp::Strength), 0);
  program.setCommandScalarChecked(1, "automask_cavity", props::Prop::BOOL, 1);
  program.setCommandScalarChecked(1, "cavity_factor", props::Prop::FLOAT32, .7);
  auto run = [&](float3 position = float3(0, 0, 1)) {
    test_assert(ex.applyResolvedProgram(&program, position, normal).error ==
                PropError::ERROR_NONE);
    test_assert(!brush.automask_cavity && brush.cavity_factor == 1.0f &&
                !ex.preparedCavityActive);
  };
  run(float3(100));
  test_assert(ex.preparedCavity.configurations() == 0);
  run();
  test_assert(ex.preparedCavity.configurations() == 1);
  size_t contacts = 0;
  CavityParams params;
  params.enabled = true;
  params.factor = .7f;
  params.blur_steps = brush.cavity_blur_steps;
  params.inverted = brush.cavity_inverted;
  params.use_curve = brush.cavity_use_curve;
  params.curve_lut = brush.cavity_curve.data();
  CavityScratch scratch;
  std::vector<float> first(count, -1);
  for (int v = 0; v < count; v++) {
    if ((co(v) - center).length() / .9f < 1) {
      contacts++;
      first[v] = cavityRemap(params, cavityRawT(source, v, params.blur_steps, scratch));
      test_assert((*ex.preparedCavity.active())[v] == first[v]);
    }
  }
  test_assert(contacts > 0 && ex.preparedCavity.evaluations() == contacts &&
              ex.preparedCavity.storedFactors() == contacts);
  run();
  test_assert(ex.preparedCavity.evaluations() == contacts);
  // Alter only neighboring geometry, then revisit A through B: A remains first-contact
  // frozen.
  for (int v = 0; v < count; v++)
    co(v)[2] += .015f * float((v % 5) - 2);
  std::vector<float> expectedB(count, -1);
  size_t newA = 0, contactsB = 0;
  for (int v = 0; v < count; v++) {
    if ((co(v) - center).length() / .9f >= 1)
      continue;
    const float raw = cavityRawT(source, v, params.blur_steps, scratch);
    if (first[v] < 0) {
      first[v] = cavityRemap(params, raw);
      newA++;
    }
    params.factor = .8f;
    expectedB[v] = cavityRemap(params, raw);
    params.factor = .7f;
    contactsB++;
  }
  program.setCommandScalarChecked(1, "cavity_factor", props::Prop::FLOAT32, .8);
  run();
  test_assert(ex.preparedCavity.configurations() == 2);
  const auto afterB = ex.preparedCavity.evaluations();
  test_assert(afterB == contacts + contactsB);
  for (int v = 0; v < count; v++)
    if (expectedB[v] >= 0)
      test_assert((*ex.preparedCavity.active())[v] == expectedB[v]);
  program.setCommandScalarChecked(1, "cavity_factor", props::Prop::FLOAT32, .7);
  run();
  for (int v = 0; v < count; v++)
    if (first[v] >= 0 && (co(v) - center).length() / .9f < 1)
      test_assert((*ex.preparedCavity.active())[v] == first[v]);
  const auto afterA = ex.preparedCavity.evaluations();
  test_assert(afterA == afterB + newA);
  run();
  test_assert(ex.preparedCavity.evaluations() == afterA);
  // Every configuration field participates in identity, including a disabled curve's
  // samples.
  for (const auto &override :
       std::initializer_list<BrushScalarOverride>{
           {"cavity_blur_steps", props::Prop::INT32, 3},
           {"cavity_inverted", props::Prop::BOOL, 1},
           {"cavity_use_curve", props::Prop::BOOL, 1}})
  {
    const auto configs = ex.preparedCavity.configurations();
    program.setCommandScalarChecked(1, override.name, override.type, override.value);
    run();
    test_assert(ex.preparedCavity.configurations() == configs + 1);
  }
  const auto oldCurve = brush.cavity_curve;
  brush.cavity_curve[4] += .01f;
  const auto configs = ex.preparedCavity.configurations();
  run();
  test_assert(ex.preparedCavity.configurations() == configs + 1 &&
              brush.cavity_curve[4] == oldCurve[4] + .01f);
  // A malformed later command fails before any cache or topology work.
  const auto evaluations = ex.preparedCavity.evaluations();
  program.commands[1].cavityCurveOverride.append(0);
  test_assert(ex.applyResolvedProgram(&program, center, normal).error ==
              PropError::ERROR_INVALID_VALUE);
  test_assert(ex.preparedCavity.evaluations() == evaluations && !ex.preparedCavityActive);
}

static void cavityExecutors()
{
  for (auto mode :
       {CommandExecutor::NeighborMode::LiveDisk, CommandExecutor::NeighborMode::Csr})
  {
    auto *mesh = mesh::createCube(12, 2.0f);
    {
      spatial::SpatialTree tree(mesh);
      tree.leaf_limit = 32;
      tree.buildAll();
      Brush brush;
      brush.cavity_factor = 1;
      CommandExecutor ex(&tree, &brush);
      ex.neighborMode = mode;
      ex.beginStep(false);
      cavityExecutionCases(
          ex,
          brush,
          MeshCavitySrc{mesh},
          [&](int v) -> float3 & { return mesh->v.co[v]; },
          mesh->v.count);
      test_assert(mesh->topo_cache.valid(*mesh));
      ex.endStep();
      test_assert(
          ex.applyResolvedDab(SculptBrushes::DRAW, float3(0.0f), float3(0, 0, 1)).error ==
          PropError::ERROR_INVALID_OWNER);
      ex.beginStep(false);
      test_assert(ex.preparedCavity.evaluations() == 0);
      brush.automask_cavity = true;
      brush.radius = 1;
      brush.writeProps();
      mesh->topo_stamp++;
      test_assert(mesh->topo_frozen && !mesh->topo_cache.valid(*mesh));
      test_assert(
          ex.applyResolvedDab(SculptBrushes::DRAW, float3(0, 0, 1), float3(0, 0, 1))
              .error == PropError::ERROR_NONE);
      test_assert(brush.automask_cavity && ex.preparedCavity.evaluations() > 0);
      CavityScratch wrapped, fresh;
      wrapped.ensure(mesh->v.count);
      wrapped.token = UINT32_MAX;
      const auto reference = cavityRawT(MeshCavitySrc{mesh}, 0, 2, fresh);
      test_assert(cavityRawT(MeshCavitySrc{mesh}, 0, 2, wrapped) == reference &&
                  wrapped.token == 1);
      ex.endStep();
    }
    alloc::Delete(mesh);
  }
  for (bool deferred : {false, true}) {
    auto *cage = mesh::createCube(4, 2.0f);
    {
      subdiv::Multires mr;
      mr.init(*cage, 2);
      auto *domain = mr.gridDomain(2);
      domain->ensureTree(50);
      Brush brush;
      brush.cavity_factor = 1;
      GridBrushExecutor ex(domain, &brush);
      ex.deferNormals = deferred;
      ex.beginStep();
      cavityExecutionCases(
          ex,
          brush,
          GridBrushExecutor::GridCavitySrc{domain},
          [&](int v) -> float3 & { return domain->pos()[v]; },
          domain->vertCount());
      ex.endStep();
      ex.beginStep();
      test_assert(ex.preparedCavity.evaluations() == 0);
      ex.endStep();
      ex.attach(domain);
      test_assert(ex.preparedCavity.configurations() == 0);
    }
    alloc::Delete(cage);
  }
  fprintf(stderr, "prepared cavity mesh/grid first-contact cache and lifecycle passed\n");
}

static std::vector<float3> cavityEffects(
    bool grid, bool prepared, bool nonAccum, bool deferred, SculptBrushes secondary)
{
  std::vector<float3> output;
  auto *mesh = mesh::createCube(grid ? 4 : 12, 2.0f);
  {
    subdiv::Multires mr;
    mr.init(*mesh, 2);
    auto *domain = mr.gridDomain(2);
    domain->ensureTree(50);
    spatial::SpatialTree tree(mesh);
    tree.leaf_limit = 32;
    tree.buildAll();
    meshlog::MeshLog meshLog;
    if (!prepared)
      meshLog.setActiveMesh(mesh);
    subdiv::GridStrokeLog gridLog;
    Brush brush;
    brush.radius = 2;
    brush.strength = .2f;
    brush.automask_cavity = prepared;
    brush.cavity_use_curve = true;
    brush.cavity_curve.fill(.75f);
    brush.automask_view_normal = prepared;
    brush.view_normal_limit = .4f;
    brush.view_normal_falloff = .2f;
    brush.writeProps();
    const auto originalCurve = brush.cavity_curve;
    BrushProgram program;
    const SculptBrushes tools[] = {SculptBrushes::DRAW, secondary, SculptBrushes::DRAW};
    const float masks[] = {.25f, .75f, .5f};
    for (int i = 0; i < 3; i++) {
      program.addCommand(int(tools[i]));
      program.setCommandFloat(i, int(BrushProp::Radius), i == 0 ? .7f : 1.5f);
      program.setCommandFloat(
          i, int(BrushProp::Strength), .2f * (prepared ? 1 : masks[i]));
      if (prepared) {
        for (const auto &setting : std::initializer_list<BrushScalarOverride>{
                 {"automask_cavity", props::Prop::BOOL, 1},
                 {"cavity_factor", props::Prop::FLOAT32, 2},
                 {"cavity_blur_steps", props::Prop::INT32, 3},
                 {"cavity_inverted", props::Prop::BOOL, 0},
                 {"cavity_use_curve", props::Prop::BOOL, 1},
                 {"automask_view_normal", props::Prop::BOOL, 0},
                 {"cull_backfaces", props::Prop::BOOL, 1},
                 {"view_normal_limit", props::Prop::FLOAT32, 1},
                 {"view_normal_falloff", props::Prop::FLOAT32, .1}})
          program.setCommandScalarChecked(i, setting.name, setting.type, setting.value);
      }
      if (prepared && i != 1) {
        Vector<float> lut;
        lut.resize(256);
        for (auto &sample : lut)
          sample = masks[i];
        test_assert(program.replaceCommandCavityCurveChecked(i, lut) == 0);
      }
    }
    CommandExecutor meshEx(&tree, &brush);
    meshEx.meshLog = &meshLog;
    meshEx.nonAccum = nonAccum;
    meshEx.setStrokeGen(1);
    GridBrushExecutor gridEx(domain, &brush, &gridLog);
    gridEx.nonAccum = nonAccum;
    gridEx.deferNormals = deferred;
    auto positions = [&]() {
      std::vector<float3> values;
      int count = grid ? domain->vertCount() : mesh->v.count;
      for (int v = 0; v < count; v++)
        values.push_back(grid ? domain->pos()[v] : mesh->v.co[v]);
      return values;
    };
    auto before = positions();
    if (grid)
      gridEx.beginStep();
    else
      meshEx.beginStep(false);
    for (int dab = 0; dab < 3; dab++) {
      float3 center(.1f * dab, 0, 1), normal(0, 0, 1);
      if (grid) {
        if (prepared)
          test_assert(gridEx.applyResolvedProgram(&program, center, normal).error ==
                      PropError::ERROR_NONE);
        else
          gridEx.applyProgram(&program, center, normal);
      } else {
        if (prepared)
          test_assert(meshEx.applyResolvedProgram(&program, center, normal).error ==
                      PropError::ERROR_NONE);
        else {
          meshEx.applyDab(&program, center, normal, 1.5f, nullptr, 0);
          tree.updateQueries();
        }
      }
      test_assert(brush.cavity_curve == originalCurve &&
                  brush.automask_cavity == prepared && brush.cavity_use_curve &&
                  brush.cavity_factor == 1 && !brush.cavity_inverted);
      test_assert(brush.cavity_blur_steps == 2 &&
                  brush.automask_view_normal == prepared && !brush.cull_backfaces &&
                  brush.view_normal_limit == .4f && brush.view_normal_falloff == .2f);
    }
    output = positions();
    int moved = 0;
    for (size_t i = 0; i < output.size(); i++)
      moved += (output[i] - before[i]).length() > 1e-6f;
    test_assert(moved > 0);
    if (grid) {
      gridEx.endStep();
      gridLog.undo();
    } else {
      meshEx.endStep();
      meshLog.undo(mesh, &tree);
    }
    auto undone = positions();
    for (size_t i = 0; i < before.size(); i++)
      test_assert((undone[i] - before[i]).lengthSqr() == 0);
  }
  alloc::Delete(mesh);
  return output;
}

static void cavityEffectsMatrix()
{
  for (bool grid : {false, true})
    for (bool nonAccum : {false, true})
      for (bool deferred : {false, true}) {
        if (!grid && deferred)
          continue;
        for (auto secondary : {SculptBrushes::MASK, SculptBrushes::SMOOTH}) {
          auto expected = cavityEffects(grid, false, nonAccum, deferred, secondary);
          auto actual = cavityEffects(grid, true, nonAccum, deferred, secondary);
          test_assert(expected.size() == actual.size());
          for (size_t i = 0; i < actual.size(); i++)
            test_assert((actual[i] - expected[i]).length() < 2e-6f);
        }
      }
  fprintf(stderr, "prepared cavity independent strength oracle and exact undo passed\n");
}
