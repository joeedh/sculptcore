#pragma once

#include "props/prop_struct.h"
#include <array>

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::brush {
struct Brush;
struct BrushUniformManifestEntry;
struct BrushAttrManifestEntry;
struct BrushScalarOverride;
struct BrushDynamicsOverride;
struct BrushAttrLayerOverride;
enum class ScalarSource { Authored, RawStandalone };

/** Validate falloff configuration before preparing or publishing a dab. */
bool resolvedFalloffSupported(const Brush &brush);
bool resolvedFalloffNormalSupported(const Brush &brush, float x, float y, float z);

/** Mesh execution supports materialized numeric attributes; grids stay restricted. */
bool resolvedAttributesSupported(std::span<const BrushAttrManifestEntry> attributes,
                                 bool enhance = false,
                                 bool meshDomain = false);
props::ScalarRegistrationResult
validatePreparedAttributes(mesh::Mesh &m,
                           std::span<const BrushAttrManifestEntry> attributes,
                           std::span<const BrushAttrLayerOverride> overrides,
                           util::Vector<BrushAttrManifestEntry> &planned);

/** Static executor settings are native-backed and never published as DSL properties. */
std::span<const props::ScalarDeclaration> executorSettingDeclarations();
bool executorSetting(const util::string &name);

struct PreparedBrushScalar {
  util::string name;
  props::Prop type = props::Prop::INVALID_TYPE;
  double value = 0;
  bool dynamic = false;
  int nativeMember = -1;
  int storeSlot = -1;
};

/** Read-only candidate snapshot, not an execution or publication token. */
class PreparedBrushScalars {
public:
  std::span<const PreparedBrushScalar> values() const
  {
    return {values_.size() ? &values_[0] : nullptr, values_.size()};
  }
  std::span<const props::ScalarDeclaration> declarations() const
  {
    return {declarations_.size() ? &declarations_[0] : nullptr, declarations_.size()};
  }
  const Brush *sourceBrush() const
  {
    return brush_;
  }
  const props::StructDef *sourceDefinition() const
  {
    return definition_;
  }
  const std::array<float, 256> &cavityCurve() const
  {
    return cavityCurve_;
  }
  float unboundedExtent() const
  {
    return unboundedExtent_;
  }

private:
  friend struct CommandExecutor;
  friend struct GridBrushExecutor;
  friend struct CageSmoothSession;
  /** Only for synchronous prepare/check/publish in the executor. No raw edits,
   * callbacks or yield points may intervene; this is not a reusable token. */
  props::ScalarRegistrationResult publish(Brush &brush) const;
  void applyWorking(Brush &brush) const;
  friend props::ScalarRegistrationResult
  prepareBrushScalars(Brush &,
                      std::span<const BrushUniformManifestEntry>,
                      const props::DeviceInputCtx &,
                      PreparedBrushScalars &,
                      std::span<const BrushScalarOverride>,
                      std::span<const BrushUniformManifestEntry>,
                      ScalarSource,
                      std::span<const BrushDynamicsOverride>,
                      std::span<const float>);
  const Brush *brush_ = nullptr;
  const props::StructDef *definition_ = nullptr;
  util::Vector<PreparedBrushScalar> values_;
  util::Vector<props::ScalarDeclaration> declarations_;
  std::array<float, 256> cavityCurve_{};
  // Context snapshot; absence from the manifest does not authorize an override.
  float unboundedExtent_ = 0;
};

bool validGrabFrame(const Brush &brush);

/** Validate actual float cutoff arithmetic before selection or validateOnly. */
props::ScalarRegistrationResult
validateUnboundedSupport(const PreparedBrushScalars &stage, float radius);

/** Use fresh scratch-factory manifests, never caller-writable query snapshots.
 * Does not publish declarations, initialize slots, invoke getters or change owners.
 * Preserves output on failure. Raw edits after preparation invalidate the snapshot. */
props::ScalarRegistrationResult
prepareBrushScalars(Brush &brush,
                    std::span<const BrushUniformManifestEntry> uniforms,
                    const props::DeviceInputCtx &inputs,
                    PreparedBrushScalars &output,
                    std::span<const BrushScalarOverride> overrides = {},
                    std::span<const BrushUniformManifestEntry> programUniforms = {},
                    ScalarSource source = ScalarSource::Authored,
                    std::span<const BrushDynamicsOverride> dynamicsOverrides = {},
                    std::span<const float> cavityCurveOverride = {});

} // namespace sculptcore::brush
