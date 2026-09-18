#pragma once

template <class Source> struct DeformedCavityReference {
  Source source;
  const std::vector<float3> &positions;
  int vertCap() const
  {
    return source.vertCap();
  }
  float3 co(int v) const
  {
    return positions[v];
  }
  float3 no(int v) const
  {
    return source.no(v);
  }
  auto neighbors(int v) const
  {
    return source.neighbors(v);
  }
};

template <class Executor, class Source, class Co>
static void
unboundedCavityTransition(Executor &ex, Brush &brush, Source source, Co co, int count)
{
  const float3 center(0, 0, 1), normal(0, 0, 1);
  brush.radius = 4;
  brush.strength = .25f;
  brush.grabTo = float3(0.0f);
  brush.unboundedExtent = 1;
  brush.cavity_factor = .05f;
  brush.cavity_blur_steps = 1;
  brush.writeProps();
  ex.anchoredGrab = false;
  BrushProgram program;
  program.addCommand(int(SculptBrushes::DRAW));
  program.addCommand(int(SculptBrushes::KELVINLET));
  program.setCommandScalarChecked(0, "automask_cavity", props::Prop::BOOL, 0);
  program.setCommandScalarChecked(1, "automask_cavity", props::Prop::BOOL, 1);
  std::vector<float3> positions;
  int crossed = 0;
  size_t contacts = 0;
  float cutoff = 0;
  auto supported = [&](const float3 &p) {
    return std::hypot(double(p[0]) - center[0],
                      double(p[1]) - center[1],
                      double(p[2]) - center[2]) < double(cutoff);
  };
  for (int v = 0; v < count; v++) {
    const auto p = co(v);
    const float t = 1 - std::min((p - center).length() / 4, 1.0f);
    const float strength = t * t * (3 - 2 * t) * .25f;
    positions.push_back(p + normal * strength * 4 * .5f);
  }
  // Select a cutoff between an actual before/after pair, so both domains
  // exercise a crossing despite their different tessellation and curvature.
  for (int candidate = 0; candidate < count; candidate++) {
    cutoff =
        ((co(candidate) - center).length() + (positions[candidate] - center).length()) *
        .5f;
    crossed = 0;
    contacts = 0;
    for (int v = 0; v < count; v++) {
      crossed += supported(co(v)) != supported(positions[v]);
      contacts += supported(positions[v]);
    }
    if (crossed > 0 && contacts > 0)
      break;
  }
  test_assert(crossed > 0 && contacts > 0);
  program.setCommandScalarChecked(1, "radius", props::Prop::FLOAT32, cutoff);
  CavityParams params;
  params.enabled = true;
  params.factor = .05f;
  params.blur_steps = 1;
  params.curve_lut = brush.cavity_curve.data();
  CavityScratch scratch;
  DeformedCavityReference<Source> reference{source, positions};
  std::vector<float> factors(count, -1);
  for (int v = 0; v < count; v++)
    if (supported(positions[v]))
      factors[v] = cavityRemap(params, cavityRawT(reference, v, 1, scratch));
  test_assert(ex.applyResolvedProgram(&program, center, normal).error ==
              PropError::ERROR_NONE);
  test_assert(ex.preparedCavity.evaluations() == contacts);
  for (int v = 0; v < count; v++) {
    test_assert((co(v) - positions[v]).length() < 1e-6f);
    if (factors[v] >= 0)
      test_assert(std::abs((*ex.preparedCavity.active())[v] - factors[v]) < 1e-6f);
  }
}

static void unboundedCavityTransitionMatrix()
{
  for (bool grid : {false, true}) {
    auto *mesh = mesh::createCube(4, 2.0f);
    mesh->topo_cache.ensureRing1(*mesh);
    {
      subdiv::Multires mr;
      mr.init(*mesh, 2);
      auto *domain = mr.gridDomain(2);
      domain->ensureTree(40);
      spatial::SpatialTree tree(mesh);
      tree.leaf_limit = 24;
      tree.buildAll();
      Brush brush;
      if (grid) {
        GridBrushExecutor ex(domain, &brush);
        ex.beginStep();
        unboundedCavityTransition(
            ex,
            brush,
            GridBrushExecutor::GridCavitySrc{domain},
            [&](int v) -> float3 & { return domain->pos()[v]; },
            domain->vertCount());
        ex.endStep();
      } else {
        CommandExecutor ex(&tree, &brush);
        ex.beginStep(false);
        unboundedCavityTransition(
            ex,
            brush,
            MeshCavitySrc{mesh},
            [&](int v) -> float3 & { return mesh->v.co[v]; },
            mesh->v.count);
        ex.endStep();
      }
    }
    alloc::Delete(mesh);
  }
  fprintf(
      stderr,
      "DRAW crossing unbounded cavity support uses post-stage first-contact geometry\n");
}
