#pragma once
#include "brush/c-api/stroke_inputs.h"

static mesh::Mesh *preparedDyntopoGrid()
{
  auto *m = alloc::New<mesh::Mesh>("prepared dyntopo grid");
  constexpr int n = 13;
  int verts[n * n];
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++) {
      float fx = float(x) / 6 - 1, fy = float(y) / 6 - 1;
      verts[y * n + x] = m->make_vertex(float3(fx, fy, .04f * std::sin(3 * fx + fy)));
    }
  for (int y = 0; y < n - 1; y++)
    for (int x = 0; x < n - 1; x++) {
      int a = verts[y * n + x], b = verts[y * n + x + 1];
      int c = verts[(y + 1) * n + x + 1], d = verts[(y + 1) * n + x];
      int first[] = {a, b, c}, second[] = {a, c, d};
      m->make_face(std::span<int>(first, 3));
      m->make_face(std::span<int>(second, 3));
    }
  m->recalc_normals();
  return m;
}

static std::vector<double> preparedDyntopoState(mesh::Mesh *m)
{
  std::vector<double> values{double(m->v.count),
                             double(m->e.count),
                             double(m->f.count),
                             double(m->c.count),
                             double(m->l.count)};
  for (int v : m->v) {
    values.push_back(v);
    for (int axis = 0; axis < 3; axis++)
      values.push_back(m->v.co[v][axis]);
  }
  for (int e : m->e) {
    values.push_back(e);
    values.push_back(m->e.vs[e][0]);
    values.push_back(m->e.vs[e][1]);
  }
  return values;
}

