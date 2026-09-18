#pragma once

// Included by test_brush_prepared_execution.cc so the numerical oracle and the
// actual executor regressions run in the same required native suite.
struct KelvinletProbe {
  struct Vertex {
    float3 co, no{0, 0, 1};
    float mask = 0;
    int v = 0;
  } point;
  struct Node {
    Vector<int> affected_verts;
    bool updated = false;
    template <class T> void update(T)
    {
      updated = true;
    }
  };
  using vertex_iter = Vertex;
  using vertex_iter_factory = int;
  using face_iter = int;
  using node_type = Node;
  using capture_policy = MeshCapturePolicy;
  static constexpr bool supportsFaceStages = false;
  template <class T> std::span<Vertex> makeVertexIter(Node &)
  {
    return {&point, 1};
  }
  const float3 *liveVertNoPtr(const CommandCtx<KelvinletProbe> &, int)
  {
    return &point.no;
  }
};

static std::array<double, 3> kelvinletReference(
    float3 point, float3 from, float3 force, float radius, float mu, float nu)
{
  // Original formula, with promotion BEFORE subtraction, squaring and products.
  const double a = (1.0 + double(nu)) / (2.0 * double(mu));
  const double b = a / (4.0 * (1.0 - double(nu)));
  std::array<double, 3> r, output;
  double rr = double(radius) * double(radius), dot = 0;
  for (int axis = 0; axis < 3; axis++) {
    r[axis] = double(point[axis]) - double(from[axis]);
    rr += r[axis] * r[axis];
    dot += double(force[axis]) * r[axis];
  }
  const double e = std::sqrt(rr), inverseCube = 1.0 / (e * e * e);
  const double c1 = (a - b) / e + .5 * a * double(radius) * double(radius) * inverseCube;
  const double c2 = b * dot * inverseCube;
  const double normalize = double(radius) / std::max(1.5 * a - b, 1e-6);
  for (int axis = 0; axis < 3; axis++)
    output[axis] = (double(force[axis]) * c1 + r[axis] * c2) * normalize;
  return output;
}

