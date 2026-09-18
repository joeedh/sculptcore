#include "brush/brush_executor.h"
#include "brush/brush_preparation.h"
#include "brush/grid_executor.h"
#include "mesh/mesh_shapes.h"
#include "test_util.h"
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace sculptcore::brush {
inline constexpr int kExtraSlot_prelude_gain = 30;
}
#include "prepared_preludes.brush.gen.h"

test_init;
using namespace sculptcore;
using namespace sculptcore::brush;
using props::PropError;

static void generatedPreludeExecution()
{
  CommandExecutor::brush_command meshCommand;
  GridBrushExecutor::brush_command gridCommand;
  command::createPreparedpreludetestBrush<CommandExecutor>(meshCommand);
  command::createPreparedpreludetestBrush<GridBrushExecutor>(gridCommand);
  test_assert(meshCommand.preparedScalarSafe && meshCommand.preparedHostNoop);
  test_assert(gridCommand.preparedScalarSafe && gridCommand.preparedHostNoop);
  Brush brush;
  CommandCtxBase context;
  for (float value : {std::nextafter(0.1f, 1.0f), 0.5f, std::nextafter(0.9f, 0.0f)}) {
    brush.setNamedFloat(kExtraSlot_prelude_gain, value);
    meshCommand.execHost(context, brush);
    gridCommand.execHost(context, brush);
    test_assert(brush.getNamedFloat(kExtraSlot_prelude_gain) == value);
  }
  // Raw callbacks still clamp; certification does not remove authored host code.
  brush.setNamedFloat(kExtraSlot_prelude_gain, -1.0f);
  meshCommand.execHost(context, brush);
  test_assert(brush.getNamedFloat(kExtraSlot_prelude_gain) == 0.1f);
  brush.setNamedFloat(kExtraSlot_prelude_gain, 2.0f);
  gridCommand.execHost(context, brush);
  test_assert(brush.getNamedFloat(kExtraSlot_prelude_gain) == 0.9f);
  brush.setNamedFloat(kExtraSlot_prelude_gain, 0.5f);
  auto *mesh = mesh::createCube(2, 2.0f);
  {
    spatial::SpatialTree tree(mesh);
    tree.buildAll();
    meshlog::MeshLog log;
    log.setActiveMesh(mesh);
    CommandExecutor executor(&tree, &brush);
    executor.meshLog = &log;
    executor.beginStep(false);
    executor.ctx.m = mesh;
    executor.ctx.meshLog = &log;
    Vector<float3> before;
    for (int v : mesh->v)
      before.append(mesh->v.co[v]);
    auto leaves = tree.leaves();
    mesh->freezeTopo();
    executor.ctx.isFirstOfStep = true;
    executor.curCaptureSlot = 0;
    executor.curCaptureTool = int(SculptBrushes::DRAW);
    executor.exec(meshCommand, {leaves.data(), leaves.size()});
    size_t i = 0;
    for (int v : mesh->v) {
      test_assert((mesh->v.co[v] - before[i++] - float3(0, 0, 1.5f)).length() < 1e-6f);
    }
    executor.endStep();
  }
  alloc::Delete(mesh);
  fprintf(stderr,
          "compiled mesh/grid prelude factories and native reduce execution passed\n");
}

static props::Float32Prop *source(Brush &brush, const char *name)
{
  return static_cast<props::Float32Prop *>(static_cast<props::detail::PropBaseType *>(
      brush.props.struct_def->lookupLocal(name)));
}

