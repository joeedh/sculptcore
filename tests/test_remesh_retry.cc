// Tier 8a test: the metric-driven retry loop around QuadRemesh.
//
//  - auto_retry off (default) must be the legacy single-run path: no trail.
//  - auto_retry on over a clean input (the zero-singularity torus) must stop
//    after one attempt (no rule fires) and return a byte-identical mesh to
//    the retry-off run.
//  - A fold-heavy input (the pipeline sphere) must engage the fold rungs
//    (field_smoothness -> pre_remesh) and pick a winner with fewer folds.
//  - An absurd target edge length forces a clean extract failure every
//    attempt: the failure fallback ladder must escalate in order
//    (field_smoothness -> pre_remesh), record the trail, and respect
//    max_attempts; max_attempts=1 must cap the loop at the initial run.
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/mesh_shapes.h"
#include "remesh/remesh.h"
#include "remesh/remesh_params.h"
#include "remesh/remesh_report.h"

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
using litestl::math::float3;

namespace {

void dumpTrail(const char *tag, const remesh::RemeshRunReport &rep)
{
  for (int i = 0; i < rep.attempts_run; i++) {
    const remesh::RemeshRunReport::RetryAttempt &a = rep.attempts[i];
    fprintf(stderr,
            "[%s] attempt %d: %s ok=%d folds=%d sing=%d inverted=%d odd=%d "
            "ratio=%.3g reason=%s\n",
            tag,
            i,
            a.escalation,
            int(a.success),
            a.parametrization_folds,
            a.num_singularities,
            a.inverted_faces,
            a.odd_residuals,
            a.max_adjacent_edge_ratio,
            a.failure_reason.c_str());
  }
}

// Order-stable position checksum: equal meshes from the deterministic pipeline
// iterate identically, so a running sum over (index-weighted) coords suffices.
double meshChecksum(Mesh *m)
{
  double sum = 0.0;
  int i = 0;
  for (int v : m->v) {
    float3 co = m->v.co[v];
    sum += (i + 1) * (double(co[0]) + 2.0 * double(co[1]) + 4.0 * double(co[2]));
    i++;
  }
  return sum;
}

// Retry off (the default): legacy single run, the trail must stay untouched.
// Retry on over the same clean input: one attempt, winner 0, identical mesh.
void testRetryOffAndCleanParity()
{
  // The zero-singularity, fold-free full-pipeline case (test_remesh_extract.cc).
  Mesh *s = makeTorus(48, 32, 1.0f, 0.35f);

  remesh::RemeshParams p;
  p.target_edge_length = 0.1f;
  remesh::RemeshRunReport off;
  Mesh *out_off = remesh::QuadRemesh(*s, p, nullptr, nullptr, &off);
  TASSERT(out_off != nullptr);
  TASSERT(off.success);
  TASSERT(off.attempts_run == 0);
  TASSERT(off.winner == -1);
  TASSERT(off.solve_faces > 0);

  remesh::RemeshParams pr = p;
  pr.auto_retry = true;
  pr.max_attempts = 3;
  remesh::RemeshRunReport on;
  Mesh *out_on = remesh::QuadRemesh(*s, pr, nullptr, nullptr, &on);
  dumpTrail("clean", on);
  TASSERT(out_on != nullptr);
  TASSERT(on.success);
  // Clean input: no escalation rule fires, the loop stops after one attempt.
  TASSERT(on.attempts_run == 1);
  TASSERT(on.winner == 0);
  TASSERT(std::strcmp(on.attempts[0].escalation, "initial") == 0);
  TASSERT(on.attempts[0].from_original);
  TASSERT(on.attempts[0].success);

  if (out_off && out_on) {
    TASSERT(out_off->v.count == out_on->v.count);
    TASSERT(out_off->f.count == out_on->f.count);
    TASSERT(meshChecksum(out_off) == meshChecksum(out_on));
  }
  if (out_off)
    litestl::alloc::Delete<Mesh>(out_off);
  if (out_on)
    litestl::alloc::Delete<Mesh>(out_on);
  litestl::alloc::Delete<Mesh>(s);
}

// The pipeline sphere at this density carries enough parametrization folds to
// trip the noisy-folds rung: the loop must escalate field_smoothness then
// pre_remesh and the winner must beat the initial attempt on folds.
void testRetryEngagesAndImprovesFolds()
{
  Mesh *s = makeUVSphere(24, 32, 2.0f);

  remesh::RemeshParams p;
  p.target_edge_length = 0.1f;
  p.auto_retry = true;
  p.max_attempts = 3;
  remesh::RemeshRunReport rep;
  Mesh *out = remesh::QuadRemesh(*s, p, nullptr, nullptr, &rep);
  dumpTrail("foldy", rep);

  TASSERT(out != nullptr);
  TASSERT(rep.success);
  TASSERT(rep.attempts_run == 3);
  TASSERT(std::strcmp(rep.attempts[0].escalation, "initial") == 0);
  TASSERT(std::strcmp(rep.attempts[1].escalation, "field_smoothness") == 0);
  TASSERT(std::strcmp(rep.attempts[2].escalation, "pre_remesh") == 0);
  TASSERT(rep.winner >= 0 && rep.winner < rep.attempts_run);
  // The whole point of the loop: the kept result has fewer folds than the
  // single-attempt run would have produced.
  TASSERT(rep.attempts[rep.winner].parametrization_folds <
          rep.attempts[0].parametrization_folds);
  // The top-level report must be the winner's report.
  TASSERT(rep.parametrization_folds == rep.attempts[rep.winner].parametrization_folds);
  for (int i = 0; i < rep.attempts_run; i++) {
    TASSERT(rep.attempts[i].from_original);
  }

  if (out)
    litestl::alloc::Delete<Mesh>(out);
  litestl::alloc::Delete<Mesh>(s);
}

// An edge length far beyond the mesh extent leaves no lattice points: every
// attempt fails cleanly and the fallback ladder must walk its fixed order.
void testRetryFailureLadder()
{
  Mesh *s = makeUVSphere(16, 24, 1.0f);

  remesh::RemeshParams p;
  p.target_edge_length = 50.0f;
  p.auto_retry = true;
  p.max_attempts = 3;
  // The third rung flips pre_remesh on; pin its target to a sane explicit
  // value so the pre-pass doesn't inherit the absurd 50.0 output scale.
  p.pre_remesh_target = 0.2f;
  remesh::RemeshRunReport rep;
  Mesh *out = remesh::QuadRemesh(*s, p, nullptr, nullptr, &rep);

  TASSERT(out == nullptr);
  TASSERT(!rep.success);
  TASSERT(rep.attempts_run == 3);
  TASSERT(rep.winner >= 0 && rep.winner < 3);
  TASSERT(std::strcmp(rep.attempts[0].escalation, "initial") == 0);
  TASSERT(std::strcmp(rep.attempts[1].escalation, "field_smoothness") == 0);
  TASSERT(std::strcmp(rep.attempts[2].escalation, "pre_remesh") == 0);
  for (int i = 0; i < rep.attempts_run; i++) {
    TASSERT(rep.attempts[i].from_original);
    TASSERT(!rep.attempts[i].success);
    TASSERT(!rep.attempts[i].failure_reason.empty());
  }
  // The escalations must actually land in the attempt's recorded params.
  TASSERT(rep.attempts[1].params.field_smoothness ==
          2.0f * rep.attempts[0].params.field_smoothness);
  TASSERT(rep.attempts[2].params.pre_remesh);

  if (out)
    litestl::alloc::Delete<Mesh>(out);
  litestl::alloc::Delete<Mesh>(s);
}

// max_attempts=1 with retry on: the cap wins over the would-fire ladder.
void testRetryAttemptCap()
{
  Mesh *s = makeUVSphere(16, 24, 1.0f);

  remesh::RemeshParams p;
  p.target_edge_length = 50.0f;
  p.auto_retry = true;
  p.max_attempts = 1;
  remesh::RemeshRunReport rep;
  Mesh *out = remesh::QuadRemesh(*s, p, nullptr, nullptr, &rep);

  TASSERT(out == nullptr);
  TASSERT(rep.attempts_run == 1);
  TASSERT(rep.winner == 0);
  TASSERT(std::strcmp(rep.attempts[0].escalation, "initial") == 0);

  if (out)
    litestl::alloc::Delete<Mesh>(out);
  litestl::alloc::Delete<Mesh>(s);
}

} // namespace

int main()
{
  testRetryOffAndCleanParity();
  testRetryEngagesAndImprovesFolds();
  testRetryFailureLadder();
  testRetryAttemptCap();
  return retval;
}
