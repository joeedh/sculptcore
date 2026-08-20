// The per-tool host-pass table. Every "this brush needs host work around its
// kernel" fact is one row here, invoked blindly by the executors (see
// brush_hooks.h). The one per-tool residue left outside this table is
// gpu_marshal.cc's ctx tail (KELVINLET/GRAB/POSE), kept deliberately.
#include "brush_hooks.h"

#include "brush/brush.h"
#include "brush/brush_executor.h"
#include "brush/enhance.h"
#include "brush/feature_field.h"

#include <span>

namespace sculptcore::brush {

using litestl::util::Vector;

static void collectRegionVerts(BrushHookCtx &c, Vector<int> &out)
{
  for (spatial::SpatialNode *node : *c.nodes) {
    for (int v : node->data->unique_verts) {
      out.append(v);
    }
  }
}

/** BSMOOTH / FEATURE_ALIGN both read the lazily-derived `.boundary.vert.class`,
 * so it is refreshed once at stroke start, while topology links are live. */
static void hookBoundaryClassRefresh(BrushHookCtx &c)
{
  c.exec.refreshBoundaryClassForBSmooth(c.m);
}

/** ENHANCE: fill the cached per-vertex difference-of-smooths displacement over
 * the dab region before the kernel reads `.brush.enhance.disp`. Cached per
 * stroke (keyed by strokeGen); topology is live (brushNeedsLiveLinks). */
static void hookEnhanceRegion(BrushHookCtx &c)
{
  Vector<int> regionVerts;
  collectRegionVerts(c, regionVerts);
  EnhanceParams ep;
  ep.rings = c.brush->enhance_rings;
  ep.inner = c.brush->enhance_inner;
  updateEnhanceRegion(*c.m, regionVerts, ep, c.exec.strokeGen);
}

/** FEATURE_ALIGN: (re)seed + diffuse the per-vertex cross field over the dab's
 * region so the kernel reads an up-to-date field. Incremental — only the
 * region's verts are written, so the saved field grows as the stroke covers
 * the mesh. Topology is live (brushNeedsLiveLinks). */
static void hookCrossFieldRegion(BrushHookCtx &c)
{
  Vector<int> regionVerts;
  collectRegionVerts(c, regionVerts);
  FeatureFieldParams ffParams;
  updateCrossFieldRegion(*c.m, regionVerts, ffParams);
}

/** POLYGROUP: mark the touched nodes' faces boundary-dirty so the next
 * recomputeDirty reclassifies their inter-group edges. */
static void hookPolygroupDirty(BrushHookCtx &c)
{
  std::span<spatial::SpatialNode *> nodeSpan(c.nodes->data(), c.nodes->size());
  c.exec.markPolygroupDirty(nodeSpan);
}

const BrushHooks *brushHooksFor(SculptBrushes tool)
{
  static const BrushHooks bsmooth = {hookBoundaryClassRefresh, nullptr, nullptr};
  static const BrushHooks featureAlign = {
      hookBoundaryClassRefresh, hookCrossFieldRegion, nullptr};
  static const BrushHooks enhance = {nullptr, hookEnhanceRegion, nullptr};
  static const BrushHooks polygroup = {nullptr, nullptr, hookPolygroupDirty};

  switch (tool) {
  case SculptBrushes::BSMOOTH:
    return &bsmooth;
  case SculptBrushes::FEATURE_ALIGN:
    return &featureAlign;
  case SculptBrushes::ENHANCE:
    return &enhance;
  case SculptBrushes::POLYGROUP:
    return &polygroup;
  default:
    return nullptr;
  }
}

}  // namespace sculptcore::brush
