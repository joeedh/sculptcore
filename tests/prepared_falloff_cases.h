#pragma once

static std::vector<float3> preparedFalloffRun(bool grid,
                                              bool resolved,
                                              bool programMode,
                                              FalloffShape shape,
                                              FalloffKind kind,
                                              bool layered = false)
{
  auto *cage = mesh::createCube(grid ? 4 : 12, 2.0f);
  std::vector<float3> output;
  {
    subdiv::Multires mr;
    mr.init(*cage, 2);
    if (layered) {
      const int layer = mr.layerAdd();
      test_assert(mr.setEditTarget(layer) == layer);
    }
    auto *domain = mr.gridDomain(2);
    auto *gridTree = domain->ensureTree(40);
    spatial::SpatialTree tree(cage);
    tree.leaf_limit = 24;
    tree.buildAll();
    test_assert(grid ? gridTree->leaves.size() > 4 : tree.leaves().size() > 4);
    meshlog::MeshLog meshLog;
    meshLog.setActiveMesh(cage);
    subdiv::GridStrokeLog gridLog;
    Brush brush;
    brush.radius = .5f;
    brush.strength = .2f;
    brush.falloff_shape = shape;
    brush.falloff_kind = kind;
    brush.falloff_dir = float3(0, 0, 1);
    brush.falloff_extent = float3(1.2f, .8f, 1);
    for (int i = 0; i < kFalloffCurveSize; i++)
      brush.falloff_curve[i] = .1f + .9f * float(i) / float(kFalloffCurveSize - 1);
    brush.writeProps();
    if (resolved)
      source(brush, "radius")
          ->dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::ADD, 1);
    CommandExecutor meshEx(&tree, &brush);
    meshEx.meshLog = &meshLog;
    GridBrushExecutor gridEx(domain, &brush, &gridLog);
    BrushProgram program;
    const auto mainTool = layered ? SculptBrushes::LAYERDRAW : SculptBrushes::DRAW;
    program.addCommand(int(mainTool));
    if (programMode)
      program.addCommand(int(SculptBrushes::DRAW));
    std::vector<float3> before;
    const int count = grid ? domain->vertCount() : cage->v.count;
    for (int v = 0; v < count; v++)
      before.push_back(grid ? domain->pos()[v] : cage->v.co[v]);
    const auto blobBefore = storeBlob(mr);
    if (grid)
      gridEx.beginStep();
    else
      meshEx.beginStep(false);
    const float3 center(0, 0, 1), normal(0, 0, 1);
    for (int dab = 0; dab < 2; dab++) {
      const float pressure = .25f + .5f * dab;
      brush.pushDeviceInput(int(props::DeviceType::PRESSURE), pressure);
      for (int stage = 0; stage < (programMode ? 2 : 1); stage++) {
        program.commands[stage].floatOverrides.clear();
        program.setCommandFloat(
            stage, int(BrushProp::Radius), .5f + stage + (resolved ? 0 : pressure));
        program.setCommandFloat(stage, int(BrushProp::Strength), .2f);
      }
      if (resolved) {
        auto result =
            grid ? (programMode ? gridEx.applyResolvedProgram(&program, center, normal)
                                : gridEx.applyResolvedDab(mainTool, center, normal))
                 : (programMode ? meshEx.applyResolvedProgram(&program, center, normal)
                                : meshEx.applyResolvedDab(mainTool, center, normal));
        test_assert(result.error == PropError::ERROR_NONE);
      } else if (grid) {
        brush.radius = 100;
        test_assert(gridEx.applyProgram(&program, center, normal) >= 0);
      } else {
        test_assert(meshEx.applyDab(&program, center, normal, 100, nullptr, 0) >= 0);
        tree.updateQueries();
      }
    }
    if (grid)
      gridEx.endStep();
    else
      meshEx.endStep();
    const auto blobAfter = storeBlob(mr);
    int changed = 0;
    for (int v = 0; v < count; v++) {
      output.push_back(grid ? domain->pos()[v] : cage->v.co[v]);
      test_assert(std::isfinite(output.back()[2]));
      changed += (output.back() - before[v]).lengthSqr() > 0;
    }
    test_assert(changed > 0);
    if (grid)
      test_assert(gridLog.undo());
    else
      meshLog.undo(cage, &tree);
    if (grid)
      test_assert(storeBlob(mr) == blobBefore);
    for (int v = 0; v < count; v++) {
      const auto co = grid ? domain->pos()[v] : cage->v.co[v];
      test_assert((co - before[v]).lengthSqr() == 0);
    }
    if (grid)
      test_assert(gridLog.redo());
    else
      meshLog.redo(cage, &tree);
    if (grid)
      test_assert(storeBlob(mr) == blobAfter);
    for (int v = 0; v < count; v++) {
      const auto co = grid ? domain->pos()[v] : cage->v.co[v];
      test_assert((co - output[v]).lengthSqr() == 0);
    }
  }
  alloc::Delete(cage);
  return output;
}

static void preparedFalloffGate()
{
  for (bool grid : {false, true})
    for (bool program : {false, true})
      for (auto shape : {FalloffShape::Spherical,
                         FalloffShape::Cube,
                         FalloffShape::Box,
                         FalloffShape::Linear})
        for (auto kind :
             {FalloffKind::Smoothstep, FalloffKind::Gaussian, FalloffKind::Curve})
        {
          auto checked = preparedFalloffRun(grid, true, program, shape, kind);
          auto raw = preparedFalloffRun(grid, false, program, shape, kind);
          test_assert(checked.size() == raw.size());
          float error = 0;
          for (size_t i = 0; i < raw.size(); i++)
            error = std::max(error, (checked[i] - raw[i]).length());
          if (error >= 1e-6f)
            fprintf(stderr, "falloff mismatch grid=%d program=%d shape=%d kind=%d error=%g\n",
                    grid, program, int(shape), int(kind), error);
          test_assert(error < 1e-6f);
        }
  for (bool program : {false, true}) {
    auto checked = preparedFalloffRun(
        true, true, program, FalloffShape::Spherical, FalloffKind::Smoothstep, true);
    auto raw = preparedFalloffRun(
        true, false, program, FalloffShape::Spherical, FalloffKind::Smoothstep, true);
    test_assert(checked.size() == raw.size());
    for (size_t i = 0; i < raw.size(); i++)
      test_assert((checked[i] - raw[i]).length() < 1e-6f);
  }
  fprintf(stderr,
          "prepared falloff mesh/grid standalone/program all-region parity and undo "
          "passed\n");
}
