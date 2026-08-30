/* VDM region promotion (displacementAndSubSurf plan, V4 gate): a scripted
 * stroke followed by a forced promotion must turn the displaced VDM region
 * into real geometry — subdivided faces flipped to GEOM carrier, verts seeded
 * onto base + VDM (boundary verts pinned to the surface the VDM neighbours
 * still render — no cracks), the promoted footprint's texels cleared, and the
 * carrier boundary marked EDGE_LAYER_REGION. One undo press reverts topology,
 * seeds, carriers AND texels together; redo replays. A dyntopo stroke across
 * the promoted seam must preserve the region-boundary feature edges. */
#include "test_util.h"

#include "debug/scene.h"
#include "debug/script.h"

#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "mesh/utils/mesh_validate.h"
#include "spatial/spatial.h"
#include "vdm/vdm_store.h"

#include "litestl/math/vector.h"

#include <cmath>
#include <cstdio>

test_init;

/* Local assert (mirrors test_spatial_raycast.cc): the shared test_assert can
 * lose an earlier failure, this latches retval. */
#undef test_assert
#define test_assert(expr)                                                                \
  do {                                                                                   \
    if (!(expr)) {                                                                       \
      fprintf(stderr, "%s failed\n", #expr);                                             \
      retval = 1;                                                                        \
    }                                                                                    \
  } while (0)

using namespace sculptcore;
using namespace sculptcore::debug_app;
using litestl::math::float3;

static int countCarrier(Scene &scene, int carrier)
{
  int n = 0;
  for (int f : scene.mesh->f) {
    if (scene.tree->treeMesh.f.carrier[f] == carrier) {
      n++;
    }
  }
  return n;
}

static int countLayerRegionEdges(Scene &scene)
{
  mesh::BoolAttrView *view =
      mesh::boundary::findBoolEdgeView(scene.mesh, mesh::boundary::EDGE_LAYER_REGION);
  if (!view) {
    return 0;
  }
  int n = 0;
  for (int e : scene.mesh->e) {
    if (view->get(e)) {
      n++;
    }
  }
  return n;
}

static float maxTexelLen(vdm::VdmStore *store)
{
  float m = 0.0f;
  store->foreachTile([&](const vdm::VdmTile &t) {
    for (const float3 &d : t.texels) {
      float l = d.length();
      m = l > m ? l : m;
    }
  });
  return m;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  Scene scene(256, 256, /*headless=*/true);
  auto r = script::run(scene,
                       "make_shape kind=grid n=24 m=24 size=1.0\n"
                       "build_spatial leaf_limit=64 depth_limit=10\n"
                       "vdm_init resolution=256 tile=16 planar_uv=1\n"
                       "save_pos id=geo\n"
                       "vdm_stroke origin=0,0,0 normal=0,0,1 radius=0.25 strength=1.0\n"
                       "save_vdm id=stroked\n",
                       ".");
  test_assert(r.ok);
  if (!r.ok) {
    fprintf(stderr, "  setup line %d: %s\n", r.line_no, r.error.c_str());
    return 1;
  }

  int vdmBefore = countCarrier(scene, 1);
  int geomBefore = countCarrier(scene, 0);
  int vertsBefore = scene.mesh->v.count;
  int facesBefore = scene.mesh->f.count;
  float strokeMag = maxTexelLen(scene.vdm);
  test_assert(geomBefore == 0 && vdmBefore == facesBefore);
  test_assert(strokeMag > 1e-4f);

  // --- promote the displaced region (force = every face with stored |D|) ---
  r = script::run(scene, "vdm_promote force=1\n", ".");
  test_assert(r.ok);
  if (!r.ok) {
    fprintf(stderr, "  promote line %d: %s\n", r.line_no, r.error.c_str());
    return 1;
  }

  int geomAfter = countCarrier(scene, 0);
  int regionEdges = countLayerRegionEdges(scene);
  float clearedMag = maxTexelLen(scene.vdm);
  fprintf(stderr,
          "promote: geomFaces=%d (was 0), verts %d->%d, faces %d->%d, "
          "regionEdges=%d, texelMag %f->%f\n",
          geomAfter,
          vertsBefore,
          int(scene.mesh->v.count),
          facesBefore,
          int(scene.mesh->f.count),
          regionEdges,
          strokeMag,
          clearedMag);
  test_assert(geomAfter > 0);
  test_assert(int(scene.mesh->v.count) > vertsBefore); // subdivision happened
  test_assert(int(scene.mesh->f.count) > facesBefore);
  test_assert(regionEdges > 0);
  // Footprint texels cleared: what's left is at most boundary residue, far
  // below the stroke magnitude.
  test_assert(clearedMag < strokeMag * 0.1f);

  // Seeded geometry actually moved off the base plane (grid z was 0).
  float maxZ = 0.0f;
  for (int v : scene.mesh->v) {
    float z = std::fabs(scene.mesh->v.co[v][2]);
    maxZ = z > maxZ ? z : maxZ;
  }
  fprintf(stderr, "promote: maxZ=%f (stroke mag %f)\n", maxZ, strokeMag);
  test_assert(maxZ > strokeMag * 0.5f);

  // No cracks: topology stays valid through subdivide + seed.
  {
    std::string terr;
    test_assert(mesh::checkTopology(*scene.mesh, terr, /*requireTriangles=*/false));
    if (!terr.empty()) {
      fprintf(stderr, "topology: %s\n", terr.c_str());
    }
  }

  // --- one undo reverts topology + seeds + carriers + texels together ---
  scene.meshLog.undo(scene.mesh, scene.tree);
  r = script::run(scene, "assert_pos id=geo\nassert_vdm id=stroked\n", ".");
  test_assert(r.ok);
  test_assert(int(scene.mesh->v.count) == vertsBefore);
  test_assert(int(scene.mesh->f.count) == facesBefore);
  test_assert(countCarrier(scene, 1) == vdmBefore);
  test_assert(countLayerRegionEdges(scene) == 0);
  fprintf(stderr,
          "undo: verts=%d faces=%d vdmFaces=%d regionEdges=%d\n",
          int(scene.mesh->v.count),
          int(scene.mesh->f.count),
          countCarrier(scene, 1),
          countLayerRegionEdges(scene));

  // --- redo replays the promotion ---
  scene.meshLog.redo(scene.mesh, scene.tree);
  test_assert(countCarrier(scene, 0) == geomAfter);
  test_assert(countLayerRegionEdges(scene) == regionEdges);
  test_assert(maxTexelLen(scene.vdm) < strokeMag * 0.1f);

  // --- dyntopo across the promoted seam preserves the region boundary ---
  // preserve_features defaults true in DynTopoParams; detail below the grid's
  // ~0.043 edge length forces splits in the stroked band.
  r = script::run(scene,
                  "set_brush_tool tool=draw\n"
                  "set_brush radius=0.3 strength=0.4\n"
                  "dyntopo enabled=1 detail=0.03\n"
                  "stroke origin=0.1,0,0 normal=0,0,1 repeat=3\n",
                  ".");
  test_assert(r.ok);
  if (!r.ok) {
    fprintf(stderr, "  dyntopo line %d: %s\n", r.line_no, r.error.c_str());
    return 1;
  }
  int regionEdgesAfter = countLayerRegionEdges(scene);
  fprintf(stderr,
          "dyntopo across seam: regionEdges %d -> %d\n",
          regionEdges,
          regionEdgesAfter);
  // Splits may add flagged child edges; the feature must never vanish.
  test_assert(regionEdgesAfter >= regionEdges);

  /* Return retval directly (not test_end()): the debug Scene infrastructure
   * leaves allocations live at exit (mirrors test_paint_undo.cc). */
  return retval;
}