static double cutoffReference(float3 point, float3 center, float radius, float extent)
{
  if (extent <= 0)
    return 1;
  // The cutoff is explicitly a float product in the native execution contract.
  const double cutoff = double(radius * extent);
  double squared = 0;
  for (int axis = 0; axis < 3; axis++) {
    const double d = double(point[axis]) - double(center[axis]);
    squared += d * d;
  }
  const double t = std::clamp((cutoff - std::sqrt(squared)) / (.2 * cutoff), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

static void kelvinletNumerics()
{
  Brush brush;
  brush.unboundedExtent = 0;
  size_t cases = 0;
  for (int exponent : {-127, -126, -100, -80, -40, -10, 0, 10, 40, 80, 120}) {
    brush.radius = std::ldexp(1.0f, exponent);
    for (float mu : {std::nextafter(1e-6f, 1.0f), 1.0f, 100.0f})
      for (float nu : {0.0f, .4f, std::nextafter(.499f, 0.0f)}) {
        brush.mu = mu;
        brush.nu = nu;
        for (float3 offset : {float3(0.0f),
                              float3(brush.radius, 0, 0),
                              float3(brush.radius * .5f),
                              float3(1, 2, -1),
                              float3(std::ldexp(1.0f, 24), 0, 0)})
          for (float3 force : {float3(.1f, -.2f, .3f),
                               float3(0, std::ldexp(1.0f, 24), 0),
                               float3(std::numeric_limits<float>::max() * .5f)})
          {
            KelvinletProbe probe;
            KelvinletProbe::Node node;
            CommandCtxBase base;
            CommandCtx<KelvinletProbe> context(base, node, probe, brush);
            brush.grabFrom = offset * -1.0f;
            brush.grabTo = force;
            auto expected = kelvinletReference(
                float3(0.0f), brush.grabFrom, force, brush.radius, mu, nu);
            command::kelvinlet<KelvinletProbe, AccumLive>(context);
            double forceScale = std::max({std::abs(double(force[0])),
                                          std::abs(double(force[1])),
                                          std::abs(double(force[2]))});
            double error[3];
            for (int axis = 0; axis < 3; axis++) {
              test_assert(std::isfinite(probe.point.co[axis]));
              error[axis] = double(probe.point.co[axis]) - expected[axis];
            }
            test_assert(std::hypot(error[0], error[1], error[2]) <=
                        4e-6 * forceScale +
                            8 * double(std::numeric_limits<float>::denorm_min()));
            test_assert(node.updated && node.affected_verts.size() == 1);
            cases++;
          }
      }
  }
  // The normalized coefficient must not round above one near the origin:
  // multiplying even one excess ulp by FLT_MAX overflows a representable field.
  const float largest = std::numeric_limits<float>::max();
  const float minimum = std::nextafter(std::ldexp(1.0f, -128), 1.0f);
  for (float radius : {minimum, 1.0f})
    for (int axis = 0; axis < 3; axis++)
      for (float distance : {0.0f, std::ldexp(1.0f, -20), std::ldexp(1.0f, -10)}) {
        KelvinletProbe probe;
        KelvinletProbe::Node node;
        CommandCtxBase base;
        CommandCtx<KelvinletProbe> context(base, node, probe, brush);
        brush.radius = radius;
        brush.mu = 1.0f - 6.0f * std::ldexp(1.0f, -24);
        brush.nu = 0;
        brush.grabFrom = float3(0.0f);
        brush.grabFrom[axis] = -distance;
        brush.grabTo = float3(0.0f);
        brush.grabTo[(axis + 1) % 3] = largest;
        const auto expected = kelvinletReference(
            probe.point.co, brush.grabFrom, brush.grabTo, radius, brush.mu, brush.nu);
        command::kelvinlet<KelvinletProbe, AccumLive>(context);
        double error[3];
        for (int i = 0; i < 3; i++) {
          test_assert(std::isfinite(probe.point.co[i]));
          error[i] = double(probe.point.co[i]) - expected[i];
        }
        test_assert(std::hypot(error[0], error[1], error[2]) <= 4e-6 * double(largest));
      }
  // Opposing finite endpoints overflow a float subtraction; transverse output
  // remains measurable even though adding a small axial result rounds away.
  {
    KelvinletProbe probe;
    probe.point.co = float3(largest, 0, 0);
    KelvinletProbe::Node node;
    CommandCtxBase base;
    CommandCtx<KelvinletProbe> context(base, node, probe, brush);
    brush.grabFrom = float3(-largest, 0, 0);
    brush.grabTo = float3(0, largest, 0);
    brush.radius = largest;
    brush.mu = 1;
    brush.nu = .4f;
    const auto expected = kelvinletReference(
        probe.point.co, brush.grabFrom, brush.grabTo, brush.radius, brush.mu, brush.nu);
    command::kelvinlet<KelvinletProbe, AccumLive>(context);
    test_assert(probe.point.co[0] == largest && std::isfinite(probe.point.co[1]));
    test_assert(std::abs(double(probe.point.co[1]) - expected[1]) <=
                4e-6 * double(largest));
  }
  // Near-origin anisotropic addition must not overflow multi-component FLT_MAX
  // forces even when sqrt(1 + tiny squared distance) rounds back to one.
  for (float distance :
       {std::ldexp(1.0f, -15), std::ldexp(1.0f, -14), std::ldexp(1.0f, -13)})
    for (int axis = 0; axis < 3; axis++)
      for (float sign : {-1.0f, 1.0f}) {
        KelvinletProbe probe;
        KelvinletProbe::Node node;
        CommandCtxBase base;
        CommandCtx<KelvinletProbe> context(base, node, probe, brush);
        brush.radius = 1;
        brush.mu = 1;
        brush.nu = .49f;
        brush.grabFrom = float3(0.0f);
        brush.grabFrom[axis] = -4 * distance * sign;
        brush.grabFrom[(axis + 1) % 3] = -3 * distance;
        brush.grabFrom[(axis + 2) % 3] = -2 * distance;
        brush.grabTo = float3(largest);
        brush.grabTo[axis] *= sign;
        const auto expected = kelvinletReference(probe.point.co,
                                                 brush.grabFrom,
                                                 brush.grabTo,
                                                 brush.radius,
                                                 brush.mu,
                                                 brush.nu);
        command::kelvinlet<KelvinletProbe, AccumLive>(context);
        double error[3];
        for (int i = 0; i < 3; i++) {
          test_assert(std::isfinite(probe.point.co[i]));
          error[i] = double(probe.point.co[i]) - expected[i];
        }
        test_assert(std::hypot(error[0], error[1], error[2]) <= 4e-6 * double(largest));
      }
  // Distinct window and field centers expose distance-square under/overflow.
  brush.grabFrom = float3(0.0f);
  brush.grabTo = float3(0, 1, 0);
  brush.mu = 1;
  brush.nu = .4f;
  brush.unboundedExtent = 1;
  for (int exponent : {-80, 80}) {
    brush.radius = std::ldexp(1.0f, exponent);
    for (float distance : {.5f,
                           std::nextafter(.8f, 0.0f),
                           .8f,
                           std::nextafter(.8f, 1.0f),
                           .9f,
                           std::nextafter(1.0f, 0.0f),
                           1.0f,
                           std::nextafter(1.0f, 2.0f),
                           2.0f})
    {
      KelvinletProbe probe;
      KelvinletProbe::Node node;
      CommandCtxBase base;
      base.surfacePos = float3(brush.radius * distance, 0, 0);
      CommandCtx<KelvinletProbe> context(base, node, probe, brush);
      const double expected =
          cutoffReference(float3(0.0f), base.surfacePos, brush.radius, 1);
      test_assert(std::abs(double(context.unboundedWindow(float3(0.0f))) - expected) <
                  1e-7);
      command::kelvinlet<KelvinletProbe, AccumLive>(context);
      test_assert(std::abs(double(probe.point.co[1]) - expected) < 1e-7);
      test_assert(node.updated == (expected != 0));
      test_assert(node.affected_verts.size() == size_t(expected != 0));
    }
  }
  static int textureCalls = 0;
  TextureProgram texture;
  texture.eval = [](const float *, const float *, const float *, const TexEvalCtx *) {
    textureCalls++;
    return 1.0f;
  };
  brush.texture_program = &texture;
  for (int skip = 0; skip < 3; skip++) {
    KelvinletProbe probe;
    KelvinletProbe::Node node;
    CommandCtxBase base;
    base.surfacePos = float3(skip == 2 ? 2.0f : 0.0f, 0, 0);
    CommandCtx<KelvinletProbe> context(base, node, probe, brush);
    brush.radius = skip == 0 ? 0 : 1;
    brush.grabTo = float3(0, skip == 1 ? 0.0f : 1.0f, 0);
    textureCalls = 0;
    command::kelvinlet<KelvinletProbe, AccumLive>(context);
    test_assert(textureCalls == 0 && !node.updated && node.affected_verts.size() == 0);
    test_assert(probe.point.co.length() == 0);
  }
  brush.texture_program = nullptr;
  fprintf(
      stderr,
      "Kelvinlet double reference passed %zu scalar endpoint cases and cutoff extremes\n",
      cases);
}

static void unboundedSnapshots()
{
  Brush brush;
  brush.unboundedExtent = 7;
  PreparedBrushScalars inherited, declared;
  test_assert(prepareBrushScalars(brush, {}, brush.deviceInputCtx, inherited).error ==
              PropError::ERROR_NONE);
  test_assert(inherited.unboundedExtent() == 7);
  const BrushUniformManifestEntry manifest{
      "unboundedExtent", true, true, 0, false, 0, 0, -1, props::Prop::FLOAT32, true};
  const BrushScalarOverride override{"unboundedExtent", props::Prop::FLOAT32, 3};
  test_assert(prepareBrushScalars(
                  brush, {&manifest, 1}, brush.deviceInputCtx, declared, {&override, 1})
                  .error == PropError::ERROR_NONE);
  test_assert(declared.unboundedExtent() == 3 && inherited.unboundedExtent() == 7 &&
              brush.unboundedExtent == 7);
  test_assert(
      prepareBrushScalars(brush, {}, brush.deviceInputCtx, inherited, {&override, 1})
          .error == PropError::ERROR_NOT_EXISTS);
  test_assert(inherited.unboundedExtent() == 7);
  test_assert(validateUnboundedSupport(inherited, .5f).error == PropError::ERROR_NONE);
  for (auto [radius, extent] : {std::pair{1e30f, 1e30f},
                                std::pair{1e-20f, 1e-30f},
                                std::pair{1.0f, std::numeric_limits<float>::denorm_min()},
                                std::pair{1.0f, std::numeric_limits<float>::infinity()}})
  {
    brush.unboundedExtent = extent;
    test_assert(prepareBrushScalars(brush, {}, brush.deviceInputCtx, inherited).error ==
                PropError::ERROR_NONE);
    test_assert(validateUnboundedSupport(inherited, radius).error ==
                PropError::ERROR_INVALID_VALUE);
    test_assert(validateUnboundedSupport(inherited, 0).error == PropError::ERROR_NONE);
  }
  for (float extent : {-1.0f, 0.0f}) {
    brush.unboundedExtent = extent;
    test_assert(prepareBrushScalars(brush, {}, brush.deviceInputCtx, inherited).error ==
                PropError::ERROR_NONE);
    test_assert(validateUnboundedSupport(inherited, std::ldexp(1.0f, -127)).error ==
                PropError::ERROR_NONE);
    test_assert(
        validateUnboundedSupport(inherited, std::nextafter(std::ldexp(1.0f, -128), 1.0f))
            .error == PropError::ERROR_NONE);
    test_assert(validateUnboundedSupport(inherited, std::ldexp(1.0f, -128)).error ==
                PropError::ERROR_INVALID_VALUE);
  }
  brush.grabTo[1] = std::numeric_limits<float>::quiet_NaN();
  test_assert(validateUnboundedSupport(inherited, 1).error ==
              PropError::ERROR_INVALID_VALUE);
  fprintf(stderr,
          "unbounded extent snapshots, override eligibility and numerical preflight "
          "passed\n");
}

static bool exactUnboundedPositions(const std::vector<float3> &a,
                                    const std::vector<float3> &b)
{
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); i++)
    for (int axis = 0; axis < 3; axis++)
      if (a[i][axis] != b[i][axis])
        return false;
  return true;
}