static void meshExecution()
{
  auto *mesh = mesh::createCube(12, 2.0f);
  {
    spatial::SpatialTree tree(mesh);
    tree.leaf_limit = 32;
    tree.buildAll();
    meshlog::MeshLog log;
    log.setActiveMesh(mesh);
    Brush brush;
    brush.radius = 1.5f;
    brush.strength = 0.2f;
    brush.writeProps();
    auto *radius = source(brush, "radius");
    auto *strength = source(brush, "strength");
    radius->dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::ADD, 1);
    brush.pushDeviceInput(int(props::DeviceType::PRESSURE), 0.5f);
    int owner = 17;
    radius->owner = &owner;
    radius->flag = props::PropFlag::READ_ONLY;
    radius->dynamics.devices[0].curDeviceValue = 0.9f;
    radius->dynamics.devices[0].hasDeviceValue = false;
    brush.radius = 0.01f;
    CommandExecutor executor(&tree, &brush);
    executor.meshLog = &log;
    executor.beginStep(false);
    const float3 center(0, 0, 1), normal(0, 0, 1);
    Vector<float3> before;
    for (int v : mesh->v)
      before.append(mesh->v.co[v]);
    Vector<spatial::SpatialNode *> rawNodes;
    tree.filterNodes(center, brush.radius, rawNodes);
    Vector<int> rawVerts;
    for (auto *node : rawNodes)
      for (int v : node->data->unique_verts)
        rawVerts.append(v);
    auto inRaw = [&](int v) {
      for (int raw : rawVerts)
        if (raw == v)
          return true;
      return false;
    };
    // A first empty-region invalid dab still validates, without filtering,
    // registration, native writes, capture or first-dab advancement.
    strength->dynamics.configure(
        props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
    strength->dynamics.devices[0].curveTable.append(0.5f);
    const auto emptyMem = log.stepMemSize(log.lastStepId());
    test_assert(
        executor.applyResolvedDab(SculptBrushes::DRAW, float3(100), normal).error ==
        PropError::ERROR_INVALID_DYNAMICS);
    props::ScalarDeclaration retained;
    test_assert(!brush.props.struct_def->scalarDeclaration("radius", retained));
    test_assert(executor.isFirstOfStep && brush.strokePathCount == 0 &&
                brush.radius == 0.01f);
    test_assert(!mesh->topo_frozen && log.stepMemSize(log.lastStepId()) == emptyMem);
    test_assert(brush.namedFloats.size() == 0 && brush.namedInts.size() == 0);
    strength->dynamics.devices.clear();
    test_assert(executor.applyResolvedDab(SculptBrushes::DRAW, center, normal).error ==
                PropError::ERROR_NONE);
    test_assert(executor.lastDabNodeCount > int(rawNodes.size()));
    test_assert(!executor.isFirstOfStep && brush.radius == 2 &&
                brush.strokePathCount == 1);
    test_assert(radius->owner == &owner && *radius->internal_value() == 1.5f);
    test_assert(radius->dynamics.devices[0].curDeviceValue == 0.9f &&
                !radius->dynamics.devices[0].hasDeviceValue);
    int outsideMoved = 0;
    Vector<float3> after;
    for (int v : mesh->v) {
      after.append(mesh->v.co[v]);
      if (!inRaw(v) && mesh->v.co[v][2] > before[v][2])
        outsideMoved++;
    }
    test_assert(outsideMoved > 0);
    test_assert(log.stepMemSize(log.lastStepId()) > emptyMem);

    // Raw edits do not bump configurationGeneration; every call must inspect them.
    auto generation = brush.configurationGeneration();
    strength->dynamics.configure(
        props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
    strength->dynamics.devices[0].curveTable.append(0.5f);
    const int nodeCount = executor.lastDabNodeCount;
    const auto capturedMem = log.stepMemSize(log.lastStepId());
    test_assert(executor.applyResolvedDab(SculptBrushes::DRAW, center, normal).error ==
                PropError::ERROR_INVALID_DYNAMICS);
    test_assert(brush.configurationGeneration() == generation && brush.radius == 2);
    test_assert(executor.lastDabNodeCount == nodeCount && brush.strokePathCount == 1);
    test_assert(log.stepMemSize(log.lastStepId()) == capturedMem);
    for (int v : mesh->v)
      test_assert((mesh->v.co[v] - after[v]).lengthSqr() == 0);
    strength->dynamics.devices.clear();
    // Zero pressure can yield radius zero: publish, but do not run a kernel
    // which would divide by radius or add stroke/capture state.
    radius->dynamics.devices[0].mixMode = math::BasicMix::MULTIPLY;
    brush.pushDeviceInput(int(props::DeviceType::PRESSURE), 0);
    test_assert(executor.applyResolvedDab(SculptBrushes::DRAW, center, normal).error ==
                PropError::ERROR_NONE);
    test_assert(brush.radius == 0 && executor.lastDabNodeCount == 0 &&
                brush.strokePathCount == 1);
    test_assert(log.stepMemSize(log.lastStepId()) == capturedMem);
    brush.pushDeviceInput(int(props::DeviceType::PRESSURE), 1);
    test_assert(
        executor.applyResolvedDab(SculptBrushes::DRAW, float3(100), normal).error ==
        PropError::ERROR_NONE);
    test_assert(brush.radius == 1.5f && executor.lastDabNodeCount == 0);
    executor.endStep();
    log.undo(mesh, &tree);
    for (int v : mesh->v)
      test_assert((mesh->v.co[v] - before[v]).lengthSqr() == 0);
    log.redo(mesh, &tree);
    for (int v : mesh->v)
      test_assert((mesh->v.co[v] - after[v]).lengthSqr() == 0);
    fprintf(stderr,
            "resolved DRAW expanded region, rejection and undo passed (%d outside raw "
            "leaves)\n",
            outsideMoved);
  }
  alloc::Delete(mesh);
}

static void capabilities()
{
  auto *mesh = mesh::createCube(2, 2.0f);
  {
    spatial::SpatialTree tree(mesh);
    tree.buildAll();
    Brush brush;
    brush.radius = 4;
    brush.writeProps();
    CommandExecutor executor(&tree, &brush);
    executor.beginStep(false);
    auto rejected = [&](SculptBrushes tool = SculptBrushes::DRAW) {
      test_assert(executor.applyResolvedDab(tool, float3(0.0f), float3(0, 0, 1)).error !=
                  PropError::ERROR_NONE);
      props::ScalarDeclaration retained;
      test_assert(!brush.props.struct_def->scalarDeclaration("radius", retained));
      test_assert(brush.namedFloats.size() == 0 && brush.namedInts.size() == 0 &&
                  brush.namedBools.size() == 0);
      test_assert(!mesh->topo_frozen && brush.strokePathCount == 0 &&
                  executor.isFirstOfStep);
    };
    brush.falloff_shape = FalloffShape(-1);
    rejected();
    brush.falloff_shape = FalloffShape::Box;
    brush.falloff_extent[0] = 0;
    rejected();
    brush.falloff_extent[0] = 1;
    brush.falloff_shape = FalloffShape::Spherical;
    brush.falloff_kind = FalloffKind::Curve;
    brush.falloff_curve[0] = NAN;
    rejected();
    brush.falloff_curve[0] = 0;
    brush.falloff_kind = FalloffKind::Smoothstep;
    executor.nonAccum = true;
    test_assert(executor.supportsResolved(SculptBrushes::DRAW));
    executor.nonAccum = false;
    executor.stepHasDyntopo = true;
    test_assert(executor.supportsResolved(SculptBrushes::DRAW));
    executor.stepHasDyntopo = false;
    brush.automask_cavity = true;
    test_assert(executor.supportsResolved(SculptBrushes::DRAW));
    brush.automask_cavity = false;
    test_assert(executor.supportsResolved(SculptBrushes::GRAB));
    test_assert(executor.supportsResolved(SculptBrushes::KELVINLET));
    test_assert(executor.supportsResolved(SculptBrushes::ENHANCE));
    test_assert(executor.supportsResolved(SculptBrushes::COLOR));
    rejected(SculptBrushes(999999));
    executor.tree = nullptr;
    rejected();
    executor.tree = &tree;
    tree.m = nullptr;
    rejected();
    tree.m = mesh;
    *source(brush, "radius")->internal_value() = 1e-40f;
    const auto vertex = mesh->v.co[0];
    test_assert(
        executor.applyResolvedDab(SculptBrushes::DRAW, vertex, float3(0, 0, 1)).error ==
        PropError::ERROR_INVALID_VALUE);
    test_assert((mesh->v.co[0] - vertex).lengthSqr() == 0 && brush.radius == 4);
    rejected();
    *source(brush, "radius")->internal_value() = 4;
    test_assert(executor
                    .applyResolvedDab(SculptBrushes::DRAW,
                                      float3(std::numeric_limits<float>::quiet_NaN()),
                                      float3(0, 0, 1))
                    .error == PropError::ERROR_INVALID_VALUE);
    meshlog::MeshLog log;
    log.setActiveMesh(mesh);
    executor.meshLog = &log;
    log.beginStep(false);
    executor.beginPreviewDab(float3(0.0f), 4);
    test_assert(executor.previewActive());
    rejected();
    executor.rollbackPreviewDab();
    executor.endStep();
  }
  alloc::Delete(mesh);
}

static Vector<float3> smooth(CommandExecutor::NeighborMode mode)
{
  Vector<float3> output;
  auto *mesh = mesh::createCube(4, 2.0f);
  {
    spatial::SpatialTree tree(mesh);
    tree.leaf_limit = 32;
    tree.buildAll();
    Brush brush;
    brush.radius = 3;
    brush.strength = 0.3f;
    brush.writeProps();
    CommandExecutor executor(&tree, &brush);
    executor.neighborMode = mode;
    executor.beginStep(false);
    test_assert(
        executor.applyResolvedDab(SculptBrushes::SMOOTH, float3(0.0f), float3(0, 0, 1))
            .error == PropError::ERROR_NONE);
    for (int v : mesh->v)
      output.append(mesh->v.co[v]);
    executor.endStep();
  }
  alloc::Delete(mesh);
  return output;
}

static std::string storeBlob(subdiv::Multires &mr)
{
  std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
  mr.store.write(stream);
  return stream.str();
}

static void checkGridNormals(subdiv::GridLevelDomain &domain)
{
  std::vector<float3> normals;
  for (int v = 0; v < domain.vertCount(); v++)
    normals.push_back(domain.no[v]);
  domain.refreshAllNormals();
  for (int v = 0; v < domain.vertCount(); v++)
    test_assert((domain.no[v] - normals[v]).length() < 1e-6f);
}

static std::string gridState(GridBrushExecutor &ex)
{
  std::string state;
  auto append = [&](const auto &value) {
    state.append(reinterpret_cast<const char *>(&value), sizeof(value));
  };
  for (int v = 0; v < ex.domain->vertCount(); v++) {
    append(ex.domain->pos()[v]);
    append(ex.domain->no[v]);
    append(ex.domain->mask[v]);
  }
  for (const auto &leaf : ex.tree->leaves) {
    append(leaf.aabb.min);
    append(leaf.aabb.max);
  }
  for (const auto *set :
       std::initializer_list<const Vector<int> *>{&ex.lastDabMoved(),
                                                  &ex.lastDabGrids(),
                                                  &ex.strokeTouchedVerts(),
                                                  &ex.strokeTouchedLeaves(),
                                                  &ex.strokeTouchedGrids()})
  {
    append(set->size());
    for (int v : *set)
      append(v);
  }
  append(ex.stats);
  append(ex.isFirstOfStep);
  append(ex.brush->radius);
  append(ex.brush->strength);
  append(ex.brush->strokePathCount);
  append(ex.brush->strokeDir);
  if (ex.log) {
    append(ex.log->bytes());
    append(ex.log->stepCount());
    append(ex.log->openStepSerial());
  }
  append(ex.domain->multires()->downPropDebt(ex.domain->level()));
  state += storeBlob(*ex.domain->multires());
  return state;
}

static void gridExecution()
{
  auto *cage = mesh::createCube(4, 2.0f);
  {
    subdiv::Multires mr;
    mr.init(*cage, 3);
    auto *domain = mr.gridDomain(2);
    auto *tree = domain->ensureTree(50);
    mr.gridDomain(3);
    subdiv::GridStrokeLog log;
    Brush brush;
    brush.radius = 1.5f;
    brush.strength = 0.2f;
    brush.writeProps();
    auto *radius = source(brush, "radius");
    auto *strength = source(brush, "strength");
    radius->dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::ADD, 1);
    brush.pushDeviceInput(int(props::DeviceType::PRESSURE), 0.5f);
    int owner = 17;
    radius->owner = &owner;
    radius->flag = props::PropFlag::READ_ONLY;
    radius->dynamics.devices[0].curDeviceValue = 0.9f;
    radius->dynamics.devices[0].hasDeviceValue = false;
    brush.radius = 0.01f;
    GridBrushExecutor ex(domain, &brush, &log);
    ex.deferNormals = true;
    ex.beginStep();
    const float3 center(0, 0, 1), normal(0, 0, 1);
    std::vector<float3> before, beforeNormals;
    for (int v = 0; v < domain->vertCount(); v++) {
      before.push_back(domain->pos()[v]);
      beforeNormals.push_back(domain->no[v]);
    }
    const auto beforeBlob = storeBlob(mr);
    const bool beforeDebt = mr.downPropDebt(2);
    Vector<int> rawLeaves;
    tree->query(center, brush.radius, rawLeaves);
    auto inRaw = [&](int v) {
      for (int leaf : rawLeaves)
        if (tree->leafOfVert[v] == leaf)
          return true;
      return false;
    };
    auto invalidStack = [&]() {
      strength->dynamics.configure(
          props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
      strength->dynamics.devices[0].curveTable.append(0.5f);
    };
    invalidStack();
    auto state = gridState(ex);
    test_assert(ex.applyResolvedDab(SculptBrushes::DRAW, float3(100), normal).error ==
                PropError::ERROR_INVALID_DYNAMICS);
    test_assert(gridState(ex) == state);
    props::ScalarDeclaration retained;
    test_assert(!brush.props.struct_def->scalarDeclaration("radius", retained));
    test_assert(brush.namedFloats.size() == 0 && brush.namedInts.size() == 0);
    strength->dynamics.devices.clear();
    test_assert(ex.applyResolvedDab(SculptBrushes::DRAW, center, normal).error ==
                PropError::ERROR_NONE);
    test_assert(brush.radius == 2 && brush.strokePathCount == 1 && !ex.isFirstOfStep);
    test_assert(radius->owner == &owner && *radius->internal_value() == 1.5f);
    test_assert(radius->dynamics.devices[0].curDeviceValue == 0.9f &&
                !radius->dynamics.devices[0].hasDeviceValue);
    int outsideMoved = 0;
    std::vector<float3> after;
    for (int v = 0; v < domain->vertCount(); v++) {
      after.push_back(domain->pos()[v]);
      if (!inRaw(v) && domain->pos()[v][2] > before[v][2])
        outsideMoved++;
      test_assert((domain->no[v] - beforeNormals[v]).lengthSqr() == 0);
    }
    test_assert(outsideMoved > 0 && log.bytes() > 0);
    invalidStack();
    auto generation = brush.configurationGeneration();
    state = gridState(ex);
    test_assert(ex.applyResolvedDab(SculptBrushes::DRAW, center, normal).error ==
                PropError::ERROR_INVALID_DYNAMICS);
    test_assert(gridState(ex) == state && brush.configurationGeneration() == generation);
    strength->dynamics.devices.clear();
    const auto touched = ex.strokeTouchedVerts().size();
    const auto captured = log.bytes();
    radius->dynamics.devices[0].mixMode = math::BasicMix::MULTIPLY;
    brush.pushDeviceInput(int(props::DeviceType::PRESSURE), 0);
    test_assert(ex.applyResolvedDab(SculptBrushes::DRAW, center, normal).error ==
                PropError::ERROR_NONE);
    test_assert(brush.radius == 0 && ex.lastDabMoved().size() == 0 &&
                ex.lastDabGrids().size() == 0);
    brush.pushDeviceInput(int(props::DeviceType::PRESSURE), 1);
    test_assert(ex.applyResolvedDab(SculptBrushes::DRAW, float3(100), normal).error ==
                PropError::ERROR_NONE);
    test_assert(ex.strokeTouchedVerts().size() == touched && log.bytes() == captured &&
                brush.strokePathCount == 1 && ex.stats.dabs == 3);
    test_assert(ex.flushNormals().size() > 0);
    std::vector<float3> flushed;
    for (int v = 0; v < domain->vertCount(); v++)
      flushed.push_back(domain->no[v]);
    domain->refreshAllNormals();
    for (int v = 0; v < domain->vertCount(); v++)
      test_assert((domain->no[v] - flushed[v]).length() < 1e-6f);
    ex.endStep();
    const auto afterBlob = storeBlob(mr);
    const bool afterDebt = mr.downPropDebt(2);
    test_assert(!mr.hasGridDomain(3) && log.stepCount() == 1 && beforeBlob != afterBlob);
    // Ending the first stroke dropped a finer domain, but retains this attachment.
    ex.beginStep();
    test_assert(ex.applyResolvedDab(SculptBrushes::DRAW, center, normal).error ==
                PropError::ERROR_NONE);
    ex.endStep();
    test_assert(log.stepCount() == 2 && log.undo());
    for (int v = 0; v < domain->vertCount(); v++)
      test_assert(std::memcmp(&domain->pos()[v], &after[v], sizeof(float3)) == 0);
    checkGridNormals(*domain);
    test_assert(storeBlob(mr) == afterBlob && mr.downPropDebt(2) == afterDebt);
    test_assert(log.undo());
    for (int v = 0; v < domain->vertCount(); v++)
      test_assert(std::memcmp(&domain->pos()[v], &before[v], sizeof(float3)) == 0);
    checkGridNormals(*domain);
    test_assert(storeBlob(mr) == beforeBlob && mr.downPropDebt(2) == beforeDebt);
    test_assert(log.redo());
    for (int v = 0; v < domain->vertCount(); v++)
      test_assert(std::memcmp(&domain->pos()[v], &after[v], sizeof(float3)) == 0);
    checkGridNormals(*domain);
    test_assert(storeBlob(mr) == afterBlob && mr.downPropDebt(2) == afterDebt);
    fprintf(stderr,
            "resolved grid DRAW expanded region and exact undo passed (%d outside raw "
            "leaves)\n",
            outsideMoved);
  }
  alloc::Delete(cage);
}

static void gridCapabilities()
{
  auto *cage = mesh::createCube(2, 2.0f);
  {
    subdiv::Multires mr;
    mr.init(*cage, 2);
    auto *domain = mr.gridDomain(2);
    Brush brush;
    brush.radius = 4;
    brush.writeProps();
    subdiv::GridStrokeLog log, other;
    GridBrushExecutor ex(domain, &brush, &log);
    auto run = [&](SculptBrushes tool = SculptBrushes::DRAW) {
      return ex.applyResolvedDab(tool, float3(0.0f), float3(0, 0, 1));
    };
    auto rejected = [&](SculptBrushes tool = SculptBrushes::DRAW) {
      const auto state = gridState(ex);
      test_assert(run(tool).error != PropError::ERROR_NONE);
      test_assert(gridState(ex) == state);
      props::ScalarDeclaration retained;
      test_assert(!brush.props.struct_def->scalarDeclaration("radius", retained));
      test_assert(brush.namedFloats.size() == 0 && brush.namedInts.size() == 0 &&
                  brush.namedBools.size() == 0 && brush.strokePathCount == 0);
    };
    rejected();
    ex.beginStep();
    brush.falloff_shape = FalloffShape(-1);
    rejected();
    brush.falloff_shape = FalloffShape::Box;
    brush.falloff_extent[0] = 0;
    rejected();
    brush.falloff_extent[0] = 1;
    brush.falloff_shape = FalloffShape::Spherical;
    brush.falloff_kind = FalloffKind::Curve;
    brush.falloff_curve[0] = NAN;
    rejected();
    brush.falloff_curve[0] = 0;
    brush.falloff_kind = FalloffKind::Smoothstep;
    ex.nonAccum = true;
    test_assert(ex.supportsResolved(SculptBrushes::DRAW));
    ex.nonAccum = false;
    brush.automask_cavity = true;
    test_assert(ex.supportsResolved(SculptBrushes::DRAW));
    brush.automask_cavity = false;
    for (auto tool : std::initializer_list<SculptBrushes>{SculptBrushes::ENHANCE,
                                                          SculptBrushes::COLOR,
                                                          SculptBrushes::POLYGROUP,
                                                          SculptBrushes::FEATURE_ALIGN,
                                                          SculptBrushes(999999)})
      rejected(tool);
    auto *tree = ex.tree;
    ex.tree = nullptr;
    test_assert(run().error == PropError::ERROR_INVALID_OWNER);
    ex.tree = tree;
    ex.domain = nullptr;
    test_assert(run().error == PropError::ERROR_INVALID_OWNER);
    ex.domain = domain;
    other.attach(domain);
    other.beginStep();
    ex.log = &other;
    rejected();
    ex.log = nullptr;
    rejected();
    ex.log = &log;
    log.endStep(false);
    rejected();
    log.beginStep();
    rejected();
    log.attach(domain);
    rejected();
    ex.attach(domain);
    ex.endStep();
    test_assert(ex.stats.strokes == 0 && log.stepCount() == 0);
    ex.log = nullptr;
    ex.beginStep();
    ex.log = &other;
    rejected();
    ex.log = nullptr;
    *source(brush, "radius")->internal_value() = 1e-40f;
    rejected();
    *source(brush, "radius")->internal_value() = 4;
    const auto state = gridState(ex);
    test_assert(ex.applyResolvedDab(SculptBrushes::DRAW,
                                    float3(std::numeric_limits<float>::quiet_NaN()),
                                    float3(0, 0, 1))
                    .error == PropError::ERROR_INVALID_VALUE);
    test_assert(gridState(ex) == state);
    ex.endStep();
    // Check generation before touching the freed domain or its freed tree.
    ex.beginStep();
    mr.invalidateAll();
    test_assert(run().error == PropError::ERROR_INVALID_OWNER);
    domain = mr.gridDomain(2);
    test_assert(run().error == PropError::ERROR_INVALID_OWNER);
    ex.attach(domain);
    rejected();
    ex.beginStep();
    test_assert(run().error == PropError::ERROR_NONE);
    ex.endStep();
    mr.gridAttrs().declareHostAttr("color", mesh::AttrType::FLOAT4);
    ex.beginStep();
    ex.applyDab(SculptBrushes::COLOR, float3(0.0f), float3(0, 0, 1));
    ex.endStep();
    ex.beginStep();
    const auto mirrorState = gridState(ex);
    test_assert(run().error == PropError::ERROR_INVALID_VALUE);
    test_assert(gridState(ex) == mirrorState);
    ex.endStep();
    int layer = mr.layerAdd();
    test_assert(mr.setEditTarget(layer) == layer);
    ex.attach(mr.gridDomain(2));
    ex.beginStep();
    test_assert(ex.supportsResolved(SculptBrushes::DRAW));
    test_assert(ex.supportsResolved(SculptBrushes::LAYERDRAW));
    test_assert(run().error == PropError::ERROR_NONE);
    ex.endStep();
    ex.beginStep();
    cage->activeEditLayer = -1;
    const auto layerState = gridState(ex);
    test_assert(run().error == PropError::ERROR_INVALID_OWNER);
    test_assert(gridState(ex) == layerState);
    cage->activeEditLayer = layer;
    ex.endStep();
  }
  alloc::Delete(cage);
  fprintf(stderr, "resolved grid capabilities and lifecycle rejection passed\n");
}

static std::vector<float3>
gridSmooth(bool resolved, bool deferred, SculptBrushes tool = SculptBrushes::SMOOTH)
{
  std::vector<float3> output;
  auto *cage = mesh::createCube(4, 2.0f);
  {
    subdiv::Multires mr;
    mr.init(*cage, 2);
    auto *domain = mr.gridDomain(2);
    domain->ensureTree(50);
    Brush brush;
    brush.radius = resolved ? 1.5f : 2.0f;
    brush.strength = 0.3f;
    brush.writeProps();
    if (resolved) {
      source(brush, "radius")
          ->dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::ADD, 1);
      brush.pushDeviceInput(int(props::DeviceType::PRESSURE), 0.5f);
      brush.radius = 0.01f;
    }
    GridBrushExecutor ex(domain, &brush);
    ex.deferNormals = deferred;
    ex.beginStep();
    for (int dab = 0; dab < 3; dab++) {
      const float3 center(0.1f * dab, 0, 1), normal(0, 0, 1);
      if (resolved)
        test_assert(ex.applyResolvedDab(tool, center, normal).error ==
                    PropError::ERROR_NONE);
      else
        ex.applyDab(tool, center, normal);
    }
    ex.endStep();
    for (int v = 0; v < domain->vertCount(); v++) {
      output.push_back(domain->pos()[v]);
      output.push_back(domain->no[v]);
    }
  }
  alloc::Delete(cage);
  return output;
}

