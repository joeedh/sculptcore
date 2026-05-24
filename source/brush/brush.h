#pragma once
#include "../props/prop_dynamics.h"
#include "../props/prop_struct.h"
#include "litestl/binding/binding.h"
#include "litestl/math/vector.h"
#include "litestl/util/compiler_util.h"

#include <cmath>

#include "props.h"

namespace sculptcore::brush {
using litestl::util::StrLiteral;
using litestl::math::float3;

// Falloff curve shapes selectable per brush. All three are analytic
// (no LUT yet) so the WGSL emit can stay branch-only without storage
// buffers. `t` is the normalized 0..1 centerwise input — 1 at the brush
// center, 0 at the radius. Smoothstep is the historical default and
// keeps the regression dumps bit-identical when no `set_falloff` runs.
//
// `Spherical{ Curve1D }` from the design doc lands in a later slice
// once a Curve LUT buffer is plumbed through the uniform-block path.
enum class FalloffKind : unsigned char {
  Smoothstep = 0,
  Linear = 1,
  Gaussian = 2,
};

struct Brush {
  props::StructProp props;
  props::DeviceInputCtx deviceInputCtx;

  float strength = 1;
  float radius = 1;
  /* Fraction of `radius` between successive brush dabs along a stroke. */
  float spacing = 0.25f;
  bool invert = false;
  FalloffKind falloff_kind = FalloffKind::Smoothstep;

  // Kelvinlet brush uniforms — Lamé-style material constants. Live on Brush
  // (rather than only on CommandCtx) because they're authored alongside
  // strength/radius. Defaults match the kelvinlet paper's "soft rubber".
  float mu = 1.0f;
  float nu = 0.4f;

  // Grab-style ctx state (kelvinlet, future pose). `grabFrom` is the stroke
  // origin captured at the start of the dab; `grabTo` is the current cursor.
  float3 grabFrom{0, 0, 0};
  float3 grabTo{0, 0, 0};

  // Pose-brush cage. `poseCageRest` is sampled when the dab starts; the DSL
  // displaces each vertex by a weighted sum of `poseCageNow[i] - poseCageRest[i]`
  // with weights = 1 / (1 + |v.co - poseCageRest[i]|²). Four anchors is the
  // tightest fit that still gives a smooth pose without needing a loop in
  // the DSL (Wave 4b has no `for`).
  float3 poseCageRest[4] = {};
  float3 poseCageNow[4] = {};

  static litestl::binding::types::Struct<Brush> *defineBindings()
  {
    using namespace litestl::binding;
    types::Struct<Brush> *st =
        new types::Struct<Brush>("sculptcore::brush::Brush", sizeof(Brush));

    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);

    BIND_STRUCT_MEMBER(st, strength);
    BIND_STRUCT_MEMBER(st, radius);
    BIND_STRUCT_MEMBER(st, spacing);
    BIND_STRUCT_MEMBER(st, invert);
    BIND_STRUCT_MEMBER(st, mu);
    BIND_STRUCT_MEMBER(st, nu);
    BIND_STRUCT_MEMBER(st, grabFrom);
    BIND_STRUCT_MEMBER(st, grabTo);
    BIND_STRUCT_MEMBER(st, props);
    BIND_STRUCT_METHOD(st, loadProps, MARGS());
    BIND_STRUCT_METHOD(st, writeProps, MARGS());

    return st;
  }

  Brush() : props(&structDef_)
  {
    structDef_.Float32("strength", "strength");
    structDef_.Float32("radius", "radius");
    structDef_.Float32("spacing", "spacing");
    structDef_.Bool("invert", "invert");
    structDef_.Float32("mu", "mu");
    structDef_.Float32("nu", "nu");
  }

  void loadProps()
  {
    strength = props.lookupValue<float>("strength", 1.0);
    radius = props.lookupValue<float>("radius", 1.0);
    spacing = props.lookupValue<float>("spacing", 0.25);
    invert = props.lookupValue<bool>("invert", false);
    mu = props.lookupValue<float>("mu", 1.0);
    nu = props.lookupValue<float>("nu", 0.4);
  }

  void writeProps()
  {
    props.setValue<float>("strength", strength);
    props.setValue<float>("radius", radius);
    props.setValue<float>("spacing", spacing);
    props.setValue<bool>("invert", invert);
    props.setValue<float>("mu", mu);
    props.setValue<float>("nu", nu);
  }

  // Evaluate the active falloff curve at normalized centerwise `t`
  // (1 at center, 0 at radius). Source of truth for the C++ side; the
  // WGSL emitter mirrors the same three branches in `brush_falloff`.
  // The Gaussian width (9 in the exponent) hits exp(-9) ~= 1.2e-4 at
  // the edge, so no hard cutoff is needed.
  float falloffEval(float t) const
  {
    switch (falloff_kind) {
    case FalloffKind::Smoothstep:
      return t * t * (3.0f - 2.0f * t);
    case FalloffKind::Linear:
      return t;
    case FalloffKind::Gaussian: {
      float u = 1.0f - t;
      return std::exp(-9.0f * u * u);
    }
    }
    return t;
  }

private:
  props::StructDef structDef_;
};
} // namespace sculptcore::brush
