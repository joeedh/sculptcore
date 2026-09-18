#include "semantic_scalars.h"
#include "brush_configuration.h"
#include "litestl/util/alloc.h"
#include "props/prop_coerce.h"

#include <cstring>
#include <limits>

namespace sculptcore::brush {
using props::Prop;
using props::PropError;

int SemanticScalars::add(
    int type, double value, double minimum, double maximum, int dynamic)
{
  if ((dynamic != 0 && dynamic != 1) || entries_.size() >= size_t(INT32_MAX))
    return -int(PropError::ERROR_INVALID_VALUE);
  props::ScalarDeclaration declaration{
      "semantic", Prop(type), true, value, true, minimum, maximum, bool(dynamic)};
  props::ScalarDomain domain;
  auto error = props::normalizeScalarDeclaration(declaration, domain);
  if (error != PropError::ERROR_NONE)
    return -int(error);
  entries_.append(Entry{Prop(type), domain, bool(dynamic), {}});
  return int(entries_.size()) - 1;
}

int SemanticScalars::replace(int index, const props::Dynamics &dynamics)
{
  if (index < 0 || size_t(index) >= entries_.size())
    return int(PropError::ERROR_NOT_EXISTS);
  auto &entry = entries_[index];
  if ((!entry.dynamic && dynamics.devices.size()) ||
      !props::Dynamics::validStack(dynamics.devices))
    return int(PropError::ERROR_INVALID_DYNAMICS);
  entry.dynamics = dynamics;
  return 0;
}

int SemanticScalars::evaluate(int count,
                              const float *inputs,
                              const float *radii,
                              int radiusIndex,
                              double *output,
                              size_t outputCount) const
{
  if (count < 0 || radiusIndex < -1 ||
      (radiusIndex >= 0 && (size_t(radiusIndex) >= entries_.size() ||
                            entries_[radiusIndex].type != Prop::FLOAT32)) ||
      (count && (!inputs || (radiusIndex >= 0 && !radii))) ||
      (entries_.size() &&
       size_t(count) > std::numeric_limits<size_t>::max() / entries_.size()))
    return int(PropError::ERROR_INVALID_VALUE);
  const size_t required = size_t(count) * entries_.size();
  if (outputCount != required || (required && !output))
    return int(PropError::ERROR_INVALID_VALUE);
  util::Vector<double> candidate;
  candidate.resize(required);
  for (int row = 0; row < count; row++) {
    const float mask = inputs[size_t(row) * 5 + 4];
    if (!std::isfinite(mask) || mask < 0 || mask > 15 || std::trunc(mask) != mask)
      return int(PropError::ERROR_INVALID_VALUE);
    props::DeviceInputCtx context;
    for (int device = 0; device < 4; device++)
      if (int(mask) & (1 << device))
        context.push(device, inputs[size_t(row) * 5 + device]);
    for (size_t index = 0; index < entries_.size(); index++) {
      const auto &entry = entries_[index];
      const double base =
          int(index) == radiusIndex ? double(radii[row]) : entry.domain.initial;
      if (!std::isfinite(base) || base < entry.domain.min || base > entry.domain.max)
        return int(PropError::ERROR_INVALID_VALUE);
      bool valid = false;
      props::numtype_dispatch(entry.type, [&]<class P>() {
        using T = typename P::value_type;
        T result{};
        valid = entry.dynamics.evaluateChecked(
            T(base), T(entry.domain.min), T(entry.domain.max), context, result);
        if (valid)
          candidate[size_t(row) * entries_.size() + index] = double(result);
      });
      if (!valid)
        return int(PropError::ERROR_INVALID_DYNAMICS);
    }
  }
  if (required)
    std::memcpy(output, candidate.data(), required * sizeof(double));
  return 0;
}
} // namespace sculptcore::brush

using sculptcore::brush::SemanticScalars;
using sculptcore::props::PropError;

extern "C" {
SemanticScalars *SemanticScalars_create()
{
  return litestl::alloc::New<SemanticScalars>("semantic scalar collection");
}

void SemanticScalars_free(SemanticScalars *collection)
{
  if (collection)
    litestl::alloc::Delete(collection);
}

int SemanticScalars_add(SemanticScalars *collection,
                        int type,
                        double value,
                        double minimum,
                        double maximum,
                        int dynamic)
{
  return collection ? collection->add(type, value, minimum, maximum, dynamic)
                    : -int(PropError::ERROR_INVALID_OWNER);
}

int SemanticScalars_replaceDynamics(SemanticScalars *collection,
                                    int index,
                                    util::Vector<int> *devices,
                                    util::Vector<int> *modes,
                                    util::Vector<float> *factors,
                                    util::Vector<int> *enabled,
                                    util::Vector<int> *offsets,
                                    util::Vector<float> *samples,
                                    util::Vector<int> *kinds,
                                    util::Vector<double> *parameters)
{
  if (!collection || !devices || !modes || !factors || !enabled || !offsets || !samples ||
      !kinds || !parameters)
    return int(PropError::ERROR_INVALID_OWNER);
  sculptcore::props::Dynamics candidate;
  int status = sculptcore::brush::decodeResponseDynamics(*devices,
                                                         *modes,
                                                         *factors,
                                                         *enabled,
                                                         *offsets,
                                                         *samples,
                                                         *kinds,
                                                         *parameters,
                                                         candidate);
  return status ? status : collection->replace(index, candidate);
}

int SemanticScalars_evaluate(const SemanticScalars *collection,
                             int count,
                             const float *inputs,
                             const float *radii,
                             int radiusIndex,
                             double *output,
                             size_t outputCount)
{
  return collection ? collection->evaluate(
                          count, inputs, radii, radiusIndex, output, outputCount)
                    : int(PropError::ERROR_INVALID_OWNER);
}
}
