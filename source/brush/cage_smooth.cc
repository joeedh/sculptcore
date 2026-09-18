#include "brush/cage_smooth.h"
#include "brush/brush_program_preparation.h"

namespace sculptcore::brush {

bool CageSmoothSession::supportsResolved(int tool)
{
  auto *slot = mr ? mr->findSlot(level) : nullptr;
  if (!begun_ || !brush || !mr || mr->activeLevel() != level ||
      mr->domainGeneration() != domainGeneration_ || !slot || slot->mesh != slotMesh_ ||
      mr->gridAttrs().generation() != derivedGeneration_ ||
      (tool != int(SculptBrushes::COLOR) && tool != int(SculptBrushes::COLORSMOOTH)))
    return false;
  CommandExecutor::brush_command command;
  return exec.createPreparedCommand(SculptBrushes(tool), command) &&
         command.preparedScalarSafe && (!command.execHost || command.preparedHostNoop) &&
         resolvedFalloffSupported(*brush);
}

int CageSmoothSession::dabResolved(int tool,
                                   math::float3 center,
                                   math::float3 normal,
                                   bool validateOnly)
{
  using props::PropError;
  if (!supportsResolved(tool))
    return -1;
  for (int axis = 0; axis < 3; axis++)
    if (!std::isfinite(center[axis]) || !std::isfinite(normal[axis]))
      return -1;
  if (!resolvedFalloffNormalSupported(*brush, normal[0], normal[1], normal[2]))
    return -1;
  auto *cage = mr->cage();
  CommandExecutor::brush_command command;
  if (!cage || !exec.createPreparedCommand(SculptBrushes(tool), command))
    return -1;
  int layer = -1;
  for (int i = 0; i < int(cage->v.attrs.attrs.size()); i++) {
    const auto &attr = cage->v.attrs.attrs[i];
    if (attr.name == attrName && attr.type == mesh::AttrType::FLOAT4)
      layer = i;
  }
  if (layer < 0)
    return -1;
  Vector<BrushAttrLayerOverride> targets;
  for (int i = 0; i < int(command.attrs.size()); i++) {
    const auto &attr = command.attrs[i];
    if (attr.domain != AttrElemDomain::Vertex || attr.type != mesh::AttrType::FLOAT4 ||
        !(attr.use & int(mesh::AttrUse::COLOR)) || attr.boundName.size())
      return -1;
    targets.append({i, layer});
  }
  Vector<BrushAttrManifestEntry> planned;
  if (validatePreparedAttributes(*cage,
                                 {command.attrs.data(), command.attrs.size()},
                                 {targets.data(), targets.size()},
                                 planned)
          .error != PropError::ERROR_NONE)
    return -1;
  PreparedBrushScalars prepared;
  if (prepareBrushScalars(*brush,
                          {command.uniforms.data(), command.uniforms.size()},
                          brush->deviceInputCtx,
                          prepared)
          .error != PropError::ERROR_NONE)
    return -1;
  float radius = -1;
  for (const auto &value : prepared.values())
    if (value.name == string("radius"))
      radius = float(value.value);
  if (!std::isfinite(radius) || radius < 0 ||
      (radius > 0 && !std::isfinite(1.0f / radius)))
    return -1;
  if (validateOnly)
    return 0;
  ScopedBrushWorkingValues policy(*brush, {&prepared, 1}, true);
  if (prepared.publish(*brush).error != PropError::ERROR_NONE)
    return -1;
  brush->automask_cavity = false;
  brush->reproject_uvs = false;
  dabGridsOut_.clear();
  if (radius > 0) {
    const float row[4] = {center[0], center[1], center[2], radius};
    if (resolvedFalloffNeedsAllNodes(*brush)) {
      for (int grid = 0; grid < int(gridVert_.size()); grid++)
        dabGridsOut_.append(grid);
    } else {
      mr->dabGrids(level, row, 1, dabGridsOut_);
    }
  }
  if (!dabGridsOut_.size())
    return 0;
  node_.data->unique_verts = util::OrderedSet<int>();
  for (int grid : dabGridsOut_)
    node_.data->unique_verts.add(gridVert_[grid]);
  node_.affected_verts.clear();
  nodes_.clear();
  nodes_.append(&node_);
  if (exec.brushNeedsLiveLinks(SculptBrushes(tool)) || exec.keepTopoThawed) {
    if (cage->topo_frozen)
      cage->thawTopo();
  } else if (!cage->topo_frozen) {
    cage->freezeTopo();
  }
  exec.ctx.m = cage;
  exec.ctx.surfacePos = center;
  exec.ctx.surfaceNo = normal;
  exec.ctx.meshLog = nullptr;
  exec.ctx.isFirstOfStep = exec.isFirstOfStep;
  exec.updateStrokeFrame(center);
  brush->pushStrokeSample(center, normal);
  exec.setGrabAccumAdd(false);
  auto *coData = cage->v.co.get_data();
  std::swap(coData->pages, limitCo_.pages);
  exec.exec(command, {nodes_.data(), nodes_.size()}, {targets.data(), targets.size()});
  std::swap(coData->pages, limitCo_.pages);
  exec.clearIsFirstOfStep();
  if (node_.affected_verts.size())
    epilogue();
  return int(node_.data->unique_verts.size());
}

} // namespace sculptcore::brush
