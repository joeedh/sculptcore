#pragma once

// Independent graph-distance oracle: build adjacency from edges, not the native
// CSR walk or EnhanceScratch, and sum each vertex once in index order.
static std::vector<float3>
enhanceOracle(mesh::Mesh *m, int outer, int inner, const std::vector<float3> &normals)
{
  outer = std::max(outer, 1);
  inner = std::clamp(inner, 0, outer);
  const int capacity = int(m->v.capacity());
  std::vector<std::vector<int>> graph(capacity);
  for (int edge : m->e) {
    int a = m->e.vs[edge][0], b = m->e.vs[edge][1];
    graph[a].push_back(b);
    graph[b].push_back(a);
  }
  std::vector<float3> result(capacity);
  for (int center : m->v) {
    std::vector<int> distance(capacity, -1), queue{center};
    distance[center] = 0;
    for (size_t head = 0; head < queue.size(); head++) {
      int v = queue[head];
      if (distance[v] >= outer)
        continue;
      for (int next : graph[v])
        if (distance[next] == -1) {
          distance[next] = distance[v] + 1;
          queue.push_back(next);
        }
    }
    float3 a(0.0f), b(0.0f), n(0.0f);
    int countA = 0, countB = 0;
    for (int v : m->v) {
      if (distance[v] < 0)
        continue;
      b += m->v.co[v];
      n += normals[v];
      countB++;
      if (distance[v] <= inner) {
        a += m->v.co[v];
        countA++;
      }
    }
    n = n.length() > 1e-6f ? n / n.length() : normals[center];
    result[center] = n * (a / float(countA) - b / float(countB)).dot(n);
  }
  return result;
}

static std::vector<float3> enhanceNormals(mesh::Mesh *m)
{
  std::vector<float3> normals(m->v.capacity());
  for (int v : m->v)
    normals[v] = m->v.no[v];
  return normals;
}