static void unboundedExecution(bool grid,
                               bool programMode,
                               bool cavity,
                               bool deferred,
                               float extent,
                               CommandExecutor::NeighborMode neighbors)
{
  auto *mesh = mesh::createCube(grid ? 4 : 12, 2.0f);
  {
    subdiv::Multires mr;
    mr.init(*mesh, 2);
    auto *domain = mr.gridDomain(2);
    auto *gridTree = domain->ensureTree(50);
    spatial::SpatialTree tree(mesh);
    tree.leaf_limit = 32;
    tree.buildAll();
    test_assert(tree.leaves().size() > 1 && gridTree->leaves.size() > 1);
    meshlog::MeshLog meshLog;
    meshLog.setActiveMesh(mesh);
    subdiv::GridStrokeLog gridLog;
    Brush brush;
    brush.radius = .25f;
    brush.strength = .12f;
    brush.unboundedExtent = extent;
    brush.grabFrom = float3(.2f, -.1f, 1.1f);
    brush.grabTo = float3(.03f, -.02f, .08f);
    brush.automask_cavity = cavity;
    brush.cavity_factor = 0;
    brush.writeProps();
    CommandExecutor meshEx(&tree, &brush);
    meshEx.meshLog = &meshLog;
    meshEx.anchoredGrab = false;
    meshEx.neighborMode = neighbors;
    GridBrushExecutor gridEx(domain, &brush, &gridLog);
    gridEx.anchoredGrab = false;
    gridEx.deferNormals = deferred;
    if (grid)
      gridEx.beginStep();
    else
      meshEx.beginStep(false);
    auto positions = [&]() {
      std::vector<float3> output;
      const int count = grid ? domain->vertCount() : mesh->v.count;
      for (int v = 0; v < count; v++)
        output.push_back(grid ? domain->pos()[v] : mesh->v.co[v]);
      return output;
    };
    const auto original = positions();
    auto expected = original;
    std::vector<bool> contacted(original.size(), false);
    BrushProgram program;
    if (programMode)
      program.addCommand(int(SculptBrushes::DRAW));
    const int field = program.addCommand(int(SculptBrushes::KELVINLET));
    if (programMode) {
      program.setCommandScalarChecked(0, "radius", props::Prop::FLOAT32, .25);
      program.setCommandScalarChecked(field, "mu", props::Prop::FLOAT32, 2);
      BrushDynamicsOverride material{"mu", props::Prop::FLOAT32, {}};
      material.dynamics.configure(
          props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
      program.commands[field].dynamicsOverrides.append(std::move(material));
      program.setCommandScalarChecked(field, "nu", props::Prop::FLOAT32, .1f);
      BrushDynamicsOverride poisson{"nu", props::Prop::FLOAT32, {}};
      poisson.dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::ADD, .25f);
      program.commands[field].dynamicsOverrides.append(std::move(poisson));
    }
    float3 center(0, 0, 1);
    const float3 normal(0, 0, 1);
    auto run = [&](bool validateOnly = false) {
      if (programMode)
        return grid ? gridEx.applyResolvedProgram(&program, center, normal, validateOnly)
                    : meshEx.applyResolvedProgram(&program, center, normal, validateOnly);
      return grid ? gridEx.applyResolvedDab(
                        SculptBrushes::KELVINLET, center, normal, validateOnly)
                  : meshEx.applyResolvedDab(
                        SculptBrushes::KELVINLET, center, normal, validateOnly);
    };
    // Accepted zero and missed first dabs must not poison later support/cache setup.
    brush.unboundedExtent = 1;
    if (programMode)
      program.setCommandScalarChecked(0, "radius", props::Prop::FLOAT32, 0);
    for (float initialRadius : {0.0f, 1.0f}) {
      center = float3(100.0f);
      if (programMode)
        program.setCommandScalarChecked(
            field, "radius", props::Prop::FLOAT32, initialRadius);
      else
        *source(brush, "radius")->internal_value() = initialRadius;
      test_assert(run().error == PropError::ERROR_NONE);
      test_assert(exactUnboundedPositions(positions(), original));
    }
    center = float3(0, 0, 1);
    brush.unboundedExtent = extent;
    if (programMode)
      program.setCommandScalarChecked(0, "radius", props::Prop::FLOAT32, .25f);
    for (int dab = 0; dab < 2; dab++) {
      const float radius = dab == 0 ? 1.3f : 2.7f;
      const float pressure = dab == 0 ? .4f : .8f;
      brush.pushDeviceInput(int(props::DeviceType::PRESSURE), pressure);
      if (programMode)
        program.setCommandScalarChecked(field, "radius", props::Prop::FLOAT32, radius);
      else
        *source(brush, "radius")->internal_value() = radius;
      // Validation must inspect the entire late candidate before any query/capture.
      const auto prior = positions();
      const auto oldPath = brush.strokePathCount;
      const auto oldGrid = grid ? gridState(gridEx) : std::string();
      const auto evaluations = grid ? gridEx.preparedCavity.evaluations()
                                    : meshEx.preparedCavity.evaluations();
      const auto topology = mesh->topo_stamp;
      auto rejected = [&]() {
        for (bool only : {false, true})
          test_assert(run(only).error == PropError::ERROR_INVALID_VALUE);
        test_assert(exactUnboundedPositions(positions(), prior) &&
                    brush.strokePathCount == oldPath);
        test_assert(mesh->topo_stamp == topology);
        test_assert((grid ? gridEx.preparedCavity.evaluations()
                          : meshEx.preparedCavity.evaluations()) == evaluations);
        if (grid)
          test_assert(gridState(gridEx) == oldGrid);
      };
      for (float invalid : {std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::max()})
      {
        brush.unboundedExtent = invalid;
        rejected();
      }
      for (auto [badRadius, badExtent] :
           {std::pair{1e-20f, 1e-30f},
            std::pair{1.0f, std::numeric_limits<float>::denorm_min()}})
      {
        if (programMode)
          program.setCommandScalarChecked(
              field, "radius", props::Prop::FLOAT32, badRadius);
        else
          *source(brush, "radius")->internal_value() = badRadius;
        brush.unboundedExtent = badExtent;
        rejected();
      }
      if (programMode)
        program.setCommandScalarChecked(field, "radius", props::Prop::FLOAT32, radius);
      else
        *source(brush, "radius")->internal_value() = radius;
      brush.unboundedExtent = extent;
      for (auto *vector : {&brush.grabFrom, &brush.grabTo})
        for (int axis = 0; axis < 3; axis++) {
          const float saved = (*vector)[axis];
          for (float invalid : {std::numeric_limits<float>::infinity(),
                                std::numeric_limits<float>::quiet_NaN()})
          {
            (*vector)[axis] = invalid;
            rejected();
          }
          (*vector)[axis] = saved;
        }
      test_assert(exactUnboundedPositions(positions(), prior) &&
                  brush.strokePathCount == oldPath);
      if (grid)
        test_assert(gridState(gridEx) == oldGrid);
      test_assert(run(true).error == PropError::ERROR_NONE);
      test_assert(exactUnboundedPositions(positions(), prior) &&
                  brush.strokePathCount == oldPath);
      const float mu = programMode ? 2.0f * pressure : 1.0f;
      const float nu = programMode ? .1f + pressure * .25f : .4f;
      for (size_t v = 0; v < expected.size(); v++) {
        auto &p = expected[v];
        if (programMode) {
          const float t = 1.0f - std::min((p - center).length() / .25f, 1.0f);
          const float falloff = t * t * (3.0f - 2.0f * t);
          if (cavity && t > 0)
            contacted[v] = true;
          p += normal * (falloff * .12f * .25f * .5f * (cavity ? .5f : 1.0f));
        }
        const double window = cutoffReference(p, center, radius, extent);
        if (cavity && (extent <= 0 || std::hypot(double(p[0]) - double(center[0]),
                                                 double(p[1]) - double(center[1]),
                                                 double(p[2]) - double(center[2])) <
                                          double(radius * extent)))
          contacted[v] = true;
        const auto delta =
            kelvinletReference(p, brush.grabFrom, brush.grabTo, radius, mu, nu);
        for (int axis = 0; axis < 3; axis++)
          p[axis] = float(double(p[axis]) + delta[axis] * window * (cavity ? .5 : 1.0));
      }
      test_assert(run().error == PropError::ERROR_NONE);
      const auto actual = positions();
      for (size_t v = 0; v < actual.size(); v++)
        test_assert((actual[v] - expected[v]).length() < 3e-6f);
      test_assert(brush.unboundedExtent == extent);
      if (programMode)
        test_assert(brush.radius == .25f && brush.mu == 1 && brush.nu == .4f);
      if (cavity) {
        size_t count = 0;
        for (bool touched : contacted)
          count += touched;
        test_assert((grid ? gridEx.preparedCavity.evaluations()
                          : meshEx.preparedCavity.evaluations()) == count);
      }
    }
    int outside = 0;
    const auto after = positions();
    for (size_t v = 0; v < after.size(); v++)
      outside += (original[v] - center).length() > .5f &&
                 (after[v] - original[v]).length() > 1e-5f;
    test_assert(outside > 0);
    if (grid) {
      gridEx.endStep();
      gridLog.undo();
    } else {
      meshEx.endStep();
      meshLog.undo(mesh, &tree);
    }
    test_assert(exactUnboundedPositions(positions(), original));
    if (grid)
      gridLog.redo();
    else
      meshLog.redo(mesh, &tree);
    test_assert(exactUnboundedPositions(positions(), after));
  }
  alloc::Delete(mesh);
}

