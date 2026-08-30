// Tier 8b test: the named preset bundles (applyRemeshPreset).
//
//  - Every name in the remeshPresetName table must apply; unknown names must
//    fail and leave the params untouched.
//  - Applying a preset resets prior knob edits to the bundle, but preserves
//    the orthogonal sizing fields (target count / edge lengths) and seed.
//  - Tier-7 invariant: no preset sets feature_hysteresis without a
//    feature_min_chain to pair it with.
//  - One full-pipeline smoke run through a preset.
#include "test_util.h"

#include "litestl/util/alloc.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "remesh/remesh.h"
#include "remesh/remesh_params.h"

#include <cstdio>
#include <cstring>

test_init;

// The shared test_assert macro has a known retval=0-on-failure bug; use a local
// one that flips retval (mirrors test_remesh_curvature.cc).
#define TASSERT(expr)                                                                    \
  do {                                                                                   \
    if (!(expr)) {                                                                       \
      retval = 1;                                                                        \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                  \
      fflush(stderr);                                                                    \
    }                                                                                    \
  } while (0)

using namespace sculptcore;
using namespace sculptcore::mesh;

namespace {

void testNameTableAndUnknown()
{
  int n = 0;
  while (remesh::remeshPresetName(n)) {
    n++;
  }
  TASSERT(n == 7);
  TASSERT(remesh::remeshPresetName(-1) == nullptr);

  // Every listed name must apply.
  for (int i = 0; i < n; i++) {
    remesh::RemeshParams p;
    TASSERT(remesh::applyRemeshPreset(p, remesh::remeshPresetName(i)));
  }

  // Unknown names fail and leave the params untouched.
  remesh::RemeshParams p;
  p.field_smoothness = 9.0f;
  p.curvature_smooth_iters = 7;
  TASSERT(!remesh::applyRemeshPreset(p, "no-such-preset"));
  TASSERT(p.field_smoothness == 9.0f);
  TASSERT(p.curvature_smooth_iters == 7);
}

void testResetAndPreservation()
{
  remesh::RemeshParams p;
  p.target_quad_count = 1234;
  p.target_edge_length = 0.33f;
  p.seed = 7u;
  p.field_smoothness = 9.0f; // a prior knob edit the preset must reset

  TASSERT(remesh::applyRemeshPreset(p, "organic-clean"));
  // Sizing + seed survive.
  TASSERT(p.target_quad_count == 1234);
  TASSERT(p.target_edge_length == 0.33f);
  TASSERT(p.seed == 7u);
  // Prior edits do not: the preset is defaults + its deltas.
  TASSERT(p.field_smoothness == 1.0f);
  TASSERT(p.curvature_smooth_iters == 1);
  TASSERT(p.auto_density);
  TASSERT(p.feature_min_chain == 3);
}

void testBundleSignatures()
{
  remesh::RemeshParams p;

  TASSERT(remesh::applyRemeshPreset(p, "organic-noisy"));
  TASSERT(p.field_smoothness == 2.0f);
  TASSERT(p.feature_hysteresis > 0.0f);
  TASSERT(p.pre_remesh);
  TASSERT(p.auto_retry);

  TASSERT(remesh::applyRemeshPreset(p, "messy-character"));
  TASSERT(p.per_component);
  TASSERT(p.cap_odd_holes);
  TASSERT(p.input_hole_fill_max_frac > 0.0f);
  TASSERT(p.triage_min_component_frac > 0.0f);
  TASSERT(p.feature_hysteresis == 0.0f);
  TASSERT(p.auto_retry);

  TASSERT(remesh::applyRemeshPreset(p, "scan"));
  TASSERT(p.pre_remesh);
  TASSERT(p.cap_odd_holes);
  TASSERT(p.feature_hysteresis > 0.0f);
  TASSERT(p.auto_retry);
  // A scan bundle must not carry messy-character's per-component split.
  TASSERT(!p.per_component);

  TASSERT(remesh::applyRemeshPreset(p, "hard-surface"));
  TASSERT(p.sharp_angle < 0.6f); // 30deg, below the 45deg default
  TASSERT(p.density_gradation == 0.3f);
  TASSERT(!p.pre_remesh);
  TASSERT(!p.auto_retry);

  TASSERT(remesh::applyRemeshPreset(p, "cad"));
  TASSERT(p.sharp_angle < 0.6f);
  TASSERT(p.per_component);
  TASSERT(p.pre_remesh);
  // The pre-pass must pin the same shallow bevels the field solve tags.
  TASSERT(p.pre_remesh_sharp_angle == p.sharp_angle);
  TASSERT(p.feature_hysteresis == 0.0f); // crisp input: no hysteresis band
  TASSERT(!p.auto_retry);

  // Tier-7 rule: hysteresis is never set without a min-chain to pair with.
  for (int i = 0; remesh::remeshPresetName(i); i++) {
    remesh::RemeshParams q;
    remesh::applyRemeshPreset(q, remesh::remeshPresetName(i));
    if (q.feature_hysteresis > 0.0f) {
      TASSERT(q.feature_min_chain >= 3);
    }
  }
}

// One preset through the whole pipeline: the bundles must be runnable, not
// just self-consistent.
void testPresetPipelineSmoke()
{
  Mesh *t = makeTorus(48, 32, 1.0f, 0.35f);

  remesh::RemeshParams p;
  p.target_edge_length = 0.1f;
  TASSERT(remesh::applyRemeshPreset(p, "organic-clean"));
  TASSERT(p.target_edge_length == 0.1f);

  Mesh *out = remesh::QuadRemesh(*t, p);
  TASSERT(out != nullptr);
  if (out) {
    TASSERT(out->f.count > 0);
    litestl::alloc::Delete<Mesh>(out);
  }
  litestl::alloc::Delete<Mesh>(t);
}

} // namespace

int main()
{
  testNameTableAndUnknown();
  testResetAndPreservation();
  testBundleSignatures();
  testPresetPipelineSmoke();
  return retval;
}
