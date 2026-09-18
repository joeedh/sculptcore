#pragma once

template <class Executor, class Read>
static void
grabOracleStroke(Executor &ex, Brush &brush, int count, Read read, bool program)
{
  std::vector<float3> base(count), expected(count);
  for (int v = 0; v < count; v++)
    expected[v] = base[v] = read(v);
  BrushProgram commands;
  commands.addCommand(int(SculptBrushes::GRAB));
  commands.addCommand(int(SculptBrushes::GRAB));
  test_assert(ex.supportsResolved(SculptBrushes::GRAB));
  test_assert(ex.supportsResolvedProgram(&commands));
  const auto initialStamp = ex.dabGen;
  test_assert(
      ex.applyResolvedDab(SculptBrushes::GRAB, float3(0.0f), float3(0, 0, 1), true)
          .error == PropError::ERROR_NONE);
  test_assert(ex.dabGen == initialStamp);
  test_assert(
      ex.applyResolvedDab(SculptBrushes::GRAB, float3(1000.0f), float3(0, 0, 1)).error ==
      PropError::ERROR_NONE);
  for (int v = 0; v < count; v++)
    test_assert((read(v) - expected[v]).length() == 0);
  source(brush, "radius")
      ->dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::ADD, 1);
  auto verify = [&]() {
    for (int v = 0; v < count; v++) {
      if ((read(v) - expected[v]).length() >= 3e-5f)
        fprintf(stderr,
                "grab oracle vertex %d error %g\n",
                v,
                (read(v) - expected[v]).length());
      test_assert((read(v) - expected[v]).length() < 3e-5f);
    }
  };
  int newlyReached = 0;
  std::vector<bool> previous(count, false);
  for (int dab = 0; dab < 6; dab++) {
    const float radii[] = {0, .65f, 2.7f, .9f, 3.5f, 2.5f};
    const float radius = radii[dab];
    const float pressure = radius > 0 ? .2f : 0;
    *source(brush, "radius")->internal_value() = radius - pressure;
    const float mainStrength = .15f + .05f * dab;
    *source(brush, "strength")->internal_value() = mainStrength;
    brush.pushDeviceInput(int(props::DeviceType::PRESSURE), pressure);
    commands.setCommandScalarChecked(1, "radius", props::Prop::FLOAT32, radius * 1.3f);
    commands.setCommandScalarChecked(1, "strength", props::Prop::FLOAT32, .17f);
    // Override the inherited radius stack for the second command.
    commands.commands[1].dynamicsOverrides.clear();
    BrushDynamicsOverride independent;
    independent.name = "radius";
    independent.type = props::Prop::FLOAT32;
    commands.commands[1].dynamicsOverrides.append(independent);
    std::vector<bool> written(count, false);
    for (int image = 0; image < 2; image++) {
      float3 center(image ? -.3f : .3f, 0, 1);
      const float3 delta(image ? -3.0f : 3.0f, .15f * dab, 1.3f * dab);
      brush.grabFrom = center;
      brush.grabTo = delta;
      for (int stage = 0; stage < (program ? 2 : 1); stage++) {
        const float r = radius * (stage ? 1.3f : 1.0f);
        const float strength = stage ? .17f : mainStrength;
        for (int v = 0; v < count; v++) {
          if (r == 0)
            continue;
          float t = std::clamp(1.0f - (base[v] - center).length() / r, 0.0f, 1.0f);
          const float fall = t * t * (3 - 2 * t) * strength;
          if (fall == 0)
            continue;
          if (dab == 2 && !previous[v])
            newlyReached++;
          if (!written[v])
            expected[v] = base[v];
          expected[v] += delta * fall;
          written[v] = previous[v] = true;
        }
      }
      auto result =
          program ? ex.applyResolvedProgram(
                        &commands, center, float3(0, 0, 1), false, image != 0)
                  : ex.applyResolvedDab(
                        SculptBrushes::GRAB, center, float3(0, 0, 1), false, image != 0);
      if (result.error != PropError::ERROR_NONE)
        fprintf(
            stderr, "grab rejection: %s (%d)\n", result.name.c_str(), int(result.error));
      test_assert(result.error == PropError::ERROR_NONE);
      verify();
    }
  }
  test_assert(newlyReached > 0);
  const auto stamp = ex.dabGen;
  const auto nativeRadius = brush.radius;
  brush.grabTo[0] = std::numeric_limits<float>::infinity();
  test_assert(
      ex.applyResolvedDab(SculptBrushes::GRAB, float3(0.0f), float3(0, 0, 1), true)
          .error == PropError::ERROR_INVALID_VALUE);
  test_assert(ex.dabGen == stamp && brush.radius == nativeRadius);
  verify();
  brush.grabTo[0] = 0;
  commands.commands[1].scalarOverrides.clear();
  commands.setCommandScalarChecked(1, "radius", props::Prop::INT32, 3);
  test_assert(ex.applyResolvedProgram(&commands, float3(0.0f), float3(0, 0, 1)).error !=
              PropError::ERROR_NONE);
  test_assert(ex.dabGen == stamp && brush.radius == nativeRadius);
  verify();
}

