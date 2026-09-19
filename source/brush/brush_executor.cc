#include "brush/brush_executor.h"
#include "brush/brush_preparation.h"
#include "brush/brush_program_preparation.h"
#include <atomic>
#include <limits>

namespace sculptcore::brush {
namespace {
bool resolvedCapability(CommandExecutor &executor,
                        const CommandExecutor::brush_command &command,
                        SculptBrushes type)
{
  const auto &brush = *executor.brush;
  return !((executor.stepHasDyntopo && executor.previewActive()) ||
           !resolvedAttributesSupported(
               {command.attrs.size() ? &command.attrs[0] : nullptr, command.attrs.size()},
               type == SculptBrushes::ENHANCE,
               true) ||
           (command.execHost && !command.preparedHostNoop) ||
           !command.preparedScalarSafe || !preparedHooksSupported(type, false) ||
           !executor.preparedPreviewSupported() || !resolvedFalloffSupported(brush));
}
} // namespace

bool CommandExecutor::preflightRaw(SculptBrushes type)
{
  Brush scratch;
  brush_command command;
  if (!brush ||
      !createCommandSwitch<AccumLive>(
          type, effectiveNeighborMode() == NeighborMode::Csr, &scratch, command))
  {
    lastRegistration = {props::PropError::ERROR_NOT_EXISTS, "kernel or brush"};
  } else {
    PreparedBrushScalars prepared;
    lastRegistration =
        prepareBrushScalars(*brush,
                            {command.uniforms.data(), command.uniforms.size()},
                            brush->deviceInputCtx,
                            prepared,
                            {},
                            {},
                            ScalarSource::RawStandalone);
  }
  lastValidation = {};
  lastValidation.ok = lastRegistration.error == props::PropError::ERROR_NONE;
  strokeValidationFailed = !lastValidation.ok;
  if (!lastValidation.ok)
    fprintf(stderr,
            "SculptCore: rejected %s (property status %d)\n",
            lastRegistration.name.c_str(),
            int(lastRegistration.error));
  return lastValidation.ok;
}

bool CommandExecutor::preflightRawProgram(BrushProgram *program)
{
  lastValidation = {};
  lastRegistration = {};
  auto validate = [&]() -> props::ScalarRegistrationResult {
    if (!brush || !program)
      return {props::PropError::ERROR_INVALID_OWNER, "program or brush"};
    Vector<brush_command> commands;
    commands.resize(program->commands.size());
    Vector<std::span<const BrushUniformManifestEntry>> manifests;
    ScopedPreparedCavity cavityScope(preparedCavityActive);
    for (size_t i = 0; i < commands.size(); i++) {
      Brush scratch;
      const auto &entry = program->commands[i];
      if (entry.dynamicsOverrides.size() || entry.scalarOverrides.size() ||
          entry.cavityCurveOverride.size())
        return programError(i,
                            {props::PropError::ERROR_INVALID_VALUE,
                             "typed overrides require resolved execution"});
      if (!createCommandSwitch<AccumLive>(entry.type,
                                          effectiveNeighborMode() == NeighborMode::Csr,
                                          &scratch,
                                          commands[i]))
        return programError(i, {props::PropError::ERROR_NOT_EXISTS, "kernel"});
      manifests.append({commands[i].uniforms.data(), commands[i].uniforms.size()});
    }
    if (!commands.size()) {
      PreparedBrushScalars prepared;
      return prepareBrushScalars(*brush, {}, brush->deviceInputCtx, prepared);
    }
    PreparedProgramScalars prepared;
    return prepareProgramScalars(*brush,
                                 *program,
                                 {manifests.data(), manifests.size()},
                                 brush->deviceInputCtx,
                                 prepared);
  };
  lastRegistration = validate();
  lastValidation.ok = lastRegistration.error == props::PropError::ERROR_NONE;
  strokeValidationFailed = !lastValidation.ok;
  if (!lastValidation.ok)
    fprintf(stderr,
            "SculptCore: rejected %s (property status %d)\n",
            lastRegistration.name.c_str(),
            int(lastRegistration.error));
  return lastValidation.ok;
}

bool CommandExecutor::createPreparedCommand(SculptBrushes type, brush_command &command)
{
  Brush scratch;
  const bool csr = effectiveNeighborMode() == NeighborMode::Csr;
  brush_command candidate;
  if (!createCommandSwitch<AccumLive>(type, csr, &scratch, candidate))
    return false;
  if (anchoredGrab && candidate.grabModeCapable) {
    brush_command original;
    if (!createCommandSwitch<AccumOrigGrab>(type, csr, &scratch, original))
      return false;
    original.grabMode = true;
    candidate = std::move(original);
  } else if (nonAccum && candidate.accumulable && !candidate.relaxesBase) {
    brush_command original;
    if (!createCommandSwitch<AccumOrig>(type, csr, &scratch, original))
      return false;
    candidate = std::move(original);
  }
  command = std::move(candidate);
  return true;
}

bool CommandExecutor::supportsResolved(SculptBrushes type)
{
  if (!brush)
    return false;
  brush_command command;
  return createPreparedCommand(type, command) && resolvedCapability(*this, command, type);
}

bool CommandExecutor::supportsResolvedProgram(BrushProgram *program)
{
  if (!program || !brush)
    return false;
  for (const auto &entry : program->commands)
    if (!supportsResolved(entry.type))
      return false;
  return true;
}

props::ScalarRegistrationResult CommandExecutor::applyResolvedDab(SculptBrushes brushType,
                                                                  float3 center,
                                                                  float3 normal,
                                                                  bool validateOnly,
                                                                  bool grabAdd)
{
  using props::PropError;
  auto fail = [&](PropError error, const char *name) {
    lastRegistration = {error, name};
    return lastRegistration;
  };
  if (!preparedStepValid() || !brush || !tree || !tree->m || !tree->getRoot()) {
    return fail(PropError::ERROR_INVALID_OWNER, "mesh");
  }
  for (int axis = 0; axis < 3; axis++) {
    if (!std::isfinite(center[axis]) || !std::isfinite(normal[axis])) {
      return fail(PropError::ERROR_INVALID_VALUE, "dab frame");
    }
  }
  if (!resolvedFalloffNormalSupported(*brush, normal[0], normal[1], normal[2]))
    return fail(PropError::ERROR_INVALID_VALUE, "falloff normal");
  brush_command command;
  if (!createPreparedCommand(brushType, command)) {
    return fail(PropError::ERROR_NOT_EXISTS, "kernel");
  }
  // These modes need additional region, host-state or attribute preflight.
  // Generated host stages may themselves change radius/center after selection.
  if (!resolvedCapability(*this, command, brushType)) {
    return fail(PropError::ERROR_INVALID_VALUE, "unsupported resolved capability");
  }
  if (command.grabModeCapable && !validGrabFrame(*brush))
    return fail(PropError::ERROR_INVALID_VALUE, "grab frame");
  Vector<BrushAttrManifestEntry> plannedAttributes;
  lastRegistration = validatePreparedAttributes(
      *tree->m,
      {command.attrs.data(), command.attrs.size()},
      {defaultAttrOverrides.data(), defaultAttrOverrides.size()},
      plannedAttributes);
  if (lastRegistration.error != PropError::ERROR_NONE)
    return lastRegistration;
  PreparedBrushScalars prepared;
  lastRegistration =
      prepareBrushScalars(*brush,
                          {command.uniforms.data(), command.uniforms.size()},
                          brush->deviceInputCtx,
                          prepared);
  if (lastRegistration.error != PropError::ERROR_NONE) {
    return lastRegistration;
  }
  float radius = -1;
  for (const auto &value : prepared.values()) {
    if (value.name == string("radius")) {
      radius = float(value.value);
    }
  }
  if (!std::isfinite(radius) || !std::isfinite(brush->falloffSupportRadius(radius)) || radius < 0 ||
      (radius > 0 && !std::isfinite(1.0f / radius)))
  {
    return fail(PropError::ERROR_INVALID_VALUE, "radius");
  }
  // No callbacks or yields between preparation and publication. The exact
  // scratch command above executes below; no live factory or legacy loader.
  if (command.unbounded) {
    lastRegistration = validateUnboundedSupport(prepared, radius);
    if (lastRegistration.error != PropError::ERROR_NONE)
      return lastRegistration;
  }
  if (validateOnly)
    return {};
  ScopedBrushWorkingValues executorSettings(*brush, {&prepared, 1}, true);
  lastRegistration = prepared.publish(*brush);
  if (lastRegistration.error != PropError::ERROR_NONE) {
    return lastRegistration;
  }
  setGrabAccumAdd(grabAdd);
  dabNodes_.clear();
  if (radius > 0) {
    if (command.unbounded) {
      for (auto *node : tree->leaves())
        if (node->data && node->data->unique_verts.size())
          dabNodes_.append(node);
    } else if (command.grabMode) {
      const float support = brush->falloffSupportRadius(radius);
      grabFilterNodes(center, support, support, dabNodes_);
    } else {
      tree->filterNodes(center, brush->falloffSupportRadius(radius), dabNodes_);
    }
  }
  lastDabNodeCount = int(dabNodes_.size());
  capturePreparedPreview();
  if (dabNodes_.size()) {
    auto *mesh = tree->m;
    if (const auto *hooks = brushHooksFor(brushType); hooks && hooks->stepPreFreeze) {
      BrushHookCtx hc{*this, mesh, &dabNodes_, brush, isFirstOfStep};
      hooks->stepPreFreeze(hc);
    }
    if (brush->automask_cavity) {
      if (mesh->topo_frozen && !mesh->topo_cache.valid(*mesh))
        mesh->thawTopo();
      mesh->topo_cache.ensureRing1(*mesh);
    }
    if (brushNeedsLiveLinks(brushType) || keepTopoThawed || stepHasDyntopo) {
      if (mesh->topo_frozen)
        mesh->thawTopo();
    } else if (!mesh->topo_frozen) {
      mesh->freezeTopo();
    }
    ctx.m = mesh;
    ctx.surfaceNo = normal;
    ctx.surfacePos = center;
    ctx.meshLog = meshLog;
    ctx.isFirstOfStep = isFirstOfStep;
    updateStrokeFrame(center);
    brush->pushStrokeSample(center, normal);
    curCaptureSlot = 0;
    curCaptureTool = int(brushType);
    ScopedPreparedCavity cavityScope(preparedCavityActive);
    BrushHookCtx hc{*this, mesh, &dabNodes_, brush, isFirstOfStep};
    const auto *hooks = brushHooksFor(brushType);
    if (hooks && hooks->preparedStageHooks && hooks->dabPre)
      hooks->dabPre(hc);
    exec(command, {dabNodes_.data(), dabNodes_.size()});
    if (hooks && hooks->preparedStageHooks && hooks->dabPost)
      hooks->dabPost(hc);
    curCaptureSlot = -1;
    tree->updateQueries();
  }
  clearIsFirstOfStep();
  return {};
}

props::ScalarRegistrationResult
CommandExecutor::applyResolvedProgram(BrushProgram *program,
                                      float3 center,
                                      float3 normal,
                                      bool validateOnly,
                                      bool grabAdd,
                                      dyntopo::DynTopoParams *topology,
                                      float topologyRadius,
                                      uint32_t topologySeed)
{
  using props::PropError;
  auto fail = [&](PropError error, const char *name) {
    lastRegistration = {error, name};
    return lastRegistration;
  };
  if (!preparedStepValid() || !program || !brush || !tree || !tree->m || !tree->getRoot())
    return fail(PropError::ERROR_INVALID_OWNER, "program or mesh");
  for (int axis = 0; axis < 3; axis++)
    if (!std::isfinite(center[axis]) || !std::isfinite(normal[axis]))
      return fail(PropError::ERROR_INVALID_VALUE, "dab frame");
  if (topology) {
    const auto &p = *topology;
    if (!stepHasDyntopo || previewActive() || tree->m->topoLocked ||
        !program->commands.size() || !std::isfinite(topologyRadius) ||
        topologyRadius < 0 || !std::isfinite(p.l_max) || p.l_max <= 0 ||
        !std::isfinite(p.l_min) || p.l_min < 0 || !std::isfinite(p.grade) ||
        p.grade < 0 || !std::isfinite(p.smooth_lambda) || p.smooth_lambda < 0 ||
        p.smooth_lambda > 1 || !std::isfinite(p.feature_corner_angle) ||
        p.feature_corner_angle < 0 || p.max_rounds < 0 || p.max_splits < 0 ||
        p.max_collapses < 0 || p.max_stall_rounds < 0 || int(p.mode) < 0 ||
        int(p.mode) > 2)
      return fail(PropError::ERROR_INVALID_VALUE, "dyntopo settings");
  }
  if (!program->commands.size()) {
    PreparedBrushScalars empty;
    lastRegistration = prepareBrushScalars(*brush, {}, brush->deviceInputCtx, empty);
    if (lastRegistration.error != PropError::ERROR_NONE || validateOnly)
      return lastRegistration;
    lastRegistration = {};
    dabNodes_.clear();
    lastDabNodeCount = 0;
    capturePreparedPreview();
    clearIsFirstOfStep();
    return {};
  }
  if (!resolvedFalloffNormalSupported(*brush, normal[0], normal[1], normal[2])) {
    lastRegistration = {PropError::ERROR_INVALID_VALUE, "falloff normal"};
    return lastRegistration;
  }
  Vector<brush_command> commands;
  commands.resize(program->commands.size());
  Vector<std::span<const BrushUniformManifestEntry>> manifests;
  bool needsLive = keepTopoThawed || stepHasDyntopo;
  Vector<BrushAttrManifestEntry> plannedAttributes;
  for (size_t i = 0; i < program->commands.size(); i++) {
    const auto &entry = program->commands[i];
    auto &command = commands[i];
    if (!createPreparedCommand(entry.type, command)) {
      lastRegistration = programError(i, {PropError::ERROR_NOT_EXISTS, "kernel"});
      return lastRegistration;
    }
    if (!resolvedCapability(*this, command, entry.type)) {
      lastRegistration = programError(
          i, {PropError::ERROR_INVALID_VALUE, "unsupported resolved capability"});
      return lastRegistration;
    }
    needsLive |= brushNeedsLiveLinks(entry.type);
    if (command.grabModeCapable && !validGrabFrame(*brush)) {
      lastRegistration = programError(i, {PropError::ERROR_INVALID_VALUE, "grab frame"});
      return lastRegistration;
    }
    const auto &overrides =
        entry.attrLayerOverrides.size() ? entry.attrLayerOverrides : defaultAttrOverrides;
    lastRegistration =
        programError(i,
                     validatePreparedAttributes(
                         *tree->m,
                         {command.attrs.data(), command.attrs.size()},
                         {overrides.size() ? &overrides[0] : nullptr, overrides.size()},
                         plannedAttributes));
    if (lastRegistration.error != PropError::ERROR_NONE)
      return lastRegistration;
    manifests.append({command.uniforms.data(), command.uniforms.size()});
  }
  PreparedProgramScalars prepared;
  lastRegistration = prepareProgramScalars(*brush,
                                           *program,
                                           {manifests.data(), manifests.size()},
                                           brush->deviceInputCtx,
                                           prepared);
  if (lastRegistration.error != PropError::ERROR_NONE)
    return lastRegistration;
  const float support = brush->falloffSupportRadius(prepared.radius);
  if (!std::isfinite(support))
    return fail(PropError::ERROR_INVALID_VALUE, "falloff support");
  bool allLeaves = false, hasGrab = false;
  for (size_t i = 0; i < commands.size(); i++) {
    hasGrab |= commands[i].grabMode && prepared.radii[i] > 0;
    if (!commands[i].unbounded)
      continue;
    lastRegistration =
        programError(i, validateUnboundedSupport(prepared.stages[i], prepared.radii[i]));
    if (lastRegistration.error != PropError::ERROR_NONE)
      return lastRegistration;
    allLeaves |= prepared.radii[i] > 0;
  }
  lastRegistration = {};
  if (validateOnly)
    return {};
  if (topology) {
    applyDynTopoDab(center, topologyRadius, topology, topologySeed);
    tree->updateQueries();
  }
  setGrabAccumAdd(grabAdd);
  dabNodes_.clear();
  if (prepared.radius > 0) {
    if (allLeaves) {
      for (auto *node : tree->leaves())
        if (node->data && node->data->unique_verts.size())
          dabNodes_.append(node);
    } else if (hasGrab) {
      grabFilterNodes(center, support, support, dabNodes_);
    } else {
      tree->filterNodes(center, support, dabNodes_);
    }
  }
  lastDabNodeCount = int(dabNodes_.size());
  capturePreparedPreview();
  if (dabNodes_.size()) {
    ScopedBrushWorkingValues working(*brush,
                                     {prepared.stages.data(), prepared.stages.size()});
    auto *mesh = tree->m;
    Vector<BrushHookFn> completed;
    BrushHookCtx hc{*this, mesh, &dabNodes_, brush, isFirstOfStep};
    for (size_t i = 0; i < program->commands.size(); i++) {
      const auto *hooks = brushHooksFor(program->commands[i].type);
      if (!prepared.radii[i] || !hooks || !hooks->stepPreFreeze)
        continue;
      bool seen = false;
      for (auto fn : completed)
        seen |= fn == hooks->stepPreFreeze;
      if (!seen) {
        hooks->stepPreFreeze(hc);
        completed.append(hooks->stepPreFreeze);
      }
    }
    bool needsCavity = false;
    for (size_t i = 0; i < prepared.stages.size(); i++) {
      if (prepared.radii[i] > 0)
        for (const auto &value : prepared.stages[i].values())
          needsCavity |= value.name == string("automask_cavity") && value.value != 0;
    }
    if (needsCavity) {
      if (mesh->topo_frozen && !mesh->topo_cache.valid(*mesh))
        mesh->thawTopo();
      mesh->topo_cache.ensureRing1(*mesh);
    }
    if (needsLive) {
      if (mesh->topo_frozen)
        mesh->thawTopo();
    } else if (!mesh->topo_frozen) {
      mesh->freezeTopo();
    }
    ctx.m = mesh;
    ctx.surfaceNo = normal;
    ctx.surfacePos = center;
    ctx.meshLog = meshLog;
    ctx.isFirstOfStep = isFirstOfStep;
    updateStrokeFrame(center);
    brush->pushStrokeSample(center, normal);
    ScopedPreparedCavity cavityScope(preparedCavityActive);
    for (size_t i = 0; i < commands.size(); i++) {
      if (prepared.radii[i] == 0)
        continue;
      prepared.stages[i].applyWorking(*brush);
      curCaptureSlot = int(i);
      curCaptureTool = int(program->commands[i].type);
      const auto *hooks = brushHooksFor(program->commands[i].type);
      if (hooks && hooks->stepPreFreeze)
        hooks->stepPreFreeze(hc);
      if (hooks && hooks->preparedStageHooks && hooks->dabPre)
        hooks->dabPre(hc);
      const auto &overrides = program->commands[i].attrLayerOverrides;
      exec(commands[i],
           {dabNodes_.data(), dabNodes_.size()},
           {overrides.size() ? &overrides[0] : nullptr, overrides.size()});
      if (hooks && hooks->preparedStageHooks && hooks->dabPost)
        hooks->dabPost(hc);
    }
    curCaptureSlot = -1;
    tree->updateQueries();
  }
  if (topology && meshLog)
    meshLog->pushTopoChunk();
  clearIsFirstOfStep();
  return {};
}

int CommandExecutor::allocateUniformQueryToken()
{
  static std::atomic<int> next{0};
  int current = next.load(std::memory_order_relaxed);
  while (current < std::numeric_limits<int>::max()) {
    if (next.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
      return current + 1;
    }
  }
  return 0;
}
} // namespace sculptcore::brush
