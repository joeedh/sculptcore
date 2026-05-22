#pragma once

#include "litestl/math/vector.h"

namespace sculptcore::brush {

/* Walks polyline segments and emits dabs at constant world-space
 * intervals. Carries a residual across calls so dabs stay evenly spaced
 * even when consecutive `advance` segments are shorter than one step.
 *
 * Usage:
 *   StrokeSpacer s{radius * brush.spacing};
 *   s.advance(p0, emit);   // first call always emits p0
 *   s.advance(p1, emit);   // emits zero or more points on (p0, p1]
 *   ...
 */
struct StrokeSpacer {
  float spacing = 0.0f;
  litestl::math::float3 last_pos{0, 0, 0};
  bool has_last = false;
  /* Distance already "spent" past the last emitted dab. */
  float residual = 0.0f;

  template <typename Emit> void advance(litestl::math::float3 p, Emit &&emit)
  {
    if (!has_last) {
      has_last = true;
      last_pos = p;
      emit(p);
      return;
    }
    if (spacing <= 0.0f) {
      emit(p);
      last_pos = p;
      return;
    }
    litestl::math::float3 d = p - last_pos;
    float len = d.length();
    if (len < 1e-7f) {
      return;
    }
    litestl::math::float3 dir = d * (1.0f / len);
    /* `walked` is the position along the segment of the next candidate
     * dab; the first one falls `spacing - residual` past `last_pos`. */
    float walked = spacing - residual;
    while (walked <= len) {
      emit(last_pos + dir * walked);
      walked += spacing;
    }
    residual = len - (walked - spacing);
    last_pos = p;
  }
};

} // namespace sculptcore::brush
