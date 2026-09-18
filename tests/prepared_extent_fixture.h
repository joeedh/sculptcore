#pragma once

static std::vector<float3>
extentStageExecution(int tool, bool grid, bool composed, int skip)
{
  std::vector<float3> output;
  auto *mesh = mesh::createCube(4, 2.0f);
  {
    subdiv::Multires mr;
    mr.init(*mesh, 2);
    auto *domain = mr.gridDomain(2);
    domain->ensureTree(40);
    spatial::SpatialTree tree(mesh);
    tree.leaf_limit = 24;
    tree.buildAll();
    Brush brush;
    brush.radius = skip == 1 ? 0 : 1;
    brush.strength = .3f;
    brush.grabFrom = float3(.1f, 0, 1);
    brush.grabTo = float3(.02f, .01f, .05f);
    brush.writeProps();
    CommandExecutor meshEx(&tree, &brush);
    GridBrushExecutor gridEx(domain, &brush);
    meshEx.anchoredGrab = gridEx.anchoredGrab = false;
    test_assert(meshEx.queryUniformManifest(tool) == 1);
    *property<props::Float32Prop>(brush, "unboundedExtent")->internal_value() = .25f;
    brush.unboundedExtent = 7;
    if (grid)
      gridEx.beginStep();
    else
      meshEx.beginStep(false);
    const float3 center = skip == 2 ? float3(100.0f) : float3(0, 0, 1);
    const float3 normal(0, 0, 1);
    BrushProgram program;
    program.addCommand(tool);
    program.addCommand(int(SculptBrushes::KELVINLET));
    program.setCommandScalarChecked(0, "unboundedExtent", Prop::FLOAT32, .25);
    auto single = [&](SculptBrushes command) {
      return grid ? gridEx.applyResolvedDab(command, center, normal)
                  : meshEx.applyResolvedDab(command, center, normal);
    };
    // A late invalid extent must not let the first, declaring command execute.
    const int count = grid ? domain->vertCount() : mesh->v.count;
    auto co = [&](int v) -> float3 & { return grid ? domain->pos()[v] : mesh->v.co[v]; };
    std::vector<float3> before;
    for (int v = 0; v < count; v++)
      before.push_back(co(v));
    if (skip != 1) {
      brush.unboundedExtent = std::numeric_limits<float>::infinity();
      const auto rejected = grid ? gridEx.applyResolvedProgram(&program, center, normal)
                                 : meshEx.applyResolvedProgram(&program, center, normal);
      test_assert(rejected.error == PropError::ERROR_INVALID_VALUE);
      for (int v = 0; v < count; v++)
        test_assert((co(v) - before[v]).length() == 0);
      test_assert(std::isinf(brush.unboundedExtent));
      brush.unboundedExtent = 7;
    }
    if (composed) {
      const auto result = grid ? gridEx.applyResolvedProgram(&program, center, normal)
                               : meshEx.applyResolvedProgram(&program, center, normal);
      test_assert(result.error == PropError::ERROR_NONE);
    } else {
      test_assert(single(SculptBrushes(tool)).error == PropError::ERROR_NONE);
      test_assert(brush.unboundedExtent == 7);
      test_assert(single(SculptBrushes::KELVINLET).error == PropError::ERROR_NONE);
    }
    test_assert(brush.unboundedExtent == 7);
    test_assert(
        *property<props::Float32Prop>(brush, "unboundedExtent")->internal_value() ==
        .25f);
    // A following bounded command must not inherit the unbounded query or extent.
    test_assert(single(SculptBrushes::DRAW).error == PropError::ERROR_NONE);
    for (int v = 0; v < count; v++) {
      output.push_back(co(v));
      if (skip)
        test_assert((co(v) - before[v]).length() == 0);
    }
    if (grid)
      gridEx.endStep();
    else
      meshEx.endStep();
  }
  alloc::Delete(mesh);
  return output;
}

static void extentStageMatrix(int tool)
{
  for (bool grid : {false, true})
    for (int skip : {0, 1, 2}) {
      const auto composed = extentStageExecution(tool, grid, true, skip);
      const auto reference = extentStageExecution(tool, grid, false, skip);
      test_assert(composed.size() == reference.size());
      for (size_t v = 0; v < composed.size(); v++)
        test_assert((composed[v] - reference[v]).length() < 1e-6f);
    }
  fprintf(stderr,
          "declared then inherited unbounded extent execution and restoration passed\n");
}
