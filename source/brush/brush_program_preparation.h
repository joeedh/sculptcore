#pragma once

#include "brush_preparation.h"
#include "brush_program.h"
#include <array>

namespace sculptcore::brush {

struct PreparedProgramScalars {
  util::Vector<PreparedBrushScalars> stages;
  util::Vector<float> radii;
  float radius = 0;
};

props::ScalarRegistrationResult programError(size_t index,
                                             props::ScalarRegistrationResult error);

/** Prepares every scratch-factory manifest without changing the brush or output on
 * failure. */
props::ScalarRegistrationResult prepareProgramScalars(
    Brush &brush,
    const BrushProgram &program,
    std::span<const std::span<const BrushUniformManifestEntry>> manifests,
    const props::DeviceInputCtx &inputs,
    PreparedProgramScalars &output);

/** Restores sparse working values and slot presence; authored properties never change. */
class ScopedBrushWorkingValues {
public:
  ~ScopedBrushWorkingValues();
  ScopedBrushWorkingValues(const ScopedBrushWorkingValues &) = delete;
  ScopedBrushWorkingValues &operator=(const ScopedBrushWorkingValues &) = delete;

private:
  friend struct CommandExecutor;
  friend struct GridBrushExecutor;
  friend struct CageSmoothSession;
  ScopedBrushWorkingValues(Brush &brush,
                           std::span<const PreparedBrushScalars> stages,
                           bool executorOnly = false);
  struct Saved {
    props::Prop type;
    int member, slot;
    std::array<unsigned char, 4> bytes{};
    bool initialized = false;
  };
  Brush &brush_;
  bool executorOnly_;
  util::Vector<Saved> saved_;
  size_t floatSize_, intSize_, boolSize_;
  std::array<float, 256> cavityCurve_;
  float unboundedExtent_;
};

} // namespace sculptcore::brush