static void commandStackConfiguration()
{
  BrushProgram program;
  program.addCommand(int(SculptBrushes::DRAW));
  Vector<int> devices, modes, enabled, offsets, kinds;
  Vector<float> factors, samples;
  Vector<double> parameters;
  offsets.append(0);
  auto replace = [&](int index = 0) {
    return program.replaceCommandResponseDynamicsChecked(index,
                                                         "radius",
                                                         int(props::Prop::FLOAT32),
                                                         devices,
                                                         modes,
                                                         factors,
                                                         enabled,
                                                         offsets,
                                                         samples,
                                                         kinds,
                                                         parameters);
  };
  test_assert(replace() == 0);
  test_assert(program.commands[0].dynamicsOverrides.size() == 1);
  test_assert(program.commands[0].dynamicsOverrides[0].dynamics.devices.size() == 0);
  test_assert(replace(-1) != 0);
  devices.append(0);
  test_assert(replace() != 0);
  test_assert(program.commands[0].dynamicsOverrides[0].dynamics.devices.size() == 0);
  modes.append(1);
  factors.append(1);
  enabled.append(1);
  offsets.append(0);
  kinds.append(2);
  const double threshold = 0.5000000000000001;
  parameters.append(threshold);
  parameters.append(0.25);
  parameters.append(0.75);
  test_assert(replace() == 0);
  test_assert(parameters[0] == threshold && offsets.size() == 2);
  const auto &layer = program.commands[0].dynamicsOverrides[0].dynamics.devices[0];
  test_assert(layer.responseThreshold == threshold && !layer.hasDeviceValue);
  test_assert(
      program.removeCommandDynamicsChecked(0, "radius", int(props::Prop::INT32)) != 0);
  test_assert(program.commands[0].dynamicsOverrides.size() == 1);
  test_assert(
      program.removeCommandDynamicsChecked(0, "radius", int(props::Prop::FLOAT32)) == 0);
  test_assert(program.commands[0].dynamicsOverrides.size() == 0);
}

static BrushProgram drawSmoothProgram(bool resolved,
                                      SculptBrushes smoothTool = SculptBrushes::SMOOTH)
{
  BrushProgram program;
  int draw = program.addCommand(int(SculptBrushes::DRAW));
  int smooth = program.addCommand(int(smoothTool));
  program.setCommandFloat(draw, int(BrushProp::Radius), resolved ? 0.3f : 0.8f);
  program.setCommandFloat(draw, int(BrushProp::Strength), 0.2f);
  program.setCommandFloat(smooth, int(BrushProp::Radius), resolved ? 1.5f : 2.0f);
  program.setCommandFloat(smooth, int(BrushProp::Strength), 0.3f);
  if (resolved) {
    BrushDynamicsOverride main{"radius", props::Prop::FLOAT32, {}};
    main.dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::ADD, 1);
    program.commands[draw].dynamicsOverrides.append(main);
    BrushDynamicsOverride later{"radius", props::Prop::FLOAT32, {}};
    later.dynamics.configure(props::DeviceType::TILTX, math::BasicMix::ADD, 1);
    program.commands[smooth].dynamicsOverrides.append(later);
    program.commands[smooth].dynamicsOverrides.append(
        {"strength", props::Prop::FLOAT32, {}});
  }
  return program;
}

