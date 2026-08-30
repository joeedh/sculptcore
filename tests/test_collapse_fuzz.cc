// Edge-collapse fuzz / integrity gate. Loads the closed Simple.obj sphere mesh
// and repeatedly collapses random edges until fewer than 10 edges remain,
// re-validating the half-edge topology and the Euler characteristic at regular
// intervals. A valid manifold-preserving edge collapse changes V/E/F by
// (-1,-3,-2) on an interior triangle, so the Euler characteristic chi = V-E+F
// is invariant; this catches any random collapse that silently corrupts the
// mesh or alters its topology. Euler invariance is only asserted while the mesh
// is still well above the degenerate limit (a tetrahedron and below can legally
// change topology under collapse); the run still drives all the way to < 10
// edges. See documentation/quad-remeshing.md.
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/rand.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/utils/edge_collapse.h"
#include "mesh/utils/mesh_validate.h"
#include "mesh/utils/obj_io.h"

#include "test_config.h"

#include <cstdint>
#include <cstdio>
#include <string>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using litestl::math::float3;
using litestl::util::Random;
using litestl::util::Vector;

namespace {

const int kStopEdges = 10;  // collapse until the mesh has fewer than this
const int kEulerFloor = 16; // assert chi-invariance only above the degenerate endgame
const int kCheckEvery = 50; // run a full integrity check every N successful collapses

// Validate the half-edge topology and (above the degenerate floor) the Euler
// characteristic. Always aborts the test on a structural defect.
void checkIntegrity(Mesh &m, int chi0, int collapses, const char *tag)
{
  RemeshReport r = remeshValidate(m);
  if (!r.manifold) {
    fprintf(stderr,
            "[%s] non-manifold after %d collapses: %s\n",
            tag,
            collapses,
            r.manifold_error.c_str());
  }
  test_assert(r.manifold);
  test_assert(r.non_manifold_edges == 0);

  if (r.edge_count >= kEulerFloor) {
    if (r.euler != chi0) {
      fprintf(stderr,
              "[%s] euler changed %d -> %d after %d collapses (V=%d E=%d F=%d)\n",
              tag,
              chi0,
              r.euler,
              collapses,
              r.vert_count,
              r.edge_count,
              r.face_count);
    }
    test_assert(r.euler == chi0);
    test_assert(r.consistent_winding);
  }
}

// Fisher-Yates shuffle so collapses are applied in a random order each pass.
void shuffle(Vector<int> &v, Random &rnd)
{
  for (int i = int(v.size()) - 1; i > 0; i--) {
    int j = int(rnd.get_int() % uint32_t(i + 1));
    int t = v[i];
    v[i] = v[j];
    v[j] = t;
  }
}

void runFuzz(uint32_t seed)
{
  char tag[64];
  snprintf(tag, sizeof(tag), "simple-seed%u", seed);

  std::string path = std::string(SCULPTCORE_ASSETS_DIR) + "/Simple.obj";
  Mesh *m = mesh::loadObj(path.c_str(), /*keepNgons=*/false);
  test_assert(m != nullptr);
  m->thawTopo();

  RemeshReport r0 = remeshValidate(*m);
  test_assert(r0.manifold);
  test_assert(r0.consistent_winding);
  int chi0 = r0.euler;
  fprintf(stderr,
          "[%s] input V=%d E=%d F=%d chi=%d\n",
          tag,
          r0.vert_count,
          r0.edge_count,
          r0.face_count,
          chi0);

  Random rnd(seed);
  int collapses = 0, rejected = 0, passes = 0, sinceCheck = 0;

  while (m->e.count >= kStopEdges) {
    // Snapshot the live, face-bearing edges and try them in random order. A
    // collapse frees and recreates nearby edges, so re-check each index against
    // the freemap before using it; freed/wire slots are simply skipped.
    Vector<int> edges;
    for (int e : m->e) {
      if (m->e.c[e] != ELEM_NONE) {
        edges.append(e);
      }
    }
    if (edges.isEmpty()) {
      break;
    }
    shuffle(edges, rnd);

    int progressed = 0;
    for (int e : edges) {
      if (m->e.count < kStopEdges) {
        break;
      }
      if (e >= int(m->e.capacity()) || m->e.freemap[e] || m->e.c[e] == ELEM_NONE) {
        continue;
      }
      int v0 = m->e.vs[e][0], v1 = m->e.vs[e][1];
      float3 mid = (m->v.co[v0] + m->v.co[v1]) * 0.5f;
      if (collapseEdge(*m, e, mid, /*blend=*/0.5f)) {
        collapses++;
        progressed++;
        if (++sinceCheck >= kCheckEvery) {
          sinceCheck = 0;
          checkIntegrity(*m, chi0, collapses, tag);
        }
      } else {
        rejected++;
      }
    }
    passes++;
    if (progressed == 0) {
      break; // no collapsible edge remains (the mesh is at its irreducible core)
    }
  }

  RemeshReport rf = remeshValidate(*m);
  fprintf(stderr,
          "[%s] done: %d collapses, %d rejected, %d passes -> V=%d E=%d F=%d "
          "chi=%d manifold=%d\n",
          tag,
          collapses,
          rejected,
          passes,
          rf.vert_count,
          rf.edge_count,
          rf.face_count,
          rf.euler,
          int(rf.manifold));

  test_assert(m->e.count < kStopEdges); // drove the mesh below the edge target
  litestl::alloc::Delete<Mesh>(m);
}

} // namespace

int main()
{
  setvbuf(stderr, nullptr, _IONBF, 0);

  for (uint32_t seed : {1u, 7u, 12345u}) {
    runFuzz(seed);
  }

  return test_end();
}
