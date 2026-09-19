#include "brush/brush_executor.h"

namespace sculptcore::brush {

  CommandExecutor::brush_command CommandExecutor::createCommand(SculptBrushes brushType)
  {
    brush_command def;
    createCommandImpl<AccumLive>(brushType, def);
    if (def.grabModeCapable && anchoredGrab) {
      // Always deform from each vert's stroke-start position, so the region is
      // fixed at stroke start and the grab follows the cursor (#35). One write-
      // back (AccumOrigGrab) serves every symmetry image: the first image to
      // touch a vert this dab re-bases it from orig, later images of the same
      // dab add their displacement onto it (arbitrated by the per-vert dab
      // stamp). Forced on regardless of the ACCUMULATE flag. The op marks each
      // image via setGrabAccumAdd, which advances the per-dab generation on the
      // primary.
      def.grabMode = true;
      /* The second impl call re-appends the same uniform + attr manifests â€”
         clear them first or grab-class brushes report every entry twice (the
         wave-5 queryUniformManifest / queryAttrManifest bridge). */
      def.uniforms = decltype(def.uniforms)();
      def.attrs = decltype(def.attrs)();
      createCommandImpl<AccumOrigGrab>(brushType, def);
    } else if (nonAccum && def.accumulable && !def.relaxesBase) {
      def.uniforms = decltype(def.uniforms)();
      def.attrs = decltype(def.attrs)();
      createCommandImpl<AccumOrig>(brushType, def);
    }
    return def;
  }

  void CommandExecutor::ensureToolMemo(SculptBrushes brushType)
  {
    if (int(brushType) != floorMemoTool_) {
      brush_command def;
      createCommandImpl<AccumLive>(brushType, def);
      floorMemoTool_ = int(brushType);
      floorMemoUnbounded_ = def.unbounded;
      floorMemoGrabCapable_ = def.grabModeCapable;
    }
  }

  void CommandExecutor::grabFilterNodes(float3 center,
                       float pinRadius,
                       float hostRadius,
                       Vector<spatial::SpatialNode *> &nodes)
  {
    if (stepHasDyntopo) {
      // Topology and the leaf set move under the stroke, so nothing survives
      // being pinned; fall back to the drag-widened filter.
      grabWidenRadius_ = std::fmax(
          grabWidenRadius_, std::fmax(hostRadius, pinRadius + brush->grabTo.length()));
      tree->filterNodes(center, grabWidenRadius_, nodes);
      return;
    }

    GrabRegion *reg = nullptr;
    const float tol = std::fmax(pinRadius, 1.0f) * 1e-4f;
    for (GrabRegion &r : grabRegions_) {
      if ((r.center - center).lengthSqr() <= tol * tol) {
        reg = &r;
        break;
      }
    }
    // Query new support and retain moved leaves from every symmetry image.
    // Radius growth must not discard leaves that already left the anchor.
    tree->filterNodes(center, pinRadius, nodes);
    util::Set<int> seen;
    for (auto *node : nodes)
      seen.add(node->id);
    for (const auto &region : grabRegions_)
      for (int id : region.nodeIds)
        if (!seen.contains(id))
          if (auto *node = tree->node_from_id(id)) {
            nodes.append(node);
            seen.add(id);
          }
    if (!reg) {
      // One entry per symmetry image; more than that means the "anchor" is
      // drifting, so stop caching rather than grow without bound.
      if (grabRegions_.size() >= 16) {
        return;
      }
      grabRegions_.append(GrabRegion());
      reg = &grabRegions_[grabRegions_.size() - 1];
    }
    reg->center = center;
    reg->radius = pinRadius;
    reg->nodeIds.clear();
    for (spatial::SpatialNode *n : nodes) {
      reg->nodeIds.append(n->id);
    }
  }

} // namespace sculptcore::brush