static void preparedGrabCases()
{
  for (bool program : {false, true})
    for (bool nonAccum : {false, true}) {
      for (auto mode :
           {CommandExecutor::NeighborMode::LiveDisk, CommandExecutor::NeighborMode::Csr})
      {
        auto *m = mesh::createCube(6, 2.0f);
        {
          spatial::SpatialTree tree(m);
          tree.leaf_limit = 16;
          tree.buildAll();
          test_assert(tree.leaves().size() > 4);
          Brush brush;
          brush.radius = 1;
          brush.strength = .3f;
          brush.writeProps();
          meshlog::MeshLog log;
          log.setActiveMesh(m);
          CommandExecutor ex(&tree, &brush);
          ex.meshLog = &log;
          ex.neighborMode = mode;
          ex.nonAccum = nonAccum;
          std::vector<float3> before(m->v.count), after(m->v.count);
          for (int v : m->v)
            before[v] = m->v.co[v];
          ex.beginStep(false);
          ex.setStrokeGen(1);
          grabOracleStroke(
              ex, brush, m->v.count, [&](int v) { return m->v.co[v]; }, program);
          ex.endStep();
          for (int v : m->v)
            after[v] = m->v.co[v];
          log.undo(m, &tree);
          for (int v : m->v)
            test_assert(std::memcmp(&m->v.co[v], &before[v], sizeof(float3)) == 0);
          log.redo(m, &tree);
          for (int v : m->v)
            test_assert(std::memcmp(&m->v.co[v], &after[v], sizeof(float3)) == 0);
          ex.beginStep(false);
          ex.setStrokeGen(2);
          *source(brush, "radius")->internal_value() = 100;
          brush.grabTo = float3(0.0f);
          test_assert(
              ex.applyResolvedDab(SculptBrushes::GRAB, float3(0.0f), float3(0, 0, 1))
                  .error == PropError::ERROR_NONE);
          for (int v : m->v)
            test_assert((m->v.co[v] - after[v]).length() < 1e-6f);
          ex.endStep();
        }
        alloc::Delete(m);
      }
      auto *cage = mesh::createCube(2, 2.0f);
      {
        subdiv::Multires mr;
        mr.init(*cage, 3);
        auto *domain = mr.gridDomain(2);
        domain->ensureTree(16);
        Brush brush;
        brush.radius = 1;
        brush.strength = .3f;
        brush.writeProps();
        subdiv::GridStrokeLog log;
        GridBrushExecutor ex(domain, &brush, &log);
        ex.nonAccum = nonAccum;
        std::vector<float3> before(domain->vertCount()), after(domain->vertCount());
        for (int v = 0; v < domain->vertCount(); v++)
          before[v] = domain->pos()[v];
        ex.beginStep();
        grabOracleStroke(
            ex,
            brush,
            domain->vertCount(),
            [&](int v) { return domain->pos()[v]; },
            program);
        ex.endStep();
        for (int v = 0; v < domain->vertCount(); v++)
          after[v] = domain->pos()[v];
        test_assert(log.undo());
        for (int v = 0; v < domain->vertCount(); v++)
          test_assert(std::memcmp(&domain->pos()[v], &before[v], sizeof(float3)) == 0);
        test_assert(log.redo());
        for (int v = 0; v < domain->vertCount(); v++)
          test_assert(std::memcmp(&domain->pos()[v], &after[v], sizeof(float3)) == 0);
        ex.beginStep();
        *source(brush, "radius")->internal_value() = 100;
        brush.grabTo = float3(0.0f);
        test_assert(
            ex.applyResolvedDab(SculptBrushes::GRAB, float3(0.0f), float3(0, 0, 1))
                .error == PropError::ERROR_NONE);
        for (int v = 0; v < domain->vertCount(); v++)
          test_assert((domain->pos()[v] - after[v]).length() < 1e-6f);
        ex.endStep();
      }
      alloc::Delete(cage);
    }
  fprintf(stderr,
          "prepared anchored grab independent mesh/grid oracle, radius growth, symmetry "
          "and undo passed\n");
}
