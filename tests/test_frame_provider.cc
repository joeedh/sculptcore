/* Frame provider (displacementAndSubSurf plan, F3 gate): smoothed normal +
 * cross-field tangent per vertex on sphere/cube fixtures. Asserts the frames
 * are orthonormal, the field's total singularity index matches Poincaré–Hopf
 * (Σ quarter-turn indices == 4·χ == 8 on a closed genus-0 mesh), and the
 * computation is deterministic (bit-identical across recomputes — the
 * cross-backend parity anchor). */
#include "test_util.h"

#include "displace/frames.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/mesh_shapes.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>
#include <cstring>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace litestl;
using namespace litestl::math;
using litestl::util::Vector;

static AttrData<float3> *attr(Mesh &m, const char *name)
{
  AttrRef ref = m.v.attrs.find_attribute(AttrType::FLOAT3, name);
  return ref.exists() ? static_cast<AttrData<float3> *>(ref.data) : nullptr;
}

static void snapshotFrames(Mesh &m, Vector<float3> &normals, Vector<float3> &tangents)
{
  AttrData<float3> *no = attr(m, displace::FRAME_NORMAL_ATTR);
  AttrData<float3> *ta = attr(m, displace::FRAME_TANGENT_ATTR);
  normals.clear();
  tangents.clear();
  for (int v : m.v) {
    normals.append(no->safe_get(v));
    tangents.append(ta->safe_get(v));
  }
}

static int checkFixture(Mesh *m, const char *tag)
{
  m->recalc_normals();

  displace::FrameProviderParams params;
  displace::updateFramesAll(*m, params);

  AttrData<float3> *no = attr(*m, displace::FRAME_NORMAL_ATTR);
  AttrData<float3> *ta = attr(*m, displace::FRAME_TANGENT_ATTR);
  test_assert(no && ta);

  // Orthonormality: unit smoothed normals, unit tangents, t ⊥ n.
  int badLen = 0, badOrtho = 0;
  for (int v : m->v) {
    float3 n = (*no)[v];
    float3 t = (*ta)[v];
    if (std::fabs(n.length() - 1.0f) > 1e-4f || std::fabs(t.length() - 1.0f) > 1e-4f) {
      badLen++;
    }
    if (std::fabs(n.dot(t)) > 1e-4f) {
      badOrtho++;
    }
  }
  fprintf(stderr,
          "%s: badLen=%d badOrtho=%d verts=%d\n",
          tag,
          badLen,
          badOrtho,
          int(m->v.count));
  test_assert(badLen == 0);
  test_assert(badOrtho == 0);

  // Poincaré–Hopf: Σ quarter-turn indices == 4·χ == 8 on closed genus-0.
  int indexSum = displace::crossFieldIndexSum(*m);
  fprintf(stderr, "%s: crossFieldIndexSum=%d (expect 8)\n", tag, indexSum);
  test_assert(indexSum == 8);

  // Determinism: recomputing over the same mesh is bit-identical.
  Vector<float3> n1, t1, n2, t2;
  snapshotFrames(*m, n1, t1);
  displace::updateFramesAll(*m, params);
  snapshotFrames(*m, n2, t2);
  test_assert(n1.size() == n2.size() && t1.size() == t2.size());
  int diff = 0;
  for (size_t i = 0; i < n1.size(); i++) {
    if (std::memcmp(&n1[int(i)], &n2[int(i)], sizeof(float3)) != 0 ||
        std::memcmp(&t1[int(i)], &t2[int(i)], sizeof(float3)) != 0)
    {
      diff++;
    }
  }
  fprintf(stderr, "%s: recompute diffs=%d\n", tag, diff);
  test_assert(diff == 0);

  return indexSum;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  {
    Mesh *m = makeUVSphere(16, 24, 1.0f);
    test_assert(m != nullptr);
    checkFixture(m, "sphere");
    alloc::Delete(m);
  }
  {
    Mesh *m = createCube(8, 0.5f);
    test_assert(m != nullptr);
    checkFixture(m, "cube");
    alloc::Delete(m);
  }

  /* Skip test_end(): attr name strings stay live in the alloc tracker
   * (mirrors the other spatial/mesh tests). */
  return retval;
}