static std::vector<double>
preparedDyntopoGeometry(bool integrated, bool nonAccum, bool cavity)
{
  auto *m = preparedDyntopoGrid();
  std::vector<double> result;
  {
    spatial::SpatialTree tree(m);
    tree.leaf_limit = 12;
    tree.buildAll();
    test_assert(tree.leaves().size() > 4);
    meshlog::MeshLog log;
    log.setActiveMesh(m);
    Brush brush;
    brush.radius = .35f;
    brush.strength = .15f;
    brush.automask_cavity = cavity;
    brush.cavity_factor = .2f;
    brush.cavity_blur_steps = 1;
    brush.writeProps();
    source(brush, "radius")
        ->dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::ADD, 1);
    brush.pushDeviceInput(int(props::DeviceType::PRESSURE), .1f);
    CommandExecutor ex(&tree, &brush);
    ex.meshLog = &log;
    ex.nonAccum = nonAccum;
    ex.neighborMode = CommandExecutor::NeighborMode::Csr;
    ex.beginStep(true);
    ex.setStrokeGen(1);
    BrushProgram program;
    program.addCommand(int(SculptBrushes::DRAW));
    program.addCommand(int(SculptBrushes::DRAW));
    program.setCommandScalarChecked(1, "radius", props::Prop::FLOAT32, .8f);
    program.setCommandScalarChecked(1, "strength", props::Prop::FLOAT32, .08f);
    BrushDynamicsOverride independent;
    independent.name = "radius";
    independent.type = props::Prop::FLOAT32;
    program.commands[1].dynamicsOverrides.append(independent);
    auto original = preparedDyntopoState(m);
    dyntopo::DynTopoParams params;
    params.l_max = .11f;
    params.l_min = .02f;
    params.max_rounds = 6;
    params.max_splits = 70;
    params.mode = dyntopo::DynTopoMode::Subdivide;
    const float3 normal(0, 0, 1);
    auto apply = [&](float3 center, dyntopo::DynTopoParams *p, uint32_t seed) {
      if (!integrated && p) {
        ex.applyDynTopoDab(center, .4f, p, seed);
        tree.updateQueries();
      }
      auto status = integrated
                        ? MeshStroke_dabProgramResolvedDyntopo(&ex,
                                                               &program,
                                                               center[0],
                                                               center[1],
                                                               center[2],
                                                               normal[0],
                                                               normal[1],
                                                               normal[2],
                                                               .4f,
                                                               p,
                                                               seed)
                        : int(ex.applyResolvedProgram(&program, center, normal).error);
      if (!integrated && p)
        log.pushTopoChunk();
      test_assert(integrated ? status >= 0 : status == int(PropError::ERROR_NONE));
      test_assert(!m->topo_frozen);
    };
    program.setCommandScalarChecked(1, "radius", props::Prop::INT32, 1);
    test_assert(ex.applyResolvedProgram(
                      &program, float3(0.0f), normal, false, false, &params, .4f, 17)
                    .error != PropError::ERROR_NONE);
    test_assert(preparedDyntopoState(m) == original && ex.isFirstOfStep);
    program.setCommandScalarChecked(1, "radius", props::Prop::FLOAT32, .8f);
    params.l_max = std::numeric_limits<float>::quiet_NaN();
    test_assert(ex.applyResolvedProgram(
                      &program, float3(0.0f), normal, false, false, &params, .4f, 17)
                    .error != PropError::ERROR_NONE);
    test_assert(preparedDyntopoState(m) == original);
    params.l_max = .11f;
    test_assert(ex.applyResolvedProgram(
                      &program, float3(0.0f), normal, true, false, &params, .4f, 17)
                    .error == PropError::ERROR_NONE);
    test_assert(preparedDyntopoState(m) == original);
    apply(float3(-.15f, 0, 0), &params, 17);
    test_assert(ex.lastDynTopoStats.splits > 0 && m->v.count > int(original[0]));
    int outerMoved = 0;
    for (size_t i = 5; i < 5 + size_t(original[0]) * 4; i += 4) {
      const int v = int(original[i]);
      float3 co{float(original[i + 1]), float(original[i + 2]), float(original[i + 3])};
      float distance = (co - float3(-.15f, 0, 0)).length();
      if (distance > .55f && distance < .7f) {
        test_assert(m->v.co[v][2] > co[2]);
        outerMoved++;
      }
    }
    test_assert(outerMoved > 0);
    auto afterSplit = preparedDyntopoState(m);
    result.insert(result.end(), afterSplit.begin(), afterSplit.end());
    const auto topologyStamp = m->topo_stamp;
    apply(float3(.15f, 0, 0), nullptr, 18);
    test_assert(m->topo_stamp == topologyStamp && preparedDyntopoState(m) != afterSplit);
    auto afterSecond = preparedDyntopoState(m);
    result.insert(result.end(), afterSecond.begin(), afterSecond.end());
    params.mode = dyntopo::DynTopoMode::Collapse;
    params.l_max = .4f;
    params.l_min = .17f;
    params.max_collapses = 30;
    apply(float3(.15f, 0, 0), &params, 19);
    test_assert(ex.lastDynTopoStats.collapses > 0);
    MeshCavitySrc cavitySource{m};
    test_assert(cavitySource.vertCap() == int(m->v.capacity()));
    for (int v : m->v)
      test_assert(v < cavitySource.vertCap());
    ex.endDynTopoStroke();
    ex.endStep();
    auto finalState = preparedDyntopoState(m);
    result.insert(result.end(), finalState.begin(), finalState.end());
    test_assert(finalState != original);
    log.undo(m, &tree);
    test_assert(preparedDyntopoState(m) == original);
    log.redo(m, &tree);
    test_assert(preparedDyntopoState(m) == finalState);
  }
  alloc::Delete(m);
  return result;
}

static void preparedDyntopoCases()
{
  for (bool nonAccum : {false, true})
    for (bool cavity : {false, true}) {
      auto integrated = preparedDyntopoGeometry(true, nonAccum, cavity);
      auto reference = preparedDyntopoGeometry(false, nonAccum, cavity);
      if (integrated != reference) {
        fprintf(stderr, "dyntopo mismatch nonAccum=%d cavity=%d sizes=%zu/%zu\n",
                nonAccum, cavity, integrated.size(), reference.size());
        size_t differences = 0;
        for (size_t i = 0; i < std::min(integrated.size(), reference.size()); i++) {
          if (integrated[i] != reference[i] && differences++ < 12)
            fprintf(stderr, "  [%zu] %.17g != %.17g\n", i, integrated[i], reference[i]);
        }
        fprintf(stderr, "  differences=%zu\n", differences);
      }
      test_assert(integrated == reference);
    }
  fprintf(stderr,
          "prepared dyntopo split/collapse, cadence, atomic rejection and undo passed\n");
}