template <class Executor, class Source, class Co>
static void unboundedCavity(Executor &ex, Brush &brush, Source source, Co co, int count)
{
  brush.automask_cavity = true;
  brush.cavity_factor = .05f;
  brush.cavity_blur_steps = 1;
  brush.grabTo = float3(0.0f);
  brush.writeProps();
  ex.anchoredGrab = false;
  BrushProgram program;
  program.addCommand(int(SculptBrushes::KELVINLET));
  std::vector<float> saved(count, -1);
  CavityParams params;
  params.enabled = true;
  params.factor = .05f;
  params.blur_steps = 1;
  params.curve_lut = brush.cavity_curve.data();
  CavityScratch scratch;
  for (int v = 0; v < count; v++)
    co(v)[2] += .17f * float(v % 5 - 2);
  size_t contacts = 0;
  for (int dab = 0; dab < 3; dab++) {
    if (dab)
      for (int v = 0; v < count; v++)
        co(v)[2] += .017f * float(v % 5 - 2);
    // Vertex zero is exactly on the first cutoff. No first contact occurs there.
    const float radius = dab == 0 ? .5f : 2;
    const float3 center = dab == 0 ? co(0) + float3(.5f, 0, 0) : float3(0.0f);
    brush.unboundedExtent = dab == 2 ? 0 : 1;
    program.setCommandScalarChecked(0, "radius", props::Prop::FLOAT32, radius);
    program.setCommandScalarChecked(0, "mu", props::Prop::FLOAT32, 1 + dab);
    program.setCommandScalarChecked(0, "nu", props::Prop::FLOAT32, .1f * dab);
    std::vector<bool> supported(count);
    for (int v = 0; v < count; v++) {
      supported[v] = dab == 2 || std::hypot(double(co(v)[0]) - center[0],
                                            double(co(v)[1]) - center[1],
                                            double(co(v)[2]) - center[2]) < radius;
      if (supported[v] && saved[v] < 0) {
        saved[v] = cavityRemap(params, cavityRawT(source, v, 1, scratch));
        contacts++;
      }
    }
    if (dab == 0)
      test_assert(!supported[0] && saved[0] < 0);
    test_assert(ex.applyResolvedProgram(&program, center, float3(0, 0, 1)).error ==
                PropError::ERROR_NONE);
    test_assert(ex.preparedCavity.configurations() == size_t(contacts != 0));
    test_assert(ex.preparedCavity.evaluations() == contacts);
    for (int v = 0; v < count; v++)
      if (supported[v])
        test_assert((*ex.preparedCavity.active())[v] == saved[v]);
  }
  test_assert(contacts == size_t(count));
  const auto [minimum, maximum] = std::minmax_element(saved.begin(), saved.end());
  test_assert(*maximum - *minimum > .01f);
}

