#include "brush/brush_executor.h"

namespace sculptcore::brush {

  void CommandExecutor::setRenderMatrix(Vector<float> &m16)
  {
    if (m16.size() != 16) {
      return;
    }
    float *dst = &ctx.renderMatrix[0][0];
    for (int i = 0; i < 16; i++) {
      dst[i] = m16[i];
    }
  }

  void CommandExecutor::setGrabAccumAdd(bool v)
  {
    grabAccumAdd = v;
    if (!v) {
      dabGen++;
    }
  }

  void CommandExecutor::beginStep(bool hasDyntopo)
  {
    preparedCavity.reset();
    preparedEnhance.reset();
    preparedStepOpen_ = true;
    preparedPreviewStroke_ = false;
    preparedPreviewClosed_ = false;
    preparedStepTree_ = tree;
    preparedStepMesh_ = tree ? tree->m : nullptr;
    preparedStepBrush_ = brush;
    preparedStepLog_ = meshLog;
    isFirstOfStep = true;
    strokeValidationFailed = false;
    stepHasDyntopo = hasDyntopo;
    /* Positions may have changed since the last stroke (undo, ops, other
     * tools) â€” force the stroke's first needsCoPrev exec to take a full
     * snapshot. The gen bump keeps stale node stamps from suppressing
     * dirty-list appends. */
    coPrevFull_ = false;
    coPrevDirty_.clear();
    coPrevGen_++;
    uvReprojPending_.clear();
    grabRegions_.clear();
    grabWidenRadius_ = 0.0f;
    planeFrameState_.reset();
    imageSign_ = float3(1.0f, 1.0f, 1.0f);
    imageIsMirror_ = false;
    if (brush) {
      brush->resetStrokePath();
    }
    if (meshLog) {
      meshLog->beginStep(hasDyntopo);
    }
    preparedStepId_ = meshLog ? meshLog->lastStepId() : -1;
  }

  void CommandExecutor::endStep()
  {
    preparedStepOpen_ = false;
    isFirstOfStep = false;
    /* Flush the stroke's deferred UV reprojection while the step is still
     * open, so the corner captures land inside it. */
    if (uvReprojPending_.size() > 0 && tree && tree->m) {
      Vector<int> rverts;
      Vector<float3> rold;
      for (const auto &pair : uvReprojPending_) {
        rverts.append(pair.key);
        rold.append(pair.value);
      }
      reprojectUvsWithCapture(tree->m,
                              std::span<const int>(rverts.data(), rverts.size()),
                              std::span<const float3>(rold.data(), rold.size()));
    }
    uvReprojPending_.clear();
    if (meshLog) {
      meshLog->endStep();
    }
  }

} // namespace sculptcore::brush
