#include "brush/grid_executor.h"
#include "brush/brush_hooks.h"
#include "brush/brush_preparation.h"
#include "brush/brush_program_preparation.h"

#include <cmath>

namespace sculptcore::brush {
namespace {
bool maskStorageSupported(const GridBrushExecutor &executor, bool writesMask)
{
  if (!writesMask)
    return true;
  const auto &store = executor.domain->multires()->store;
  const int channel =
      store.findChannel(util::string(subdiv::GridLevelDomain::kMaskChannelName));
  return channel < 0 || (store.channelElemSize(channel) == 1 &&
                         store.channelType(channel) == mesh::AttrType::FLOAT &&
                         store.channelDomain(channel) == subdiv::GridElemDomain::Vertex);
}
bool layerStorageSupported(const GridBrushExecutor &executor)
{
  const auto *mr = executor.domain->multires();
  const int layer = mr->editTarget(), channel = mr->writebackChannel();
  if (layer < 0)
    return channel == 0;
  return layer < mr->layerCount() && mr->layerEnabled(layer) && !mr->layerFrozen(layer) &&
         mr->layerWeight(layer) == 1 && channel > 0 &&
         channel < mr->store.channelCount() && mr->store.channelElemSize(channel) == 3 &&
         mr->store.channelType(channel) == mesh::AttrType::FLOAT &&
         mr->store.channelDomain(channel) == subdiv::GridElemDomain::Vertex;
}

bool preparedGridAttributes(const GridBrushExecutor &executor,
                            std::span<const BrushAttrManifestEntry> attributes)
{
  for (const auto &attribute : attributes) {
    const auto plan = gridAttrPlan(attribute, &executor.domain->multires()->gridAttrs());
    if (plan == GridAttrPlanKind::LayerScratch)
      continue;
    if (!resolvedAttributesSupported({&attribute, 1}))
      return false;
  }
  return true;
}

} // namespace

props::ScalarRegistrationResult GridBrushExecutor::resolvedState(float3 origin,
                                                                 float3 normal) const
{
  using props::PropError;
  // Check the live owner before dereferencing a possibly freed domain or tree.
  if (!attachedMultires_ ||
      attachedMultires_->domainGeneration() != attachedGeneration_ || !domain ||
      domain != attachedDomain_ || !tree || tree != attachedTree_ || !brush)
  {
    return {PropError::ERROR_INVALID_OWNER, "grid attachment"};
  }
  if (!stepOpen_ || log != stepLog_ ||
      (log && (log->domain() != domain || !stepLogSerial_ ||
               log->openStepSerial() != stepLogSerial_)))
  {
    return {PropError::ERROR_INVALID_OWNER, "grid step"};
  }
  if (attachedMultires_->editTarget() != stepEditTarget_ ||
      attachedMultires_->writebackChannel() != stepWritebackChannel_)
    return {PropError::ERROR_INVALID_OWNER, "grid edit target"};
  for (int axis = 0; axis < 3; axis++) {
    if (!std::isfinite(origin[axis]) || !std::isfinite(normal[axis])) {
      return {PropError::ERROR_INVALID_VALUE, "dab frame"};
    }
  }
  if (!resolvedFalloffNormalSupported(*brush, normal[0], normal[1], normal[2]))
    return {PropError::ERROR_INVALID_VALUE, "falloff normal"};
  return {};
}

bool GridBrushExecutor::resolvedCapability(const brush_command &command,
                                           SculptBrushes type) const
{
  return !(!preparedGridAttributes(*this,
                                   {command.attrs.size() ? &command.attrs[0] : nullptr,
                                    command.attrs.size()}) ||
           command.faceMode || !maskStorageSupported(*this, command.writesMask) ||
           command.needsOrigNormals || (command.execHost && !command.preparedHostNoop) ||
           !command.preparedScalarSafe || !preparedHooksSupported(type, true) ||
           attrMirrors_.items.size() || !layerStorageSupported(*this) ||
           !resolvedFalloffSupported(*brush));
}

bool GridBrushExecutor::preflightRaw(SculptBrushes type, float3 origin, float3 normal)
{
  lastRegistration = resolvedState(origin, normal);
  if (lastRegistration.error != props::PropError::ERROR_NONE)
    return false;
  Brush scratch;
  brush_command command;
  if (!createCommandSwitch<AccumLive>(type, &scratch, command)) {
    lastRegistration = {props::PropError::ERROR_NOT_EXISTS, "kernel"};
    return false;
  }
  if (!maskStorageSupported(*this, command.writesMask)) {
    lastRegistration = {props::PropError::ERROR_SCHEMA_CONFLICT, "mask channel"};
    return false;
  }
  PreparedBrushScalars prepared;
  lastRegistration =
      prepareBrushScalars(*brush,
                          {command.uniforms.data(), command.uniforms.size()},
                          brush->deviceInputCtx,
                          prepared,
                          {},
                          {},
                          ScalarSource::RawStandalone);
  if (lastRegistration.error != props::PropError::ERROR_NONE)
    fprintf(stderr,
            "SculptCore: rejected %s (property status %d)\n",
            lastRegistration.name.c_str(),
            int(lastRegistration.error));
  return lastRegistration.error == props::PropError::ERROR_NONE;
}

bool GridBrushExecutor::preflightRawProgram(BrushProgram *program,
                                            float3 origin,
                                            float3 normal)
{
  lastRegistration = resolvedState(origin, normal);
  if (lastRegistration.error != props::PropError::ERROR_NONE)
    return false;
  auto validate = [&]() -> props::ScalarRegistrationResult {
    if (!program)
      return {props::PropError::ERROR_INVALID_OWNER, "program"};
    Vector<brush_command> commands;
    commands.resize(program->commands.size());
    Vector<std::span<const BrushUniformManifestEntry>> manifests;
    ScopedPreparedCavity cavityScope(preparedCavityActive);
    for (size_t i = 0; i < commands.size(); i++) {
      Brush scratch;
      const auto &entry = program->commands[i];
      if (entry.dynamicsOverrides.size() || entry.scalarOverrides.size() ||
          entry.cavityCurveOverride.size() || entry.attrLayerOverrides.size())
        return programError(
            i, {props::PropError::ERROR_INVALID_VALUE, "unsupported raw grid override"});
      if (!createCommandSwitch<AccumLive>(entry.type, &scratch, commands[i]))
        return programError(i, {props::PropError::ERROR_NOT_EXISTS, "kernel"});
      if (!maskStorageSupported(*this, commands[i].writesMask))
        return programError(i, {props::PropError::ERROR_SCHEMA_CONFLICT, "mask channel"});
      if (commands[i].grabModeCapable && anchoredGrab)
        return programError(
            i, {props::PropError::ERROR_INVALID_VALUE, "anchored grid program"});
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
  if (lastRegistration.error != props::PropError::ERROR_NONE)
    fprintf(stderr,
            "SculptCore: rejected %s (property status %d)\n",
            lastRegistration.name.c_str(),
            int(lastRegistration.error));
  return lastRegistration.error == props::PropError::ERROR_NONE;
}

bool GridBrushExecutor::createPreparedCommand(SculptBrushes type, brush_command &command)
{
  Brush scratch;
  brush_command candidate;
  if (!createCommandSwitch<AccumLive>(type, &scratch, candidate))
    return false;
  if (anchoredGrab && candidate.grabModeCapable) {
    brush_command original;
    if (!createCommandSwitch<AccumOrigGrab>(type, &scratch, original))
      return false;
    original.grabMode = true;
    candidate = std::move(original);
  } else if (nonAccum && candidate.accumulable && !candidate.relaxesBase) {
    brush_command original;
    if (!createCommandSwitch<AccumOrig>(type, &scratch, original))
      return false;
    candidate = std::move(original);
  }
  command = std::move(candidate);
  return true;
}

bool GridBrushExecutor::supportsResolved(SculptBrushes type)
{
  if (resolvedState(float3(0.0f), float3(0, 0, 1)).error != props::PropError::ERROR_NONE)
    return false;
  brush_command command;
  return createPreparedCommand(type, command) && resolvedCapability(command, type);
}

bool GridBrushExecutor::supportsResolvedProgram(BrushProgram *program)
{
  if (!program)
    return false;
  for (const auto &entry : program->commands)
    if (entry.attrLayerOverrides.size() || !supportsResolved(entry.type))
      return false;
  return true;
}

props::ScalarRegistrationResult
GridBrushExecutor::applyResolvedDab(SculptBrushes brushType,
                                    float3 origin,
                                    float3 normal,
                                    bool validateOnly,
                                    bool grabAdd)
{
  using props::PropError;
  auto fail = [&](PropError error, const char *name) {
    lastRegistration = {error, name};
    return lastRegistration;
  };
  lastRegistration = resolvedState(origin, normal);
  if (lastRegistration.error != PropError::ERROR_NONE)
    return lastRegistration;
  brush_command command;
  if (!createPreparedCommand(brushType, command)) {
    return fail(PropError::ERROR_NOT_EXISTS, "kernel");
  }
  if (!resolvedCapability(command, brushType)) {
    return fail(PropError::ERROR_INVALID_VALUE, "unsupported resolved capability");
  }
  if (command.grabModeCapable && !validGrabFrame(*brush))
    return fail(PropError::ERROR_INVALID_VALUE, "grab frame");
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
  if (!std::isfinite(radius) || radius < 0 ||
      (radius > 0 && !std::isfinite(1.0f / radius)))
  {
    return fail(PropError::ERROR_INVALID_VALUE, "radius");
  }
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
  stats.dabs++;
  dabMoved_.clear();
  dabGrids_.clear();
  dabLeaves_.clear();
  nodePtrs_.clear();
  if (radius > 0 && queryDabLeaves(false,
                                   radius,
                                   origin,
                                   command.unbounded || command.grabMode ||
                                       resolvedFalloffNeedsAllNodes(*brush)))
  {
    updateStrokeFrame(origin);
    brush->pushStrokeSample(origin, normal);
    dabSeq_++;
    ScopedPreparedCavity cavityScope(preparedCavityActive);
    execStage(command, origin, normal);
    finishDab();
  }
  isFirstOfStep = false;
  return {};
}

props::ScalarRegistrationResult GridBrushExecutor::applyResolvedProgram(
    BrushProgram *program, float3 origin, float3 normal, bool validateOnly, bool grabAdd)
{
  using props::PropError;
  lastRegistration = resolvedState(origin, normal);
  if (lastRegistration.error != PropError::ERROR_NONE)
    return lastRegistration;
  if (!program) {
    lastRegistration = {PropError::ERROR_INVALID_OWNER, "program"};
    return lastRegistration;
  }
  if (!program->commands.size()) {
    PreparedBrushScalars empty;
    lastRegistration = prepareBrushScalars(*brush, {}, brush->deviceInputCtx, empty);
    if (lastRegistration.error != PropError::ERROR_NONE || validateOnly)
      return lastRegistration;
    stats.dabs++;
    dabMoved_.clear();
    dabGrids_.clear();
    dabLeaves_.clear();
    nodePtrs_.clear();
    isFirstOfStep = false;
    return {};
  }
  Vector<brush_command> commands;
  commands.resize(program->commands.size());
  Vector<std::span<const BrushUniformManifestEntry>> manifests;
  for (size_t i = 0; i < program->commands.size(); i++) {
    const auto &entry = program->commands[i];
    auto &command = commands[i];
    if (!createPreparedCommand(entry.type, command)) {
      lastRegistration = programError(i, {PropError::ERROR_NOT_EXISTS, "kernel"});
      return lastRegistration;
    }
    if (entry.attrLayerOverrides.size() || !resolvedCapability(command, entry.type)) {
      lastRegistration = programError(
          i, {PropError::ERROR_INVALID_VALUE, "unsupported resolved capability"});
      return lastRegistration;
    }
    if (command.grabModeCapable && !validGrabFrame(*brush)) {
      lastRegistration = programError(i, {PropError::ERROR_INVALID_VALUE, "grab frame"});
      return lastRegistration;
    }
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
  bool allLeaves = resolvedFalloffNeedsAllNodes(*brush);
  for (size_t i = 0; i < commands.size(); i++) {
    allLeaves |= commands[i].grabMode && prepared.radii[i] > 0;
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
  setGrabAccumAdd(grabAdd);
  stats.dabs++;
  dabMoved_.clear();
  dabGrids_.clear();
  dabLeaves_.clear();
  nodePtrs_.clear();
  if (prepared.radius > 0 && queryDabLeaves(false, prepared.radius, origin, allLeaves)) {
    ScopedBrushWorkingValues working(*brush,
                                     {prepared.stages.data(), prepared.stages.size()});
    updateStrokeFrame(origin);
    brush->pushStrokeSample(origin, normal);
    dabSeq_++;
    ScopedPreparedCavity cavityScope(preparedCavityActive);
    for (size_t i = 0; i < commands.size(); i++) {
      if (prepared.radii[i] == 0)
        continue;
      prepared.stages[i].applyWorking(*brush);
      execStage(commands[i], origin, normal);
    }
    finishDab();
  }
  isFirstOfStep = false;
  return {};
}

} // namespace sculptcore::brush