static void unboundedCavityMatrix()
{
  for (bool grid : {false, true}) {
    auto *mesh = mesh::createCube(4, 2.0f);
    mesh->topo_cache.ensureRing1(*mesh);
    {
      subdiv::Multires mr;
      mr.init(*mesh, 2);
      auto *domain = mr.gridDomain(2);
      domain->ensureTree(50);
      spatial::SpatialTree tree(mesh);
      tree.leaf_limit = 32;
      tree.buildAll();
      Brush brush;
      if (grid) {
        GridBrushExecutor ex(domain, &brush);
        ex.beginStep();
        unboundedCavity(
            ex,
            brush,
            GridBrushExecutor::GridCavitySrc{domain},
            [&](int v) -> float3 & { return domain->pos()[v]; },
            domain->vertCount());
        ex.endStep();
      } else {
        CommandExecutor ex(&tree, &brush);
        ex.beginStep(false);
        unboundedCavity(
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
  fprintf(stderr,
          "unbounded nonconstant cavity first-contact and strict support passed\n");
}

#include "unbounded_cavity_transition.h"

static void unboundedMatrix()
{
  unboundedCavityTransitionMatrix();
  unboundedCavityMatrix();
  for (bool grid : {false, true})
    for (bool program : {false, true})
      for (bool cavity : {false, true})
        for (float extent : {0.0f, 1.0f})
          for (bool deferred : {false, true}) {
            if (!grid && deferred)
              continue;
            for (auto neighbors : {CommandExecutor::NeighborMode::LiveDisk,
                                   CommandExecutor::NeighborMode::Csr})
            {
              if (grid && neighbors == CommandExecutor::NeighborMode::Csr)
                continue;
              unboundedExecution(grid, program, cavity, deferred, extent, neighbors);
            }
          }
  fprintf(stderr,
          "prepared unbounded mesh/grid independent field oracle, growth, cavity and "
          "exact undo passed\n");
}
