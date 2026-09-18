#pragma once

static std::vector<float> previewGeometry(SculptBrushes tool,
                                          bool nonAccum,
                                          CommandExecutor::NeighborMode mode,
                                          bool programMode,
                                          bool replacements)
{
  auto *m = mesh::createCube(6, 2.0f);
  for (int v : m->v)
    m->v.co[v][2] += .06f * std::sin(2.3f * m->v.co[v][0] + .7f * m->v.co[v][1]);
  std::vector<float> result;
  {
    spatial::SpatialTree tree(m);
    tree.leaf_limit = 16;
    tree.buildAll();
    test_assert(tree.leaves().size() > 4);
    meshlog::MeshLog log;
    log.setActiveMesh(m);
    Brush brush;
    brush.radius = .6f;
    brush.strength = .2f;
    brush.unboundedExtent = 0;
    brush.automask_cavity = true;
    brush.cavity_factor = 0;
    brush.cavity_blur_steps = 1;
    brush.writeProps();
    CommandExecutor ex(&tree, &brush);
    ex.meshLog = &log;
    ex.nonAccum = nonAccum;
    ex.neighborMode = mode;
    ex.beginStep(false);
    ex.setStrokeGen(1);
    BrushProgram program;
    program.addCommand(int(tool));
    program.addCommand(int(SculptBrushes::BSMOOTH));
    program.setCommandScalarChecked(1, "radius", props::Prop::FLOAT32, 2.5f);
    program.setCommandScalarChecked(1, "strength", props::Prop::FLOAT32, .13f);
    auto state = [&]() {
      std::vector<float> values;
      for (int v : m->v) {
        for (int axis = 0; axis < 3; axis++)
          values.push_back(m->v.co[v][axis]);
        values.push_back(tree.treeMesh.v.mask[v]);
      }
      return values;
    };
    auto original = state();
    const float3 normal(0, 0, 1);
    const float radii[] = {2.1f, .6f, 1.3f};
    for (int tick = replacements ? 0 : 2; tick < 3; tick++) {
      if (ex.previewActive()) {
        ex.rollbackPreviewDab();
        tree.updateQueries();
        test_assert(state() == original);
        test_assert(brush.strokePathCount == 0 && ex.isFirstOfStep);
        test_assert(ex.preparedCavity.configurations() == 0);
        test_assert(ex.preparedEnhance.configurations() == 0);
      }
      *source(brush, "radius")->internal_value() = radii[tick];
      const float3 center(.4f, 0, 1);
      ex.beginPreviewDab(center, .01f);
      for (int image = 0; image < 2; image++) {
        float3 origin(image ? -.4f : .4f, 0, 1);
        brush.grabFrom = origin;
        brush.grabTo = float3(image ? -.2f : .2f, 0, .3f);
        if (image)
          ex.extendPreviewDab(origin, .01f);
        auto status =
            programMode
                ? ex.applyResolvedProgram(&program, origin, normal, false, image != 0)
                : ex.applyResolvedDab(tool, origin, normal, false, image != 0);
        if (status.error != PropError::ERROR_NONE)
          fprintf(
              stderr, "preview rejected tool %d: %s\n", int(tool), status.name.c_str());
        test_assert(status.error == PropError::ERROR_NONE);
      }
      const auto valid = state();
      const auto stamp = ex.dabGen;
      program.setCommandScalarChecked(1, "radius", props::Prop::INT32, 2);
      test_assert(ex.applyResolvedProgram(&program, center, normal, true).error !=
                  PropError::ERROR_NONE);
      test_assert(ex.previewActive() && state() == valid && ex.dabGen == stamp);
      program.setCommandScalarChecked(1, "radius", props::Prop::FLOAT32, 2.5f);
    }
    result = state();
    if (result == original)
      fprintf(stderr,
              "preview no movement: tool %d nonAccum %d program %d\n",
              int(tool),
              nonAccum,
              programMode);
    test_assert(result != original);
    ex.commitPreviewDab();
    ex.endStep();
    log.undo(m, &tree);
    test_assert(state() == original);
    log.redo(m, &tree);
    test_assert(state() == result);
    // A later preview stroke can cancel without undoing the committed one.
    ex.beginStep(false);
    ex.setStrokeGen(2);
    ex.beginPreviewDab(float3(0.0f), .01f);
    test_assert(ex.applyResolvedDab(SculptBrushes::DRAW, float3(0.0f), normal).error ==
                PropError::ERROR_NONE);
    ex.rollbackPreviewDab();
    tree.updateQueries();
    test_assert(state() == result);
    ex.endStep();
  }
  alloc::Delete(m);
  return result;
}

static void preparedPreviewCases()
{
  for (auto tool : {SculptBrushes::DRAW,
                    SculptBrushes::SMOOTH,
                    SculptBrushes::BSMOOTH,
                    SculptBrushes::MASK,
                    SculptBrushes::GRAB,
                    SculptBrushes::KELVINLET,
                    SculptBrushes::ENHANCE})
    for (bool nonAccum : {false, true})
      for (bool program : {false, true})
        for (auto mode : {CommandExecutor::NeighborMode::LiveDisk,
                          CommandExecutor::NeighborMode::Csr})
        {
          auto replaced = previewGeometry(tool, nonAccum, mode, program, true);
          auto reference = previewGeometry(tool, nonAccum, mode, program, false);
          test_assert(replaced.size() == reference.size());
          for (size_t i = 0; i < reference.size(); i++)
            test_assert(std::abs(replaced[i] - reference[i]) < 3e-6f);
        }
  fprintf(stderr,
          "prepared previews match fresh final groups; regions, caches, rejection, "
          "cancel and undo passed\n");
}
