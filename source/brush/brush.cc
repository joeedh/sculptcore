#include "brush/brush.h"
#include "brush/brushes/extra.h"
#include <cstring>

namespace sculptcore::brush {

const NamedUniformDescriptor *extraNamedUniformDescriptor(props::Prop type, int slot)
{
  return generatedExtraNamedUniformDescriptor(type, slot);
}

const NamedUniformDescriptor *extraNamedUniformDescriptor(const util::string &name,
                                                          int &slot)
{
  slot = -1;
  for (auto type : {props::Prop::FLOAT32, props::Prop::INT32, props::Prop::BOOL}) {
    for (int index = 0; index < kNamedUniformSlotLimit; index++) {
      auto *descriptor = generatedExtraNamedUniformDescriptor(type, index);
      if (!descriptor) {
        break;
      }
      if (std::strcmp(name.c_str(), descriptor->name) == 0) {
        slot = index;
        return descriptor;
      }
    }
  }
  return nullptr;
}

props::PropError Brush::setNamedScalar(props::Prop type, int slot, double value)
{
  using props::Prop;
  using props::PropError;
  if (slot < 0 || slot >= kNamedUniformSlotLimit) {
    return PropError::ERROR_INVALID_VALUE;
  }
  auto write = [&]<typename T>(NamedUniformStore<T> &store) {
    return named_uniform_detail::StoreAccess::write(
        store, props, extraNamedUniformDescriptor(type, slot), type, slot, value);
  };
  switch (type) {
  case Prop::FLOAT32:
    return write(namedFloats);
  case Prop::INT32:
    return write(namedInts);
  case Prop::BOOL:
    return write(namedBools);
  default:
    return PropError::ERROR_INVALID_TYPE;
  }
}

static void reportNamedWrite(props::PropError error, int slot)
{
  if (error != props::PropError::ERROR_NONE) {
    fprintf(stderr, "named uniform slot %d: write error %d\n", slot, int(error));
  }
}

void Brush::setNamedFloat(int slot, float value)
{
  reportNamedWrite(setNamedScalar(props::Prop::FLOAT32, slot, value), slot);
}
void Brush::setNamedInt(int slot, int32_t value)
{
  reportNamedWrite(setNamedScalar(props::Prop::INT32, slot, value), slot);
}
void Brush::setNamedBool(int slot, bool value)
{
  reportNamedWrite(setNamedScalar(props::Prop::BOOL, slot, value), slot);
}

  /** Describe native member storage and semantic dynamics eligibility. */

} // namespace sculptcore::brush
