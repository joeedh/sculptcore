#pragma once
#include "prepared_dyntopo_cases.h"

static std::vector<double> preparedAttributeState(mesh::Mesh *m)
{
  std::vector<double> state{double(m->v.count), double(m->e.count), double(m->f.count)};
  for (int v : m->v) {
    state.push_back(v);
    for (int axis = 0; axis < 3; axis++)
      state.push_back(m->v.co[v][axis]);
  }
  for (auto domain : {AttrElemDomain::Vertex, AttrElemDomain::Face}) {
    auto &group = domain == AttrElemDomain::Vertex ? m->v.attrs : m->f.attrs;
    for (const char *name : {"color", "second", "group", "layerA", "layerB"})
      for (auto &ref : group.attrs) {
        if (ref.name != string(name))
          continue;
        const int count = domain == AttrElemDomain::Vertex ? int(m->v.capacity())
                                                           : int(m->f.capacity());
        for (int i = 0; i < count; i++) {
          if (domain == AttrElemDomain::Vertex ? m->v.freemap[i] : m->f.freemap[i])
            continue;
          if (ref.type == mesh::AttrType::INT)
            state.push_back(ref.get_data<int>()->safe_get(i));
          else if (ref.type == mesh::AttrType::FLOAT4)
            for (int axis = 0; axis < 4; axis++)
              state.push_back(ref.get_data<float4>()->safe_get(i)[axis]);
          else if (ref.type == mesh::AttrType::FLOAT3)
            for (int axis = 0; axis < 3; axis++)
              state.push_back(ref.get_data<float3>()->safe_get(i)[axis]);
        }
      }
  }
  return state;
}

static std::vector<double> preparedAttributeRun(SculptBrushes tool,
                                                bool resolved,
                                                bool programMode,
                                                bool preview,
                                                bool dyntopo = false)
{
  auto *m = preparedDyntopoGrid();
  m->ensureFaceGroups();
  for (const char *name : {"color", "second"}) {
    auto ref = m->v.attrs.ensure(mesh::AttrType::FLOAT4, name, true);
    for (int v : m->v)
      (*ref.get_data<float4>())[v] = float4(.1f + .01f * (v % 7), .2f, .3f, 1);
  }
  m->addSculptLayerNamed("layerA");
  m->addSculptLayerNamed("layerB");
  std::vector<double> result;
  {
    spatial::SpatialTree tree(m);
    tree.leaf_limit = 12;
    tree.buildAll();
    test_assert(tree.leaves().size() > 4);
    meshlog::MeshLog log;
    log.setActiveMesh(m);
    Brush brush;
    brush.radius = .8f;
    brush.strength = .3f;
    brush.brushColor = float4(.8f, .1f, .5f, 1);
    brush.activeGroup = 5;
    brush.writeProps();
    CommandExecutor ex(&tree, &brush);
    ex.meshLog = &log;
    ex.beginStep(dyntopo);
    ex.setStrokeGen(1);
    BrushProgram program;
    program.addCommand(int(tool));
    if (tool == SculptBrushes::LAYERDRAW) {
      ex.defaultAttrOverrides.append({0, m->sculptLayerAttrIndex(0)});
      program.commands[0].attrLayerOverrides.append({0, m->sculptLayerAttrIndex(0)});
    }
    if (programMode) {
      program.addCommand(int(tool));
      program.setCommandFloat(1, int(BrushProp::Radius), 1.3f);
      program.setCommandFloat(1, int(BrushProp::Strength), .1f);
      if (tool == SculptBrushes::LAYERDRAW)
        program.commands[1].attrLayerOverrides.append({0, m->sculptLayerAttrIndex(1)});
      if (tool == SculptBrushes::COLOR || tool == SculptBrushes::COLORSMOOTH)
        for (int i = 0; i < int(m->v.attrs.attrs.size()); i++)
          if (m->v.attrs.attrs[i].name == string("second"))
            program.commands[1].attrLayerOverrides.append({0, i});
    }
    auto original = preparedAttributeState(m);
    auto checkOriginal = [&](const char *phase) {
      const auto current = preparedAttributeState(m);
      if (current != original) {
        fprintf(
            stderr,
            "attribute mismatch tool=%d program=%d preview=%d phase=%s sizes=%zu/%zu\n",
            int(tool),
            programMode,
            preview,
            phase,
            current.size(),
            original.size());
        for (size_t i = 0; i < std::min(current.size(), original.size()); i++)
          if (current[i] != original[i]) {
            fprintf(stderr,
                    "first mismatch %zu: %.12g / %.12g\n",
                    i,
                    current[i],
                    original[i]);
            break;
          }
      }
      test_assert(current == original);
    };
    const float3 center(0.0f), normal(0, 0, 1);
    auto apply = [&]() {
      if (dyntopo) {
        dyntopo::DynTopoParams params;
        params.l_max = .11f;
        params.l_min = .02f;
        params.mode = dyntopo::DynTopoMode::Subdivide;
        params.max_rounds = 4;
        params.max_splits = 30;
        ex.applyDynTopoDab(center, .6f, &params, 19);
        tree.updateQueries();
      }
      if (resolved) {
        auto status = programMode ? ex.applyResolvedProgram(&program, center, normal)
                                  : ex.applyResolvedDab(tool, center, normal);
        if (status.error != PropError::ERROR_NONE)
          fprintf(
              stderr, "attribute tool %d rejected: %s\n", int(tool), status.name.c_str());
        test_assert(status.error == PropError::ERROR_NONE);
      } else if (programMode) {
        Vector<spatial::SpatialNode *> nodes;
        tree.filterNodes(center, 1.3f, nodes);
        ex.execProgram(&program, &nodes, center, normal);
        tree.updateQueries();
      } else {
        ex.applyDab(tool, center, normal, .8f, nullptr, 0);
      }
      if (dyntopo && (resolved || programMode))
        log.pushTopoChunk();
    };
    if (preview) {
      ex.beginPreviewDab(center, .01f);
      apply();
      ex.rollbackPreviewDab();
      tree.updateQueries();
      checkOriginal("preview rollback");
      ex.beginPreviewDab(center, .01f);
    }
    apply();
    apply();
    result = preparedAttributeState(m);
    test_assert(result != original);
    if (preview)
      ex.commitPreviewDab();
    if (dyntopo)
      ex.endDynTopoStroke();
    ex.endStep();
    if (resolved) {
      log.undo(m, &tree);
      checkOriginal("undo");
      log.redo(m, &tree);
      test_assert(preparedAttributeState(m) == result);
    }
  }
  alloc::Delete(m);
  return result;
}