static std::vector<float3>
programGeometry(bool grid,
                bool resolved,
                CommandExecutor::NeighborMode mode,
                bool deferred,
                SculptBrushes smoothTool = SculptBrushes::SMOOTH,
                bool nonAccum = false)
{
  std::vector<float3> output;
  auto *cage = mesh::createCube(grid ? 4 : 12, 2.0f);
  if (!grid && smoothTool == SculptBrushes::BSMOOTH) {
    for (int edge : cage->e) {
      if (edge % 7 == 0)
        mesh::boundary::setEdgeFlag(cage, mesh::boundary::EDGE_SHARP, edge, true);
      if (edge % 11 == 0)
        mesh::boundary::setEdgeFlag(cage, mesh::boundary::EDGE_SEAM, edge, true);
    }
  }
  {
    subdiv::Multires mr;
    mr.init(*cage, 2);
    auto *domain = mr.gridDomain(2);
    auto *gridTree = domain->ensureTree(50);
    spatial::SpatialTree tree(cage);
    tree.leaf_limit = 32;
    tree.buildAll();
    meshlog::MeshLog meshLog;
    meshLog.setActiveMesh(cage);
    subdiv::GridStrokeLog gridLog;
    Brush brush;
    brush.radius = 1.2f;
    brush.strength = 0.2f;
    brush.writeProps();
    auto *radius = source(brush, "radius");
    int owner = 123;
    if (resolved) {
      source(brush, "strength")
          ->dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
      radius->dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::ADD, 1);
      brush.pushDeviceInput(int(props::DeviceType::PRESSURE), 0.5f);
      radius->owner = &owner;
      radius->flag = props::PropFlag::READ_ONLY;
      radius->dynamics.devices[0].curDeviceValue = 0.83f;
      radius->dynamics.devices[0].hasDeviceValue = false;
    }
    brush.radius = 0.01f;
    brush.strength = -0.77f;
    CommandExecutor meshEx(&tree, &brush);
    meshEx.meshLog = &meshLog;
    meshEx.neighborMode = mode;
    meshEx.nonAccum = nonAccum;
    if (nonAccum)
      meshEx.setStrokeGen(1);
    GridBrushExecutor gridEx(domain, &brush, &gridLog);
    gridEx.deferNormals = deferred;
    gridEx.nonAccum = nonAccum;
    BrushProgram program = drawSmoothProgram(resolved, smoothTool);
    if (nonAccum)
      program.commands[1].floatOverrides[0].value = 0.1f;
    const int channelCount = mr.store.channelCount();
    brush.pushDeviceInput(int(props::DeviceType::TILTX), 0.5f);
    std::vector<float3> before;
    int count = grid ? domain->vertCount() : 0;
    if (!grid)
      for (int v : cage->v) {
        test_assert(v == count);
        count++;
      }
    for (int v = 0; v < count; v++)
      before.push_back(grid ? domain->pos()[v] : cage->v.co[v]);
    const auto blobBefore = storeBlob(mr);
    const float3 center(0, 0, 1), normal(0, 0, 1);
    std::vector<bool> smallOwner(size_t(count), false);
    if (grid) {
      Vector<int> leaves;
      gridTree->query(center, nonAccum ? 0.6f : 0.8f, leaves);
      for (int li : leaves)
        for (int v : gridTree->leaves[li].ownedVerts)
          smallOwner[v] = true;
      gridEx.beginStep();
    } else {
      Vector<spatial::SpatialNode *> nodes;
      tree.filterNodes(center, nonAccum ? 0.6f : 0.8f, nodes);
      for (auto *node : nodes)
        for (int v : node->data->unique_verts)
          smallOwner[v] = true;
      meshEx.beginStep(false);
    }
    for (int dab = 0; dab < 3; dab++) {
      const float pressure = 0.25f + 0.125f * dab;
      const float tilt = 0.5f + 0.25f * dab;
      if (smoothTool == SculptBrushes::BSMOOTH) {
        brush.projection = 0.25f * dab;
      }
      if (resolved) {
        brush.pushDeviceInput(int(props::DeviceType::PRESSURE), pressure);
        brush.pushDeviceInput(int(props::DeviceType::TILTX), tilt);
        auto result = grid ? gridEx.applyResolvedProgram(&program, center, normal)
                           : meshEx.applyResolvedProgram(&program, center, normal);
        test_assert(result.error == PropError::ERROR_NONE);
        if (grid) {
          test_assert(mr.store.channelCount() == channelCount);
          test_assert(gridEx.supportsResolved(smoothTool)); // Mirrors would reject.
        }
        test_assert(brush.radius == 0.01f && brush.strength == -0.77f);
        test_assert(*radius->internal_value() == 1.2f && radius->owner == &owner);
        test_assert(radius->dynamics.devices[0].curDeviceValue == 0.83f &&
                    !radius->dynamics.devices[0].hasDeviceValue);
        props::ScalarDeclaration declaration;
        test_assert(!brush.props.struct_def->scalarDeclaration("radius", declaration));
        test_assert(brush.namedFloats.size() == 0 && brush.namedInts.size() == 0);
      } else if (grid) {
        program.commands[0].floatOverrides[0].value = 0.3f + pressure;
        program.commands[0].floatOverrides[1].value = 0.2f * pressure;
        program.commands[1].floatOverrides[0].value = (nonAccum ? 0.1f : 1.5f) + tilt;
        brush.radius = 3.0f;
        gridEx.applyProgram(&program, center, normal);
      } else {
        program.commands[0].floatOverrides[0].value = 0.3f + pressure;
        program.commands[0].floatOverrides[1].value = 0.2f * pressure;
        program.commands[1].floatOverrides[0].value = (nonAccum ? 0.1f : 1.5f) + tilt;
        meshEx.applyDab(&program, center, normal, 3.0f, nullptr, 0);
        tree.updateQueries();
      }
    }
    if (grid)
      gridEx.endStep();
    else
      meshEx.endStep();
    int outside = 0;
    for (int v = 0; v < count; v++) {
      auto co = grid ? domain->pos()[v] : cage->v.co[v];
      output.push_back(co);
      if (!smallOwner[v] && (co - before[v]).lengthSqr() > 0)
        outside++;
    }
    test_assert(outside > 0);
    if (nonAccum && resolved && !grid) {
      Vector<spatial::SpatialNode *> nodes;
      tree.filterNodes(center, 1.1f, nodes);
      int newLeaves = 0;
      for (auto *node : nodes) {
        bool fresh = false;
        for (int v : node->data->unique_verts)
          fresh |= !smallOwner[v];
        if (fresh) {
          newLeaves++;
          test_assert(node->captureStamps[0].tool == int(SculptBrushes::DRAW));
          test_assert(node->captureStamps[1].tool == int(smoothTool));
          test_assert(node->captureStamps[0].sid == node->captureStamps[1].sid);
        }
      }
      test_assert(newLeaves > 0);
    }
    if (grid)
      for (int v = 0; v < count; v++)
        output.push_back(domain->no[v]);
    if (resolved) {
      test_assert(brush.strokePathCount == 3);
      const auto blobAfter = storeBlob(mr);
      if (grid)
        test_assert(gridLog.undo());
      else
        meshLog.undo(cage, &tree);
      for (int v = 0; v < count; v++) {
        const auto &co = grid ? domain->pos()[v] : cage->v.co[v];
        test_assert(std::memcmp(&co, &before[v], sizeof(float3)) == 0);
      }
      if (grid) {
        test_assert(storeBlob(mr) == blobBefore);
        checkGridNormals(*domain);
        test_assert(gridLog.redo());
        test_assert(storeBlob(mr) == blobAfter);
        checkGridNormals(*domain);
      } else
        meshLog.redo(cage, &tree);
      for (int v = 0; v < count; v++) {
        const auto &co = grid ? domain->pos()[v] : cage->v.co[v];
        test_assert(std::memcmp(&co, &output[v], sizeof(float3)) == 0);
      }
      test_assert(mr.store.channelCount() == channelCount);
    }
  }
  alloc::Delete(cage);
  return output;
}

