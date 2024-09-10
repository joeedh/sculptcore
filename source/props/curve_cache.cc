#include "prop_curve.h"

#include "litestl/util/hash.h"
#include "litestl/util/map.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"
#include "litestl/util/memory.h"

using namespace litestl;
using namespace litestl::util;
using litestl::hash::HashInt;

namespace sculptcore::props::detail::curve {
Vector<util::weak_ptr<CurveGen>> curves_cache;

/* Returns curve inside inside cache if one exists and deletes existing,
 * otherwise inserts curve inside cache.
 */
util::shared_ptr<CurveGen> get_curve(CurveGen *existing)
{
  HashInt hash = existing->hash();

  for (auto &ptr : curves_cache) {
    if (ptr.expired()) {
      continue;
    }

    util::shared_ptr<CurveGen> curve = ptr.lock();
    if (curve->hash() == hash && curve->operator==(*existing)) {
      if (existing != curve.get()) {
        alloc::Delete<CurveGen>(existing);
      }

      return curve;
    }
  }

  util::shared_ptr<CurveGen> shared = util::shared_ptr<CurveGen>(existing);
  util::weak_ptr<CurveGen> weak = shared;

  curves_cache.append(weak);

  return shared;
}

} // namespace sculptcore::props::detail::curve