static void preparedEnhanceCases()
{
  for (bool nonAccum : {false, true})
    for (auto mode :
         {CommandExecutor::NeighborMode::LiveDisk, CommandExecutor::NeighborMode::Csr})
    {
      auto *m = mesh::createCube(4, 2.0f);
      {
        spatial::SpatialTree tree(m);
        tree.leaf_limit = 16;
        tree.buildAll();
        Brush brush;
        brush.radius = 4;
        brush.strength = 0;
        brush.enhance_rings = 4;
        brush.enhance_inner = 1;
        brush.writeProps();
        meshlog::MeshLog log;
        log.setActiveMesh(m);
        CommandExecutor ex(&tree, &brush);
        ex.meshLog = &log;
        ex.nonAccum = nonAccum;
        ex.neighborMode = mode;
        ex.beginStep(false);
        ex.setStrokeGen(1);
        test_assert(ex.supportsResolved(SculptBrushes::ENHANCE));
        const auto first = enhanceOracle(m, 4, 1, enhanceNormals(m));
        std::vector<float3> original(m->v.capacity());
        for (int v : m->v)
          original[v] = m->v.co[v];
        const auto attrs = m->v.attrs.attrs.size();
        test_assert(ex.applyResolvedDab(
                          SculptBrushes::ENHANCE, float3(0.0f), float3(0, 0, 1), true)
                        .error == PropError::ERROR_NONE);
        test_assert(m->v.attrs.attrs.size() == attrs &&
                    ex.preparedEnhance.configurations() == 0);
        // A missed first dab must not claim geometry. Zero strength inside the
        // support does claim it, even though no vertex moves.
        test_assert(
            ex.applyResolvedDab(SculptBrushes::ENHANCE, float3(100), float3(0, 0, 1))
                .error == PropError::ERROR_NONE);
        test_assert(ex.preparedEnhance.configurations() == 0);
        test_assert(
            ex.applyResolvedDab(SculptBrushes::ENHANCE, float3(0.0f), float3(0, 0, 1))
                .error == PropError::ERROR_NONE);
        test_assert(ex.preparedEnhance.evaluations() == size_t(m->v.count));
        for (int v : m->v)
          test_assert(((*ex.preparedEnhance.active())[v] - first[v]).length() < 2e-6f);
        test_assert(!m->v.attrs.has(mesh::AttrType::FLOAT3, ENHANCE_DISP_ATTR));
        test_assert(!m->v.attrs.has(mesh::AttrType::INT, ENHANCE_GEN_ATTR));
        BrushProgram program;
        program.addCommand(int(SculptBrushes::DRAW));
        program.addCommand(int(SculptBrushes::ENHANCE));
        program.setCommandFloat(0, int(BrushProp::Strength), .1f);
        program.setCommandFloat(1, int(BrushProp::Strength), 0);
        program.setCommandScalarChecked(1, "enhance_rings", props::Prop::INT32, 2);
        program.setCommandScalarChecked(1, "enhance_inner", props::Prop::INT32, 0);
        const auto beforeNormals = enhanceNormals(m);
        test_assert(
            ex.applyResolvedProgram(&program, float3(0.0f), float3(0, 0, 1)).error ==
            PropError::ERROR_NONE);
        const auto second = enhanceOracle(m, 2, 0, beforeNormals);
        test_assert(ex.preparedEnhance.configurations() == 2);
        for (int v : m->v)
          test_assert(((*ex.preparedEnhance.active())[v] - second[v]).length() < 2e-6f);
        test_assert(brush.enhance_rings == 4 && brush.enhance_inner == 1);
        const size_t evaluations = ex.preparedEnhance.evaluations();
        // Revisit A after the different B configuration and geometry changes.
        test_assert(
            ex.applyResolvedDab(SculptBrushes::ENHANCE, float3(0.0f), float3(0, 0, 1))
                .error == PropError::ERROR_NONE);
        test_assert(ex.preparedEnhance.evaluations() == evaluations);
        for (int v : m->v)
          test_assert(((*ex.preparedEnhance.active())[v] - first[v]).length() < 2e-6f);
        // Cached first contact remains fixed while pressure changes the applied strength.
        *source(brush, "strength")->internal_value() = .4f;
        source(brush, "strength")
            ->dynamics.configure(
                props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
        for (float pressure : {.25f, .75f}) {
          brush.pushDeviceInput(int(props::DeviceType::PRESSURE), pressure);
          std::vector<float3> expected(m->v.capacity());
          for (int v : m->v) {
            const auto point = nonAccum ? original[v] : m->v.co[v];
            const float t = 1.0f - std::min(point.length() / 4.0f, 1.0f);
            const float falloff = t * t * (3.0f - 2.0f * t);
            expected[v] = m->v.co[v] + first[v] * (.4f * pressure * falloff);
          }
          test_assert(
              ex.applyResolvedDab(SculptBrushes::ENHANCE, float3(0.0f), float3(0, 0, 1))
                  .error == PropError::ERROR_NONE);
          for (int v : m->v)
            test_assert((m->v.co[v] - expected[v]).length() < 2e-6f);
          test_assert(ex.preparedEnhance.evaluations() == evaluations);
        }
        // Late invalid settings must reject the entire program before DRAW.
        const auto stable = m->v.co[0];
        const auto stableAttrs = m->v.attrs.attrs.size();
        program.setCommandScalarChecked(1, "enhance_inner", props::Prop::FLOAT32, 1);
        test_assert(
            ex.applyResolvedProgram(&program, float3(0.0f), float3(0, 0, 1)).error ==
            PropError::ERROR_INVALID_TYPE);
        test_assert((m->v.co[0] - stable).length() == 0 &&
                    m->v.attrs.attrs.size() == stableAttrs);
        test_assert(ex.preparedEnhance.evaluations() == evaluations);
        program.setCommandScalarChecked(1, "enhance_inner", props::Prop::INT32, 0);
        program.setCommandScalarChecked(0, "enhance_inner", props::Prop::INT32, 0);
        test_assert(ex.applyResolvedProgram(&program, float3(0.0f), float3(0, 0, 1), true)
                        .error == PropError::ERROR_NOT_EXISTS);
        ex.endStep();
        log.undo(m, &tree);
        for (int v : m->v)
          test_assert((m->v.co[v] - original[v]).length() == 0);
        log.redo(m, &tree);
        test_assert((m->v.co[0] - stable).length() == 0);
        ex.beginStep(false);
        test_assert(ex.preparedEnhance.configurations() == 0);
        ex.endStep();
        m->thawTopo();
        m->topo_cache.ensureRing1(*m);
        EnhanceScratch scratch;
        auto expected = enhanceOracle(m, 2, 0, enhanceNormals(m));
        for (int v : m->v) {
          scratch.token = UINT32_MAX;
          test_assert((computeEnhanceDisp(m, v, {2, 0}, scratch) - expected[v]).length() <
                      2e-6f);
        }
        PreparedEnhanceCache cache;
        cache.beginGeometry(m);
        test_assert(&cache.entry(0, -1) == &cache.entry(1, 0));
        cache.fill(cache.entry(1, 0), m, 0);
        test_assert(cache.evaluations() == 1);
        m->topo_stamp++;
        cache.beginGeometry(m);
        test_assert(cache.configurations() == 0 && cache.evaluations() == 0);
      }
      alloc::Delete(m);
    }
  auto *sparse = mesh::createCube(2, 2.0f);
  std::vector<int> loose;
  for (int i = 0; i < 128; i++)
    loose.push_back(sparse->make_vertex(float3(10, 10, 10)));
  for (size_t i = 0; i + 1 < loose.size(); i++)
    sparse->kill_vertex(loose[i]);
  test_assert(loose.back() >= sparse->v.count);
  sparse->topo_cache.ensureRing1(*sparse);
  EnhanceScratch scratch;
  test_assert(
      computeEnhanceDisp(sparse, loose.back(), {INT32_MAX, 0}, scratch).length() == 0);
  test_assert(scratch.visitStamp.size() == sparse->v.capacity());
  alloc::Delete(sparse);
  fprintf(stderr,
          "prepared ENHANCE independent temporal oracle, restoration and undo passed\n");
}

static void preparedEnhanceSupport()
{
  for (int leafLimit : {16, 96})
    for (bool reverse : {false, true})
      for (bool nonAccum : {false, true}) {
        auto *m = mesh::createCube(4, 2.0f);
        {
          spatial::SpatialTree tree(m);
          tree.leaf_limit = leafLimit;
          tree.buildAll();
          Brush brush;
          brush.radius = 1;
          brush.strength = 0;
          brush.grabTo = float3(0.0f);
          brush.unboundedExtent = 0;
          brush.writeProps();
          CommandExecutor ex(&tree, &brush);
          ex.nonAccum = nonAccum;
          ex.anchoredGrab = false;
          ex.beginStep(false);
          ex.setStrokeGen(1);
          BrushProgram program;
          program.addCommand(int(SculptBrushes::ENHANCE));
          program.addCommand(int(SculptBrushes::ENHANCE));
          program.addCommand(int(SculptBrushes::KELVINLET));
          const int a = reverse ? 1 : 0, b = 1 - a;
          for (int slot : {a, b}) {
            program.setCommandScalarChecked(
                slot, "enhance_rings", props::Prop::INT32, slot == a ? 2 : 4);
            program.setCommandScalarChecked(
                slot, "enhance_inner", props::Prop::INT32, slot == a ? 0 : 1);
            program.setCommandScalarChecked(slot, "radius", props::Prop::FLOAT32, 0);
          }
          const float3 center(0, 0, 1), normal(0, 0, 1);
          auto run = [&](float3 point) {
            const auto result = ex.applyResolvedProgram(&program, point, normal);
            if (result.error != PropError::ERROR_NONE)
              fprintf(stderr,
                      "ENHANCE support rejection: %s (%d)\n",
                      result.name.c_str(),
                      int(result.error));
            test_assert(result.error == PropError::ERROR_NONE);
            test_assert(ex.lastDabNodeCount == int(tree.leaves().size()));
          };
          run(center);
          test_assert(ex.preparedEnhance.configurations() == 0);
          program.setCommandScalarChecked(a, "radius", props::Prop::FLOAT32, .7f);
          program.setCommandScalarChecked(b, "radius", props::Prop::FLOAT32, 1.3f);
          run(float3(100.0f));
          test_assert(ex.preparedEnhance.configurations() == 0);
          const auto firstA = enhanceOracle(m, 2, 0, enhanceNormals(m));
          const auto firstB = enhanceOracle(m, 4, 1, enhanceNormals(m));
          std::vector<bool> capturedA(m->v.capacity()), capturedB(m->v.capacity());
          size_t contacts = 0;
          for (int v : m->v) {
            capturedA[v] = (m->v.co[v] - center).length() < .7f;
            capturedB[v] = (m->v.co[v] - center).length() < 1.3f;
            contacts += capturedA[v] + capturedB[v];
          }
          test_assert(contacts > 0 && contacts < size_t(m->v.count) * 2);
          run(center);
          test_assert(ex.preparedEnhance.configurations() == 2);
          test_assert(ex.preparedEnhance.evaluations() == contacts);
          run(center);
          test_assert(ex.preparedEnhance.evaluations() == contacts);
          for (int v : m->v)
            m->v.co[v][2] += .01f * float(v % 3);
          const auto laterA = enhanceOracle(m, 2, 0, enhanceNormals(m));
          const auto laterB = enhanceOracle(m, 4, 1, enhanceNormals(m));
          program.setCommandScalarChecked(a, "radius", props::Prop::FLOAT32, 2.5f);
          program.setCommandScalarChecked(b, "radius", props::Prop::FLOAT32, 2.5f);
          run(center);
          size_t grown = 0;
          for (int v : m->v) {
            const bool now = (m->v.co[v] - center).length() < 2.5f;
            for (bool configA : {true, false}) {
              const bool prior = configA ? capturedA[v] : capturedB[v];
              auto &entry = ex.preparedEnhance.entry(configA ? 2 : 4, configA ? 0 : 1);
              auto *cached = entry.vectors.lookup_ptr(v);
              test_assert(bool(cached) == (prior || now));
              if (cached) {
                const auto expected = configA ? (prior ? firstA[v] : laterA[v])
                                              : (prior ? firstB[v] : laterB[v]);
                test_assert((*cached - expected).length() < 2e-6f);
              }
              grown += !prior && now;
            }
          }
          test_assert(grown > 0 && ex.preparedEnhance.evaluations() == contacts + grown);
          ex.endStep();
        }
        alloc::Delete(m);
      }
  fprintf(stderr,
          "prepared ENHANCE independent support, growth, command order and unbounded "
          "union passed\n");
}