static void preparedAttributeRejection()
{
  auto *m = preparedDyntopoGrid();
  {
    spatial::SpatialTree tree(m);
    tree.leaf_limit = 12;
    tree.buildAll();
    meshlog::MeshLog log;
    log.setActiveMesh(m);
    Brush brush;
    brush.radius = .8f;
    brush.strength = .3f;
    brush.writeProps();
    CommandExecutor ex(&tree, &brush);
    ex.meshLog = &log;
    ex.beginStep(false);
    BrushProgram program;
    program.addCommand(int(SculptBrushes::COLOR));
    program.addCommand(int(SculptBrushes::COLOR));
    auto original = preparedAttributeState(m);
    auto attrCount = m->v.attrs.attrs.size();
    bool hadColor = m->v.attrs.has(mesh::AttrType::FLOAT4, "color");
    for (auto selection : {BrushAttrLayerOverride{999, 0}, {0, -1}, {0, 999999}, {0, 0}})
    {
      program.commands[1].attrLayerOverrides.clear();
      program.commands[1].attrLayerOverrides.append(selection);
      test_assert(
          ex.applyResolvedProgram(&program, float3(0.0f), float3(0, 0, 1)).error !=
          PropError::ERROR_NONE);
      test_assert(preparedAttributeState(m) == original);
      test_assert(m->v.attrs.attrs.size() == attrCount && ex.isFirstOfStep);
    }
    program.commands[1].attrLayerOverrides.clear();
    test_assert(
        ex.applyResolvedProgram(&program, float3(0.0f), float3(0, 0, 1), true).error ==
        PropError::ERROR_NONE);
    test_assert(m->v.attrs.attrs.size() == attrCount);
    BrushAttrManifestEntry entries[2];
    for (auto &entry : entries) {
      entry.handle = "conflict";
      entry.materialize = true;
      entry.kernelWrites = true;
    }
    entries[1].type = mesh::AttrType::INT;
    Vector<BrushAttrManifestEntry> planned;
    test_assert(validatePreparedAttributes(*m, {entries, 1}, {}, planned).error ==
                PropError::ERROR_NONE);
    test_assert(validatePreparedAttributes(*m, {entries + 1, 1}, {}, planned).error ==
                PropError::ERROR_SCHEMA_CONFLICT);
    test_assert(m->v.attrs.attrs.size() == attrCount);
    ex.beginPreviewDab(float3(0.0f), .01f);
    test_assert(ex.applyResolvedProgram(&program, float3(0.0f), float3(0, 0, 1)).error ==
                PropError::ERROR_NONE);
    ex.rollbackPreviewDab();
    auto color = m->v.attrs.find_attribute(mesh::AttrType::FLOAT4, "color");
    test_assert(color.data != nullptr);
    if (hadColor)
      test_assert(preparedAttributeState(m) == original);
    else
      for (int v : m->v)
        test_assert(color.get_data<float4>()->safe_get(v).lengthSqr() == 0);
    ex.endStep();
  }
  alloc::Delete(m);
}

static void preparedAttributeCases()
{
  preparedAttributeRejection();
  for (auto tool : {SculptBrushes::COLOR,
                    SculptBrushes::COLORSMOOTH,
                    SculptBrushes::POLYGROUP,
                    SculptBrushes::LAYERDRAW,
                    SculptBrushes::FEATURE_ALIGN})
  {
    fprintf(stderr, "prepared attribute tool %d\n", int(tool));
    for (bool program : {false, true}) {
      auto resolved = preparedAttributeRun(tool, true, program, false);
      auto reference = preparedAttributeRun(tool, false, program, false);
      test_assert(resolved.size() == reference.size());
      for (size_t i = 0; i < reference.size(); i++)
        test_assert(std::abs(resolved[i] - reference[i]) < 1e-6);
      auto preview = preparedAttributeRun(tool, true, program, true);
      test_assert(preview == resolved);
      auto topology = preparedAttributeRun(tool, true, program, false, true);
      auto topologyReference = preparedAttributeRun(tool, false, program, false, true);
      test_assert(topology.size() == topologyReference.size());
      for (size_t i = 0; i < topologyReference.size(); i++)
        test_assert(std::abs(topology[i] - topologyReference[i]) < 1e-6);
    }
  }
  fprintf(stderr, "prepared attributes, selected layers, preview and undo passed\n");
}
