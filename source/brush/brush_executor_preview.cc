#include "brush/brush_executor.h"

namespace sculptcore::brush {

  bool CommandExecutor::preparedStepValid() const
  {
    return preparedStepOpen_ && tree == preparedStepTree_ && tree &&
           tree->m == preparedStepMesh_ && brush == preparedStepBrush_ &&
           meshLog == preparedStepLog_ &&
           (!meshLog || meshLog->hasOpenStepFor(preparedStepMesh_, preparedStepId_));
  }

  void CommandExecutor::capturePreparedPreview()
  {
    if (!previewActive())
      return;
    preparedPreviewStroke_ = true;
    meshLog->capturePreviewNodes(tree->m, {dabNodes_.data(), dabNodes_.size()});
  }

  void CommandExecutor::beginPreviewDab(float3 center, float radius)
  {
    if (!meshLog || !tree) {
      return;
    }
    meshLog->beginPreviewDab(tree->m, tree, center, radius);
  }

  void CommandExecutor::extendPreviewDab(float3 center, float radius)
  {
    if (!meshLog || !tree) {
      return;
    }
    meshLog->extendPreviewDab(tree->m, tree, center, radius);
  }

  void CommandExecutor::rollbackPreviewDab()
  {
    if (!meshLog || !tree) {
      return;
    }
    const bool prepared = preparedPreviewStroke_ && previewActive();
    meshLog->rollbackPreviewDab(tree->m, tree);
    if (prepared) {
      for (auto &ref : tree->m->v.attrs.attrs) {
        if (ref.type != mesh::AttrType::INT || (ref.name != string(".brush.disp.gen") &&
                                                ref.name != string(".brush.dab.gen")))
          continue;
        for (int v : meshLog->preview_.vertIdx)
          if (auto *stamp = static_cast<int *>(ref.data->getElemData(v)))
            *stamp = 0;
      }
      for (auto *node : tree->leaves()) {
        node->baseStampGen = 0;
        node->baseStampOpts = -1;
      }
      preparedCavity.reset();
      preparedEnhance.reset();
      coPrevFull_ = false;
      coPrevDirty_.clear();
      coPrevGen_++;
      uvReprojPending_.clear();
      brush->resetStrokePath();
      isFirstOfStep = true;
    }
  }

  void CommandExecutor::commitPreviewDab()
  {
    if (!meshLog) {
      return;
    }
    if (preparedPreviewStroke_ && previewActive())
      preparedPreviewClosed_ = true;
    meshLog->commitPreviewDab();
  }

} // namespace sculptcore::brush
