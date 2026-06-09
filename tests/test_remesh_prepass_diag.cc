// TEMP DIAGNOSTIC (Tier 9e shred triage) — not a regression gate. Loads
// Simple.obj and runs the pre-pass at a FINE target (the live default regime
// where the shred reproduces), dumping the interleave path after each outer
// iter so external analysis can pin exactly when slivers/bridges appear. Delete
// this file (and its CMakeLists entry + _diag_*.obj) once the cause is fixed.
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "mesh/mesh.h"
#include "mesh/utils/obj_io.h"
#include "mesh/utils/triangulate.h"
#include "remesh/extract/reproject.h"
#include "remesh/preremesh.h"

#include "test_config.h"

#include <cstdio>
#include <string>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using litestl::math::float3;

namespace {

Mesh *freshSimple()
{
  std::string path = std::string(SCULPTCORE_ASSETS_DIR) + "/Simple.obj";
  Mesh *m = mesh::loadObj(path.c_str(), /*keepNgons=*/true);
  if (m) {
    m->thawTopo();
    mesh::triangulateMesh(*m);
  }
  return m;
}

void writeOut(const char *tag, Mesh &m)
{
  std::string out = std::string(SCULPTCORE_ASSETS_DIR) + "/_diag_" + tag + ".obj";
  mesh::writeObj(m, out.c_str());
  fprintf(stderr, "[%s] V=%d F=%d -> %s\n", tag, m.v.count, m.f.count, out.c_str());
}

void reproject(Mesh &m)
{
  Mesh *input = freshSimple();
  remesh::ReprojectParams rp;
  remesh::reprojectToSurface(m, *input, rp);
  litestl::alloc::Delete<Mesh>(input);
}

// BK remesh only, no smooth: the clean baseline.
void runBkOnly(const char *tag, float L)
{
  Mesh *m = freshSimple();
  if (!m) { fprintf(stderr, "[%s] load failed\n", tag); return; }
  remesh::PreRemeshParams p;
  p.target = L;
  p.iters = 5;
  p.align = 1.0f;
  p.smooth_iters = 0;
  p.preserve_features = true;
  remesh::preRemesh(*m, p);
  writeOut(tag, *m);
  litestl::alloc::Delete<Mesh>(m);
}

// Field-aligned smooth, NO reproject (raw drift), one preRemesh call.
void runField(const char *tag, int smooth_iters, float L)
{
  Mesh *m = freshSimple();
  if (!m) { fprintf(stderr, "[%s] load failed\n", tag); return; }
  remesh::PreRemeshParams p;
  p.target = L;
  p.iters = 5;
  p.align = 1.0f;
  p.smooth_iters = smooth_iters;
  p.preserve_features = true;
  remesh::preRemesh(*m, p);
  writeOut(tag, *m);
  litestl::alloc::Delete<Mesh>(m);
}

// The shipping interleave path (debug app runPreRemesh): one outer iter of
// preRemesh(iters=1) + reproject, repeated. Dump after EACH iter to localize
// when the artifact first appears.
void runInterleaveStepwise(int smooth_iters, int outer, float L)
{
  Mesh *m = freshSimple();
  if (!m) { fprintf(stderr, "interleave load failed\n"); return; }
  for (int it = 0; it < outer; it++) {
    remesh::PreRemeshParams p;
    p.target = L;
    p.iters = 1;
    p.align = 1.0f;
    p.smooth_iters = smooth_iters;
    p.preserve_features = true;
    p.bootstrap_iters = it == 0 ? 2 : 0;
    p.seed = 1u + uint32_t(it);
    remesh::preRemesh(*m, p);
    reproject(*m);
    char tag[32];
    std::snprintf(tag, sizeof(tag), "iter%d", it + 1);
    writeOut(tag, *m);
  }
  litestl::alloc::Delete<Mesh>(m);
}

} // namespace

int main()
{
  setvbuf(stderr, nullptr, _IONBF, 0);
  Mesh *m0 = freshSimple();
  if (!m0) { fprintf(stderr, "load failed\n"); return 1; }
  float3 lo = m0->v.co[0], hi = m0->v.co[0];
  for (int v : m0->v) {
    float3 p = m0->v.co[v];
    for (int i = 0; i < 3; i++) {
      if (p[i] < lo[i]) lo[i] = p[i];
      if (p[i] > hi[i]) hi[i] = p[i];
    }
  }
  float D = (hi - lo).length();
  // Fine target: the live debug-app default regime (target_edge_length=0.1) where
  // the shred reproduces; D/0.15 ~ 170 edges across, ~2x finer than the prior
  // L=D/59 triage and fast enough to iterate.
  const float L = 0.15f;
  fprintf(stderr, "Simple.obj D=%.4g  L=%.4g (D/L=%.1f)\n", D, L, D / L);
  writeOut("input", *m0);
  litestl::alloc::Delete<Mesh>(m0);

  runBkOnly("A_bk_only", L);
  runField("C_field5", 5, L);   // field smooth (5 inner), no reproject
  runField("C_field2", 2, L);   // field smooth (2 inner), no reproject
  runInterleaveStepwise(2, 5, L); // shipping path, per-iter dumps iter1..iter5
  return retval;
}