static void programRejection(bool grid)
{
  auto *cage = mesh::createCube(4, 2.0f);
  {
    subdiv::Multires mr;
    mr.init(*cage, 2);
    auto *domain = mr.gridDomain(2);
    spatial::SpatialTree tree(cage);
    tree.leaf_limit = 32;
    tree.buildAll();
    meshlog::MeshLog meshLog;
    meshLog.setActiveMesh(cage);
    subdiv::GridStrokeLog gridLog;
    Brush brush;
    brush.radius = 2;
    brush.strength = 0.2f;
    brush.writeProps();
    brush.radius = 0.01f;
    CommandExecutor meshEx(&tree, &brush);
    meshEx.meshLog = &meshLog;
    GridBrushExecutor gridEx(domain, &brush, &gridLog);
    gridEx.deferNormals = true;
    if (grid)
      gridEx.beginStep();
    else
      meshEx.beginStep(false);
    BrushProgram program;
    program.addCommand(int(SculptBrushes::DRAW));
    program.addCommand(int(SculptBrushes::DRAW));
    auto run = [&](float3 center = float3(0, 0, 1)) {
      return grid ? gridEx.applyResolvedProgram(&program, center, float3(0, 0, 1))
                  : meshEx.applyResolvedProgram(&program, center, float3(0, 0, 1));
    };
    auto state = [&]() {
      if (grid)
        return gridState(gridEx);
      std::string result;
      auto append = [&](const auto &value) {
        result.append(reinterpret_cast<const char *>(&value), sizeof(value));
      };
      for (int v : cage->v) {
        append(cage->v.co[v]);
        append(cage->v.no[v]);
      }
      append(cage->topo_frozen);
      append(meshLog.stepMemSize(meshLog.lastStepId()));
      append(meshEx.isFirstOfStep);
      append(meshEx.lastDabNodeCount);
      append(brush.radius);
      append(brush.strength);
      append(brush.strokePathCount);
      return result;
    };
    auto rejects = [&](float3 center = float3(0, 0, 1)) {
      const auto prior = state();
      auto result = run(center);
      test_assert(result.error != PropError::ERROR_NONE);
      test_assert(state() == prior);
      test_assert(std::strstr(result.name.c_str(), "command[") != nullptr);
    };
    program.commands[1].scalarOverrides.append(
        {"radius", props::Prop::FLOAT32, std::numeric_limits<double>::quiet_NaN()});
    rejects(float3(100));
    test_assert(grid ? gridEx.isFirstOfStep : meshEx.isFirstOfStep);
    test_assert(!cage->topo_frozen && brush.strokePathCount == 0);
    program.commands[1].scalarOverrides.clear();
    test_assert(run().error == PropError::ERROR_NONE);
    test_assert(brush.radius == 0.01f && brush.strokePathCount == 1);
    program.commands[1].floatOverrides.append({99999, 1.0f});
    rejects();
    program.commands[1].floatOverrides.clear();
    program.setCommandFloat(1, int(BrushProp::Radius), 2);
    test_assert(program.setCommandScalarChecked(1, "radius", props::Prop::FLOAT32, 2) ==
                PropError::ERROR_NONE);
    rejects();
    program.commands[1].floatOverrides.clear();
    program.commands[1].scalarOverrides[0].type = props::Prop::INT32;
    rejects();
    program.commands[1].scalarOverrides.clear();
    program.commands[1].type = SculptBrushes(-1);
    rejects();
    program.commands[1].type = SculptBrushes::DRAW;
    program.commands[1].dynamicsOverrides.append({"missing", props::Prop::FLOAT32, {}});
    rejects();
    program.commands[1].dynamicsOverrides[0].name = "radius";
    program.commands[1].dynamicsOverrides[0].type = props::Prop::INT32;
    rejects();
    program.commands[1].dynamicsOverrides[0].type = props::Prop::FLOAT32;
    program.commands[1].dynamicsOverrides.append({"radius", props::Prop::FLOAT32, {}});
    rejects();
    program.commands[1].dynamicsOverrides.clear();
    BrushDynamicsOverride pending{"strength", props::Prop::FLOAT32, {}};
    pending.dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
    pending.dynamics.devices[0].setCurveSample(0, 3, 0.5f);
    pending.dynamics.devices[0].flag = props::DynamicFlags::DISABLED;
    program.commands[1].dynamicsOverrides.append(pending);
    rejects();
    program.commands[1].dynamicsOverrides.clear();
    program.commands[0].dynamicsOverrides.append({"strength", props::Prop::FLOAT32, {}});
    program.commands[1].dynamicsOverrides.append({"strength", props::Prop::FLOAT32, {}});
    auto *strength = source(brush, "strength");
    strength->dynamics.configure(
        props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
    strength->dynamics.devices[0].curveTable.append(0.5f);
    rejects();
    strength->dynamics.devices.clear();
    test_assert(run().error == PropError::ERROR_NONE);
    // Skipped stages retain original indices; only the middle stage captures.
    if (grid) {
      gridEx.endStep();
      gridEx.beginStep();
    } else {
      meshEx.endStep();
      meshEx.beginStep(false);
    }
    program.addCommand(int(SculptBrushes::DRAW));
    test_assert(program.setCommandScalarChecked(0, "radius", props::Prop::FLOAT32, 0) ==
                PropError::ERROR_NONE);
    test_assert(program.setCommandScalarChecked(2, "radius", props::Prop::FLOAT32, 0) ==
                PropError::ERROR_NONE);
    test_assert(run().error == PropError::ERROR_NONE && brush.strokePathCount == 1);
    if (!grid) {
      Vector<spatial::SpatialNode *> nodes;
      tree.filterNodes(float3(0, 0, 1), 2, nodes);
      test_assert(nodes.size() > 0);
      for (auto *node : nodes) {
        const auto sid = meshLog.curStrokeId() + 1;
        test_assert(node->captureStamps[1].sid == sid);
        test_assert(node->captureStamps[0].sid != sid &&
                    node->captureStamps[2].sid != sid);
      }
    }
    const auto captures =
        grid ? gridLog.bytes() : meshLog.stepMemSize(meshLog.lastStepId());
    const auto touched = gridEx.strokeTouchedVerts().size();
    program.clear();
    test_assert(run().error == PropError::ERROR_NONE && brush.strokePathCount == 1);
    test_assert(grid ? gridEx.lastDabMoved().size() == 0 : meshEx.lastDabNodeCount == 0);
    program.addCommand(int(SculptBrushes::DRAW));
    test_assert(program.setCommandScalarChecked(0, "radius", props::Prop::FLOAT32, 0) ==
                PropError::ERROR_NONE);
    test_assert(run().error == PropError::ERROR_NONE && brush.strokePathCount == 1);
    test_assert((grid ? gridLog.bytes() : meshLog.stepMemSize(meshLog.lastStepId())) ==
                captures);
    if (grid) {
      test_assert(gridEx.strokeTouchedVerts().size() == touched);
      test_assert(gridEx.flushNormals().size() > 0);
      checkGridNormals(*domain);
    }
    program.commands[0].scalarOverrides.clear();
    program.commands[0].dynamicsOverrides.append({"radius", props::Prop::FLOAT32, {}});
    const auto previous = state();
    test_assert(!(grid
                      ? gridEx.preflightRawProgram(&program, float3(100), float3(0, 0, 1))
                      : meshEx.preflightRawProgram(&program)));
    test_assert(
        (grid ? gridEx.applyProgram(&program, float3(100), float3(0, 0, 1))
              : meshEx.applyDab(&program, float3(100), float3(0, 0, 1), 2, nullptr, 0)) ==
        -1);
    test_assert(!(grid ? gridEx.prepareProgramDeclarations(&program)
                       : meshEx.prepareProgramDeclarations(&program)));
    test_assert(state() == previous);
    if (grid)
      gridEx.endStep();
    else
      meshEx.endStep();
  }
  alloc::Delete(cage);
}

static void rawRevalidation()
{
  auto *cage = mesh::createCube(2, 2.0f);
  {
    subdiv::Multires mr;
    mr.init(*cage, 1);
    auto *domain = mr.gridDomain(1);
    spatial::SpatialTree tree(cage);
    tree.buildAll();
    for (bool grid : {false, true}) {
      Brush brush;
      brush.radius = 4;
      brush.writeProps();
      CommandExecutor meshEx(&tree, &brush);
      GridBrushExecutor gridEx(domain, &brush);
      if (grid)
        gridEx.beginStep();
      else
        meshEx.beginStep(false);
      auto call = [&](bool program) {
        BrushProgram entries;
        entries.addCommand(int(SculptBrushes::DRAW));
        if (grid)
          return program
                     ? gridEx.applyProgram(&entries, float3(100), float3(0, 0, 1))
                     : gridEx.applyDab(SculptBrushes::DRAW, float3(100), float3(0, 0, 1));
        return program ? meshEx.applyDab(
                             &entries, float3(100), float3(0, 0, 1), 4, nullptr, 0)
                       : meshEx.applyDab(SculptBrushes::DRAW,
                                         float3(100),
                                         float3(0, 0, 1),
                                         4,
                                         nullptr,
                                         0);
      };
      test_assert(call(false) == 0);
      auto *strength = source(brush, "strength");
      strength->dynamics.configure(
          props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
      strength->dynamics.devices[0].curveTable.append(.5f);
      int samples = brush.strokePathCount;
      for (bool program : {false, true}) {
        test_assert(call(program) == -1);
        test_assert(brush.strokePathCount == samples);
      }
      strength->dynamics.devices.clear();
      test_assert(call(false) == 0 && call(true) == 0);
      auto &stray = brush.props.struct_def->Float64("stray", "Stray", -1);
      stray.dynamics.configure(props::DeviceType::PRESSURE, math::BasicMix::MULTIPLY, 1);
      test_assert(call(false) == -1 && call(true) == -1);
      stray.dynamics.devices.clear();
      test_assert(call(false) == 0 && call(true) == 0);
      if (grid)
        gridEx.endStep();
      else
        meshEx.endStep();
    }
  }
  alloc::Delete(cage);
  fprintf(stderr, "raw mesh/grid nonfirst empty-region validation and repair passed\n");
}

static void boundaryCapability()
{
  BrushAttrManifestEntry entry;
  entry.boundName = ".boundary.vert.class";
  entry.type = mesh::AttrType::INT;
  auto accepted = [&]() { return resolvedAttributesSupported({&entry, 1}); };
  test_assert(accepted());
  entry.type = mesh::AttrType::FLOAT;
  test_assert(!accepted());
  entry.type = mesh::AttrType::INT;
  for (auto domain : {AttrElemDomain::Face, AttrElemDomain::Edge, AttrElemDomain::Corner})
  {
    entry.domain = domain;
    test_assert(!accepted());
  }
  entry.domain = AttrElemDomain::Vertex;
  entry.kernelWrites = true;
  test_assert(!accepted());
  entry.kernelWrites = false;
  entry.use = 1;
  test_assert(!accepted());
  entry.use = 0;
  entry.boundName = "";
  entry.handle = ".boundary.vert.class";
  test_assert(!accepted());
  for (bool grid : {false, true}) {
    test_assert(preparedHooksSupported(SculptBrushes::BSMOOTH, grid));
    test_assert(preparedHooksSupported(SculptBrushes::ENHANCE, grid) == !grid);
    for (auto tool : {SculptBrushes::FEATURE_ALIGN, SculptBrushes::POLYGROUP})
      test_assert(preparedHooksSupported(tool, grid) == !grid);
  }
}

static std::vector<float3> boundaryFirstHit(bool resolved,
                                            bool programMode,
                                            bool zeroFirst,
                                            CommandExecutor::NeighborMode mode)
{
  auto *mesh = mesh::createCube(6, 2.0f);
  std::vector<float3> result;
  {
    spatial::SpatialTree tree(mesh);
    tree.leaf_limit = 32;
    tree.buildAll();
    Brush brush;
    brush.radius = 2;
    brush.strength = .3f;
    brush.projection = .4f;
    brush.writeProps();
    CommandExecutor ex(&tree, &brush);
    ex.neighborMode = mode;
    meshlog::MeshLog log;
    log.setActiveMesh(mesh);
    ex.meshLog = &log;
    ex.beginStep(false);
    mesh::boundary::setEdgeFlag(mesh, mesh::boundary::EDGE_SHARP, 0, true);
    mesh::boundary::setEdgeFlag(mesh, mesh::boundary::EDGE_SEAM, 1, true);
    mesh->freezeTopo();
    BrushProgram program;
    program.addCommand(int(SculptBrushes::BSMOOTH));
    program.addCommand(int(SculptBrushes::BSMOOTH));
    std::vector<float3> before;
    for (int v : mesh->v)
      before.push_back(mesh->v.co[v]);
    auto call = [&](float3 center) {
      if (resolved)
        return programMode
                   ? ex.applyResolvedProgram(&program, center, float3(0, 0, 1))
                   : ex.applyResolvedDab(SculptBrushes::BSMOOTH, center, float3(0, 0, 1));
      ex.refreshBoundaryClassForBSmooth(mesh);
      if (programMode)
        ex.applyDab(&program, center, float3(0, 0, 1), 2, nullptr, 0);
      else
        ex.applyDab(SculptBrushes::BSMOOTH, center, float3(0, 0, 1), 2, nullptr, 0);
      tree.updateQueries();
      return props::ScalarRegistrationResult{};
    };
    if (resolved) {
      if (zeroFirst)
        *source(brush, "radius")->internal_value() = 0;
      test_assert(call(zeroFirst ? float3(0, 0, 1) : float3(100)).error ==
                  PropError::ERROR_NONE);
      test_assert(mesh->boundaryDirty && mesh->topo_frozen && !ex.isFirstOfStep);
      test_assert(brush.strokePathCount == 0);
      *source(brush, "radius")->internal_value() = 2;
      program.commands[1].scalarOverrides.append(
          {"radius", props::Prop::FLOAT32, std::numeric_limits<double>::quiet_NaN()});
      const auto attrCount = mesh->v.attrs.attrs.size();
      const auto logBytes = log.stepMemSize(log.lastStepId());
      test_assert(
          ex.applyResolvedProgram(&program, float3(0, 0, 1), float3(0, 0, 1)).error !=
          PropError::ERROR_NONE);
      test_assert(mesh->boundaryDirty && mesh->topo_frozen &&
                  mesh->v.attrs.attrs.size() == attrCount);
      test_assert(log.stepMemSize(log.lastStepId()) == logBytes &&
                  brush.strokePathCount == 0);
      for (int v : mesh->v)
        test_assert(std::memcmp(&mesh->v.co[v], &before[v], sizeof(float3)) == 0);
      program.commands[1].scalarOverrides.clear();
    }
    test_assert(call(float3(0, 0, 1)).error == PropError::ERROR_NONE);
    test_assert(!mesh->boundaryDirty);
    test_assert((mesh::boundary::vertClass(mesh, mesh->e.vs[0][0]) &
                 mesh::boundary::BC_SHARP) != 0);
    for (int v : mesh->v)
      result.push_back(mesh->v.co[v]);
    ex.endStep();
    if (resolved) {
      log.undo(mesh, &tree);
      for (int v : mesh->v)
        test_assert(std::memcmp(&mesh->v.co[v], &before[v], sizeof(float3)) == 0);
      log.redo(mesh, &tree);
      for (int v : mesh->v)
        test_assert(std::memcmp(&mesh->v.co[v], &result[v], sizeof(float3)) == 0);
      ex.beginStep(false);
      test_assert(call(float3(0, 0, 1)).error == PropError::ERROR_NONE);
      test_assert(!mesh->boundaryDirty);
      ex.endStep();
    }
  }
  alloc::Delete(mesh);
  return result;
}

static std::vector<float3> nonAccumLifetime(bool grid, bool resolved, bool nonAccum)
{
  auto *mesh = mesh::createCube(grid ? 4 : 12, 2.0f);
  std::vector<float3> result;
  {
    subdiv::Multires mr;
    mr.init(*mesh, 2);
    auto *domain = mr.gridDomain(2);
    auto *gtree = domain->ensureTree(32);
    spatial::SpatialTree tree(mesh);
    tree.leaf_limit = 16;
    tree.buildAll();
    meshlog::MeshLog ml;
    ml.setActiveMesh(mesh);
    subdiv::GridStrokeLog gl;
    Brush brush;
    CommandExecutor ex(&tree, &brush);
    ex.meshLog = &ml;
    ex.nonAccum = nonAccum;
    GridBrushExecutor gx(domain, &brush, &gl);
    gx.nonAccum = nonAccum;
    const int count = grid ? domain->vertCount() : int(mesh->v.count);
    auto coords = [&]() {
      std::vector<float3> co;
      for (int v = 0; v < count; v++)
        co.push_back(grid ? domain->pos()[v] : mesh->v.co[v]);
      return co;
    };
    auto begin = [&]() {
      if (grid)
        gx.beginStep();
      else {
        ex.setStrokeGen(int(ex.strokeGen) + 1);
        ex.beginStep(false);
      }
    };
    auto end = [&]() {
      if (grid)
        gx.endStep();
      else
        ex.endStep();
    };
    auto &ctx = grid ? gx.ctx : ex.ctx;
    auto generation = [&]() { return int(grid ? gx.strokeGen : ex.strokeGen); };
    mesh::AttrData<float3> *disp = nullptr;
    mesh::AttrData<int> *gen = nullptr;
    auto stored = []<class T>(mesh::AttrData<T> *data, int vertex) -> T {
      return data && vertex < data->size() ? data->safe_get(vertex) : T{};
    };
    auto state = [&]() {
      std::string bytes = grid ? gridState(gx) : std::string();
      auto append = [&](const auto &value) {
        bytes.append(reinterpret_cast<const char *>(&value), sizeof(value));
      };
      for (const auto &co : coords())
        append(co);
      append(mesh->v.attrs.attrs.size());
      append(ex.isFirstOfStep);
      append(gx.isFirstOfStep);
      append(brush.strokePathCount);
      append(generation());
      if (disp && gen) {
        append(disp->pages.size());
        append(gen->pages.size());
        for (const auto &page : disp->pages)
          append(page.data);
        for (const auto &page : gen->pages)
          append(page.data);
        for (int v = 0; v < count; v++) {
          append(stored(gen, v));
          append(stored(disp, v));
        }
      }
      return bytes;
    };
    auto reject = [&]() {
      if (!resolved)
        return;
      BrushProgram bad;
      bad.addCommand(int(SculptBrushes::DRAW));
      bad.addCommand(int(SculptBrushes::BSMOOTH));
      bad.commands[1].scalarOverrides.append(
          {"radius", props::Prop::FLOAT32, std::numeric_limits<double>::quiet_NaN()});
      const auto before = state();
      auto status = grid
                        ? gx.applyResolvedProgram(&bad, float3(0, 0, 1), float3(0, 0, 1))
                        : ex.applyResolvedProgram(&bad, float3(0, 0, 1), float3(0, 0, 1));
      test_assert(status.error != PropError::ERROR_NONE && state() == before);
    };
    const float3 center(0, 0, 1), normal(0, 0, 1);
    std::vector<bool> firstUnion(count, false);
    if (grid) {
      Vector<int> leaves;
      gtree->query(center, .45f, leaves);
      for (int li : leaves)
        for (int v : gtree->leaves[li].ownedVerts)
          firstUnion[v] = true;
    } else {
      Vector<spatial::SpatialNode *> leaves;
      tree.filterNodes(center, .45f, leaves);
      for (auto *node : leaves)
        for (int v : node->data->unique_verts)
          firstUnion[v] = true;
    }
    for (int stroke = 0; stroke < 2; stroke++) {
      const auto beforeStroke = coords();
      begin();
      for (int dab = 0; dab < 3; dab++) {
        const auto tool = dab == 1 ? SculptBrushes::BSMOOTH : SculptBrushes::DRAW;
        const float radius = dab == 0 ? .45f : (dab == 1 ? 1.1f : 1.7f);
        brush.radius = radius;
        brush.strength = .6f;
        brush.projection = .25f;
        brush.writeProps();
        reject();
        const auto before = coords();
        std::vector<float3> base = before, priorDisp(count, float3(0.0f));
        if (nonAccum && disp && gen)
          for (int v = 0; v < count; v++)
            if (stored(gen, v) == generation()) {
              priorDisp[v] = stored(disp, v);
              base[v] -= priorDisp[v];
            }
        if (resolved) {
          auto status = grid ? gx.applyResolvedDab(tool, center, normal)
                             : ex.applyResolvedDab(tool, center, normal);
          test_assert(status.error == PropError::ERROR_NONE);
        } else if (grid)
          gx.applyDab(tool, center, normal);
        else {
          ex.applyDab(tool, center, normal, radius, nullptr, 0);
          tree.updateQueries();
        }
        if (nonAccum && tool == SculptBrushes::DRAW) {
          disp = ctx.dispVec;
          gen = ctx.dispGen;
          test_assert(disp && gen);
        }
        auto after = coords();
        int newlyMoved = 0, changedBase = 0;
        for (int v = 0; v < count; v++) {
          result.push_back(after[v]);
          if (!firstUnion[v] && (after[v] - before[v]).lengthSqr() > 0)
            newlyMoved++;
          if (nonAccum && stored(gen, v) == generation()) {
            auto currentBase = after[v] - stored(disp, v);
            if (tool == SculptBrushes::DRAW)
              test_assert((currentBase - base[v]).length() < 1e-6f);
            else {
              test_assert((stored(disp, v) - priorDisp[v]).length() == 0);
              changedBase += (currentBase - base[v]).length() > 1e-7f;
            }
          }
        }
        if (dab == 2)
          test_assert(newlyMoved > 0);
        if (nonAccum && dab == 1)
          test_assert(changedBase > 0);
      }
      end();
      const auto afterStroke = coords();
      if (resolved) {
        if (grid)
          test_assert(gl.undo());
        else
          ml.undo(mesh, &tree);
        auto undone = coords();
        test_assert(
            std::memcmp(undone.data(), beforeStroke.data(), count * sizeof(float3)) == 0);
        if (grid)
          test_assert(gl.redo());
        else
          ml.redo(mesh, &tree);
        auto redone = coords();
        test_assert(
            std::memcmp(redone.data(), afterStroke.data(), count * sizeof(float3)) == 0);
      }
    }
  }
  alloc::Delete(mesh);
  return result;
}

static void maskFinerUndo()
{
  auto *cage = mesh::createCube(2, 2.0f);
  {
    subdiv::Multires mr;
    mr.init(*cage, 3);
    auto *domain = mr.gridDomain(2);
    auto *fine = mr.gridDomain(3);
    for (auto *d : {domain, fine}) {
      for (int v = 0; v < d->vertCount(); v++)
        d->mask[v] = .125f + .001f * float(v % 31);
      d->flushMaskToStore();
    }
    std::vector<float> fineBefore;
    for (int v = 0; v < fine->vertCount(); v++)
      fineBefore.push_back(fine->mask[v]);
    const auto before = storeBlob(mr);
    Brush brush;
    brush.radius = 1.7f;
    brush.strength = .2f;
    brush.writeProps();
    subdiv::GridStrokeLog log;
    GridBrushExecutor ex(domain, &brush, &log);
    ex.beginStep();
    test_assert(ex.supportsResolved(SculptBrushes::MASK));
    test_assert(ex.applyResolvedDab(SculptBrushes::MASK, float3(0, 0, 1), float3(0, 0, 1))
                    .error == PropError::ERROR_NONE);
    ex.endStep();
    const auto after = storeBlob(mr);
    test_assert(after != before);
    test_assert(log.undo());
    test_assert(storeBlob(mr) == before);
    fine = mr.gridDomain(3);
    for (int v = 0; v < fine->vertCount(); v++)
      test_assert(fine->mask[v] == fineBefore[v]);
    test_assert(log.redo());
    test_assert(storeBlob(mr) == after);
  }
  alloc::Delete(cage);
  fprintf(stderr, "prepared MASK finer-level undo regression passed\n");
}

static void maskUndoStorage()
{
  // Allocated, absent and evicted levels; the absent middle level is seeded
  // whole by propagation, while existing finer data changes only in touched grids.
  for (bool absent : {false, true}) {
    auto *cage = mesh::createCube(2, 2.0f);
    {
      subdiv::Multires mr;
      mr.init(*cage, 4);
      auto *d = mr.gridDomain(1);
      for (int i = 0; i < 35; i++)
        mr.store.addChannel(string("unrelated") + string(std::to_string(i).c_str()), 1);
      int ch = d->ensureMaskChannel();
      test_assert(ch >= 32);
      const int grids[] = {0, 1};
      auto paint = [&](int level, int channel, float bias) {
        const int n = mr.store.channelElemsPerGrid(level, channel);
        for (int g = 0; g < mr.store.gridCount(); g++) {
          float *p = mr.store.elem(level, channel, g, 0, 0);
          for (int i = 0; i < n; i++)
            p[i] = bias + float(g * n + i) / 65536.0f;
        }
      };
      for (int l : {1, 2, 4})
        paint(l, ch, .125f);
      if (!absent)
        paint(3, ch, .25f);
      mr.store.setChannelLevelDebt(3, ch, true);
      const auto before = storeBlob(mr);
      mr.store.evictLevel(4);
      subdiv::GridStrokeLog log;
      log.attach(d);
      log.beginStep();
      log.captureGrids(grids, ch);
      log.captureFinerMask(grids, ch);
      const auto capturedBytes = log.bytes();
      test_assert(mr.store.channelLevelAllocated(3, ch) == !absent);
      // Mutate only captured blocks and capture them again. Earliest values win.
      for (int l = 1; l <= 4; l++) {
        for (int g : grids)
          *mr.store.elem(l, ch, g, 0, 0) += .2f;
        mr.store.setChannelLevelDebt(l, ch, !mr.store.channelLevelDebt(l, ch));
      }
      log.captureGrids(grids, ch);
      log.captureFinerMask(grids, ch);
      test_assert(log.bytes() == capturedBytes);
      log.endStep(false);
      const auto after = storeBlob(mr);
      for (int repeat = 0; repeat < 3; repeat++) {
        test_assert(log.undo());
        test_assert(mr.store.channelLevelAllocated(3, ch) == !absent);
        test_assert(mr.store.channelLevelDebt(3, ch));
        test_assert(storeBlob(mr) == before);
        test_assert(absent ? log.bytes() > capturedBytes : log.bytes() == capturedBytes);
        test_assert(log.redo());
        test_assert(storeBlob(mr) == after);
        test_assert(log.bytes() == capturedBytes);
      }
      // Channel reindexing preserves identity; same-name replacement does not.
      mr.store.removeChannel(ch - 1);
      ch = mr.store.findChannel(string("mask"));
      test_assert(log.undo());
      test_assert(*mr.store.elem(1, ch, 0, 0, 0) == .125f);
      test_assert(log.redo());
      mr.store.removeChannel(ch);
      ch = d->ensureMaskChannel();
      paint(1, ch, .75f);
      paint(4, ch, .875f);
      mr.store.setChannelLevelDebt(1, ch, true);
      mr.store.setChannelLevelDebt(4, ch, true);
      const auto replacement = storeBlob(mr);
      test_assert(log.undo());
      test_assert(storeBlob(mr) == replacement);
      test_assert(mr.store.channelLevelDebt(1, ch));
      test_assert(mr.store.channelLevelDebt(4, ch));
      log.beginStep(); // Truncate snapshots whose channel has been replaced.
      log.endStep(false);
      test_assert(log.bytes() == 0 && log.stepCount() == 0);
      log.attach(nullptr);
    }
    alloc::Delete(cage);
  }
  // First ever mask channel: the shared fold establishes it before capture.
  for (bool zeroDelta : {false, true}) {
    auto *cage = mesh::createCube(2, 2.0f);
    {
      subdiv::Multires mr;
      mr.init(*cage, 3);
      auto *d = mr.gridDomain(1);
      mr.gridDomain(3); // Cache a fine domain before the mask channel exists.
      subdiv::GridStrokeLog log;
      log.attach(d);
      log.beginStep();
      test_assert(mr.store.findChannel(string("mask")) < 0);
      const int touched[] = {0};
      d->mask[0] = zeroDelta ? 0.0f : .5f;
      gridsFoldStroke(d, &log, touched, false, true);
      int ch = mr.store.findChannel(string("mask"));
      test_assert(ch >= 0);
      test_assert(mr.store.channelLevelAllocated(3, ch) == !zeroDelta);
      const auto after = storeBlob(mr);
      test_assert(log.undo());
      test_assert(!mr.store.channelLevelAllocated(2, ch));
      test_assert(!mr.store.channelLevelAllocated(3, ch));
      for (int level : {2, 3}) {
        auto *fine = mr.gridDomain(level);
        for (int v = 0; v < fine->vertCount(); v++)
          test_assert(fine->mask[v] == 0.0f);
        test_assert(!mr.store.channelLevelAllocated(level, ch));
      }
      test_assert(!mr.store.channelLevelDebt(1, ch));
      test_assert(log.redo());
      test_assert(storeBlob(mr) == after);
      // A recreated level must not receive snapshots from the dropped instance.
      mr.store.dropTopLevel();
      mr.store.addLevel();
      *mr.store.elem(3, ch, 0, 0, 0) = .875f;
      mr.store.setChannelLevelDebt(3, ch, true);
      test_assert(log.undo());
      test_assert(*mr.store.elem(3, ch, 0, 0, 0) == .875f);
      test_assert(mr.store.channelLevelDebt(3, ch));
      test_assert(log.redo());
      test_assert(log.dropOldest());
      test_assert(log.bytes() == 0);
      log.attach(nullptr);
    }
    alloc::Delete(cage);
  }
  fprintf(stderr, "mask undo allocation, identity, debt and history cases passed\n");
}

static void maskUndoFoldAndLifetime()
{
  for (int cleanup = 0; cleanup < 3; cleanup++) {
    auto *cage = mesh::createCube(2, 2.0f);
    {
      subdiv::Multires mr;
      mr.init(*cage, 4);
      auto *d = mr.gridDomain(1);
      d->ensureTree(4);
      int ch = d->ensureMaskChannel();
      const int touched[] = {0};
      auto occs = d->occurrences(0);
      std::vector<int> occurrences(occs.begin(), occs.end());
      for (int level : {2, 4}) {
        const int n = mr.store.channelElemsPerGrid(level, ch);
        for (int grid = 0; grid < mr.store.gridCount(); grid++) {
          float *p = mr.store.elem(level, ch, grid, 0, 0);
          for (int i = 0; i < n; i++)
            p[i] = level == 2 ? .125f : .25f;
        }
      }
      // Coarse storage already exists, so the exact pre-stroke blob is stable.
      d->flushMaskToStore();
      const auto before = storeBlob(mr);
      subdiv::GridStrokeLog log;
      log.attach(d);
      // An older applied step allows eviction while newer redo owns buffers.
      log.beginStep();
      const int firstGrid[] = {0};
      log.captureGrids(firstGrid, ch);
      log.endStep(false);
      log.beginStep();
      auto *tree = d->ensureTree();
      for (int leaf = 0; leaf < int(tree->leaves.size()); leaf++)
        log.captureLeaf(leaf, false, true);
      const size_t leafPayload = log.bytes();
      for (int leaf = int(tree->leaves.size()) - 1; leaf >= 0; leaf--)
        log.captureLeaf(leaf, false, true);
      test_assert(log.bytes() == leafPayload);
      d->mask[0] = .5f;
      gridsFoldStroke(d, &log, touched, false, true);
      test_assert(mr.store.channelLevelAllocated(3, ch));
      // Independent full-grid bilinear oracle: existing fine detail is added
      // to, while the absent middle level is seeded from level 2 after editing.
      for (int level = 2; level <= 4; level++) {
        const int scale = 1 << (level - 1);
        const int side = mr.store.sideForLevel(level);
        for (int grid = 0; grid < mr.store.gridCount(); grid++)
          for (int v = 0; v <= side; v++)
            for (int u = 0; u <= side; u++) {
              float want = level == 4 ? .25f : .125f;
              for (size_t k = 0; k < occurrences.size(); k += 3) {
                if (occurrences[k] != grid)
                  continue;
                float wu = std::max(
                    0.0f, 1.0f - std::abs(float(u) / scale - occurrences[k + 1]));
                float wv = std::max(
                    0.0f, 1.0f - std::abs(float(v) / scale - occurrences[k + 2]));
                want += .5f * wu * wv;
              }
              test_assert(*mr.store.elem(level, ch, grid, u, v) == want);
            }
      }
      const auto after = storeBlob(mr);
      if (cleanup == 1)
        mr.store.evictLevel(3); // The owning redo snapshot must retain compressed bytes.
      size_t liveBytes = mr.store.residentBytes() + mr.store.evictedBytes();
      size_t payload = log.bytes();
      const size_t budgetBeforeUndo = log.retainedBytes();
      test_assert(log.undo());
      test_assert(d->mask[0] == 0.0f);
      test_assert(!mr.store.channelLevelAllocated(3, ch));
      const size_t held = log.bytes() - payload;
      test_assert(held > 0);
      // Evicting level 3 also pages unrelated channels; the seek leaves those
      // untouched, so the live byte difference is exactly the mask allocation.
      test_assert(held == liveBytes - mr.store.residentBytes() - mr.store.evictedBytes());
      test_assert(storeBlob(mr) == before);
      test_assert(log.retainedBytes() > log.bytes());
      // Fixed push-time charge already includes the future owning payload.
      test_assert(log.bytes() < budgetBeforeUndo);
      if (cleanup == 0) {
        log.beginStep();
        log.endStep(false);
        test_assert(!log.canRedo());
        test_assert(log.dropOldest());
      } else if (cleanup == 1) {
        test_assert(log.redo());
        test_assert(d->mask[0] == .5f);
        test_assert(storeBlob(mr) == after); // Rehydrate compressed owning state.
        test_assert(log.undo());
        log.attach(nullptr);
      } else {
        test_assert(log.dropOldest()); // Move the populated owning redo snapshot.
        test_assert(log.redo());
        test_assert(storeBlob(mr) == after);
        test_assert(log.undo());
        log.attach(d);
      }
      test_assert(log.bytes() == 0 && log.retainedBytes() == 0);
    }
    alloc::Delete(cage);
  }
  // Replacement during an open step needs a distinct leaf-mask first touch.
  auto *cage = mesh::createCube(2, 2.0f);
  {
    subdiv::Multires mr;
    mr.init(*cage, 2);
    auto *d = mr.gridDomain(1);
    auto *tree = d->ensureTree();
    subdiv::GridStrokeLog log;
    log.attach(d);
    log.beginStep();
    log.captureLeaf(0, false, true);
    int ch = mr.store.findChannel(string("mask"));
    mr.store.removeChannel(ch);
    ch = d->ensureMaskChannel();
    for (int v : tree->leaves[0].ownedVerts)
      d->mask[v] = .25f;
    d->flushMaskToStore();
    log.captureLeaf(0, false, true);
    for (int v : tree->leaves[0].ownedVerts)
      d->mask[v] = .5f;
    auto &verts = tree->leaves[0].ownedVerts;
    gridsFoldStroke(d, &log, {verts.data(), verts.size()}, false, true);
    test_assert(log.undo());
    for (int v : tree->leaves[0].ownedVerts)
      test_assert(d->mask[v] == .25f);
    d->syncMaskFromStore();
    for (int v : tree->leaves[0].ownedVerts)
      test_assert(d->mask[v] == .25f);
    test_assert(log.redo());
    for (int v : tree->leaves[0].ownedVerts)
      test_assert(d->mask[v] == .5f);
  }
  alloc::Delete(cage);
  fprintf(stderr, "mask shared fold interpolation and owning cleanup passed\n");
}

static void maskCaptureIndexes()
{
  auto *cage = mesh::createCube(2, 2.0f);
  {
    subdiv::Multires mr;
    mr.init(*cage, 3);
    auto *d = mr.gridDomain(1);
    subdiv::GridStrokeLog log;
    log.attach(d);
    std::vector<int> channels;
    for (int i = 0; i < 40; i++)
      channels.push_back(mr.store.addChannel(string(std::to_string(i).c_str()), 1));
    log.beginStep();
    const int first[] = {0, 1}, overlap[] = {1, 2};
    for (int ch : channels) {
      size_t bytes = log.bytes();
      log.captureGrids(first, ch);
      const size_t blockBytes = mr.store.channelElemsPerGrid(1, ch) * sizeof(float);
      test_assert(log.bytes() == bytes + 2 * blockBytes);
      *mr.store.elem(1, ch, 1, 0, 0) = .25f;
      *mr.store.elem(1, ch, 2, 0, 0) = .5f;
      log.captureGrids(overlap, ch);
      test_assert(log.bytes() == bytes + 3 * blockBytes);
      *mr.store.elem(1, ch, 2, 0, 0) = .75f;
    }
    log.endStep(false);
    test_assert(log.undo());
    for (int ch : channels) {
      test_assert(*mr.store.elem(1, ch, 1, 0, 0) == 0);
      test_assert(*mr.store.elem(1, ch, 2, 0, 0) == .5f);
    }
    test_assert(log.redo());
    for (int ch : channels)
      test_assert(*mr.store.elem(1, ch, 2, 0, 0) == .75f);
    log.attach(d);
    int mask = d->ensureMaskChannel();
    mr.store.setChannelLevelDebt(3, mask, true);
    log.beginStep();
    log.captureFinerMask(first, mask);
    *mr.store.elem(3, mask, 0, 0, 0) = .5f;
    mr.store.setChannelLevelDebt(3, mask, false);
    log.endStep(false);
    test_assert(log.canUndo());
    test_assert(log.undo());
    test_assert(!mr.store.channelLevelAllocated(3, mask));
    test_assert(mr.store.channelLevelDebt(3, mask));
    test_assert(log.redo());
    test_assert(*mr.store.elem(3, mask, 0, 0, 0) == .5f);
    test_assert(!mr.store.channelLevelDebt(3, mask));
  }
  alloc::Delete(cage);
  fprintf(stderr,
          "mask indexed multichannel capture and finer-only transaction passed\n");
}

#include "prepared_attribute_cases.h"
#include "prepared_cavity_cases.h"
#include "prepared_dyntopo_cases.h"
#include "prepared_enhance_cases.h"
#include "prepared_grab_cases.h"
#include "prepared_falloff_cases.h"
#include "prepared_preview_cases.h"
#include "prepared_unbounded_cases.h"

int main()
{
  preparedFalloffGate();
  preparedAttributeCases();
  preparedDyntopoCases();
  preparedPreviewCases();
  preparedGrabCases();
  preparedEnhanceCases();
  preparedEnhanceSupport();
  generatedPreludeExecution();
  kelvinletNumerics();
  unboundedSnapshots();
  unboundedMatrix();
  cavityPreparation();
  cavityExecutors();
  cavityEffectsMatrix();
  maskCaptureIndexes();
  maskUndoFoldAndLifetime();
  maskUndoStorage();
  maskFinerUndo();
  for (bool grid : {false, true}) {
    auto resolved = nonAccumLifetime(grid, true, true);
    auto raw = nonAccumLifetime(grid, false, true);
    auto accumulated = nonAccumLifetime(grid, false, false);
    test_assert(resolved.size() == raw.size() && raw.size() == accumulated.size());
    int different = 0;
    for (size_t i = 0; i < raw.size(); i++) {
      test_assert((resolved[i] - raw[i]).length() < 1e-6f);
      different += (raw[i] - accumulated[i]).length() > 1e-6f;
    }
    test_assert(different > 0);
    for (auto mode :
         {CommandExecutor::NeighborMode::LiveDisk, CommandExecutor::NeighborMode::Csr})
      for (bool deferred : {false, true}) {
        if ((grid && mode == CommandExecutor::NeighborMode::Csr) || (!grid && deferred))
          continue;
        auto prepared =
            programGeometry(grid, true, mode, deferred, SculptBrushes::BSMOOTH, true);
        auto reference =
            programGeometry(grid, false, mode, deferred, SculptBrushes::BSMOOTH, true);
        test_assert(prepared.size() == reference.size());
        for (size_t i = 0; i < reference.size(); i++)
          test_assert((prepared[i] - reference[i]).length() < 1e-6f);
      }
  }
  fprintf(stderr,
          "prepared nonaccum templates, displacement lifetime, region growth and retry "
          "passed\n");
  boundaryCapability();
  for (auto mode :
       {CommandExecutor::NeighborMode::LiveDisk, CommandExecutor::NeighborMode::Csr})
    for (bool program : {false, true})
      for (bool zero : {false, true}) {
        auto resolved = boundaryFirstHit(true, program, zero, mode);
        auto raw = boundaryFirstHit(false, program, zero, mode);
        test_assert(resolved.size() == raw.size());
        for (size_t i = 0; i < raw.size(); i++)
          test_assert((resolved[i] - raw[i]).length() < 1e-6f);
      }
  for (bool deferred : {false, true}) {
    auto resolved = gridSmooth(true, deferred, SculptBrushes::BSMOOTH);
    auto raw = gridSmooth(false, deferred, SculptBrushes::BSMOOTH);
    test_assert(resolved.size() == raw.size());
    for (size_t i = 0; i < raw.size(); i++)
      test_assert((resolved[i] - raw[i]).length() < 1e-6f);
  }
  for (bool grid : {false, true})
    for (auto mode :
         {CommandExecutor::NeighborMode::LiveDisk, CommandExecutor::NeighborMode::Csr})
      for (bool deferred : {false, true}) {
        if ((grid && mode == CommandExecutor::NeighborMode::Csr) || (!grid && deferred))
          continue;
        auto resolved =
            programGeometry(grid, true, mode, deferred, SculptBrushes::BSMOOTH);
        auto raw = programGeometry(grid, false, mode, deferred, SculptBrushes::BSMOOTH);
        test_assert(resolved.size() == raw.size());
        for (size_t i = 0; i < raw.size(); i++)
          test_assert((resolved[i] - raw[i]).length() < 1e-6f);
      }
  fprintf(stderr, "prepared BSMOOTH boundary, program, grid and undo gates passed\n");
  rawRevalidation();
  meshExecution();
  capabilities();
  gridExecution();
  gridCapabilities();
  for (bool deferred : {false, true}) {
    auto resolved = gridSmooth(true, deferred);
    auto raw = gridSmooth(false, deferred);
    test_assert(resolved.size() == raw.size());
    for (size_t i = 0; i < raw.size(); i++)
      test_assert((resolved[i] - raw[i]).length() < 1e-6f);
  }
  fprintf(stderr, "resolved grid SMOOTH raw-reference and deferred normals passed\n");
  for (bool grid : {false, true}) {
    programRejection(grid);
    for (auto mode :
         {CommandExecutor::NeighborMode::LiveDisk, CommandExecutor::NeighborMode::Csr})
    {
      if (grid && mode == CommandExecutor::NeighborMode::Csr)
        continue;
      for (bool deferred : {false, true}) {
        if (!grid && deferred)
          continue;
        auto resolved = programGeometry(grid, true, mode, deferred);
        auto raw = programGeometry(grid, false, mode, deferred);
        test_assert(resolved.size() == raw.size());
        for (size_t i = 0; i < raw.size(); i++)
          test_assert((resolved[i] - raw[i]).length() < 1e-6f);
      }
    }
  }
  fprintf(
      stderr,
      "resolved mesh/grid programs, widened raw reference and atomic rejection passed\n");
  {
    auto live = smooth(CommandExecutor::NeighborMode::LiveDisk);
    auto csr = smooth(CommandExecutor::NeighborMode::Csr);
    test_assert(live.size() == csr.size());
    for (size_t i = 0; i < live.size(); i++)
      test_assert((live[i] - csr[i]).length() < 1e-6f);
  }
  fprintf(stderr, "resolved mesh capability and neighbor dispatch passed\n");
  commandStackConfiguration();
  return test_end();
}
