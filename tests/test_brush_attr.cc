// CPU verification of the sbrush DSL attribute path (boundary-conditions wave
// 1). The `color` kernel declares `attr vertex float4 color`; a C++-backend
// stroke must paint that per-vertex layer toward the target color under the
// brush and leave verts outside the dab untouched. Exercises the full chain:
// codegen lowering of v.color, the BrushAttrManifestEntry, the executor's
// per-dab layer resolution, and CommandCtxBase::boundAttr<T>.
//
// The layer is pre-created and zeroed before the stroke: a freshly materialized
// attribute is not value-initialized yet (a default-init for new paint layers
// is a paint-brush-wave follow-up), so zeroing first lets us distinguish "brush
// wrote it" from "uninitialized garbage" and verify spatial localization.
#include "test_util.h"

#include "debug/scene.h"
#include "debug/script.h"

#include "mesh/attribute.h"
#include "mesh/mesh.h"

#include <cstdio>

test_init;

using namespace sculptcore::debug_app;
using namespace sculptcore::mesh;
using namespace litestl::math;

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  // Scope the Scene so its allocations are released before test_end()'s
  // leak check (mirrors test_debug_script.cc).
  {
    Scene scene(256, 256, /*headless=*/true);

    // Setup only (no stroke yet). size=0.5 => half-extent 0.25, so the +Z face
    // sits at z=0.25 (mirrors test_live_stroke).
    auto r = script::run(scene,
                         "make_cube subdivs=12 size=0.5\n"
                         "build_spatial leaf_limit=256 depth_limit=8\n"
                         "set_brush_tool tool=color\n"
                         "set_brush radius=0.25 strength=1.0\n"
                         "set_backend backend=cpp\n",
                         ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  setup line %d: %s\n", r.line_no, r.error.c_str());
      return 1;
    }

    Mesh *m = scene.mesh;
    test_assert(m != nullptr);

    // Pre-create + zero the color layer the kernel will paint into.
    AttrRef &cref = m->v.attrs.ensure(AttrType::FLOAT4, "color", /*materialize=*/true);
    AttrData<float4> *col = cref.get_data<float4>();
    test_assert(col != nullptr);
    for (int i = 0; i < m->v.count; i++) {
      (*col)[i] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    // Now stroke the center of the +Z face.
    r = script::run(scene, "stroke origin=0,0,0.25 normal=0,0,1\n", ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  stroke line %d: %s\n", r.line_no, r.error.c_str());
      return 1;
    }

    // Verts at the dab center get painted toward (1,0,0,1); verts on the opposite
    // (-Z) face are outside the dab and must still be zero.
    const float3 center{0.0f, 0.0f, 0.25f};
    int paintedNear = 0, untouchedFar = 0, bleedFar = 0;
    for (int i = 0; i < m->v.count; i++) {
      float3 co = m->v.co[i];
      float4 c = (*col)[i];
      if ((co - center).length() < 0.1f && c[0] > 0.5f) {
        paintedNear++;
      }
      if (co[2] < -0.2f) {
        if (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f) {
          untouchedFar++;
        } else {
          bleedFar++;
        }
      }
    }

    fprintf(stderr,
            "paintedNear=%d untouchedFar=%d bleedFar=%d\n",
            paintedNear,
            untouchedFar,
            bleedFar);
    test_assert(paintedNear > 0);  // brush actually wrote the layer
    test_assert(untouchedFar > 0); // far verts exist
    test_assert(bleedFar == 0);    // and were not touched by the dab
  }

  // --- face stage: poly-group paint writes a per-face int "group" attr ---
  // Exercises the DSL `face` kernel, BasicFaceIter (centroid + face-attr
  // lowering), and the executor's face dispatch. The kernel writes the stroke's
  // `activeGroup` id on faces whose centroid is under the dab (a single nonzero
  // id per stroke — maxFaceGroup()+1 for a fresh mesh); the executor value-inits
  // the rest to 0.
  {
    Scene scene(256, 256, /*headless=*/true);
    auto r = script::run(scene,
                         "make_cube subdivs=12 size=0.5\n"
                         "build_spatial leaf_limit=256 depth_limit=8\n"
                         "set_brush_tool tool=polygroup\n"
                         "set_brush radius=0.25 strength=1.0\n"
                         "set_backend backend=cpp\n"
                         "stroke origin=0,0,0.25 normal=0,0,1\n",
                         ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  poly line %d: %s\n", r.line_no, r.error.c_str());
      return 1;
    }

    Mesh *m = scene.mesh;
    test_assert(m->f.attrs.has(AttrType::INT, "group"));
    AttrRef gref = m->f.attrs.find_attribute(AttrType::INT, "group");
    AttrData<int> *gd = gref.get_data<int>();
    test_assert(gd != nullptr);

    // The stroke paints a single nonzero id; discover it (don't hardcode — it's
    // the per-stroke activeGroup, not a fixed constant) and require every painted
    // face to carry exactly that id.
    int paintedId = 0;
    for (int i = 0; i < m->f.count; i++) {
      int g = (*gd)[i];
      if (g != 0) {
        paintedId = g;
        break;
      }
    }
    int painted = 0, unpainted = 0, other = 0;
    for (int i = 0; i < m->f.count; i++) {
      int g = (*gd)[i];
      if (g == paintedId)
        painted++;
      else if (g == 0)
        unpainted++;
      else
        other++;
    }
    fprintf(stderr,
            "poly id=%d painted=%d unpainted=%d other=%d\n",
            paintedId,
            painted,
            unpainted,
            other);
    test_assert(paintedId != 0); // a stroke assigned some nonzero group id
    test_assert(painted > 0);    // faces under the dab got that id
    test_assert(unpainted > 0);  // far faces were value-inited and untouched
    test_assert(other == 0);     // no spurious second id
  }

  // --- and on a mesh with a host default group ---
  // A first-ever `group` layer created by the *bind* has to seed the host's
  // default set, not group 0 (Mesh::ensureFaceGroups' rule) -- otherwise every
  // face a multires stroke missed scatters "no face set" onto its cage face.
  {
    Scene scene(256, 256, /*headless=*/true);
    auto r = script::run(scene,
                         "make_cube subdivs=12 size=0.5\n"
                         "build_spatial leaf_limit=256 depth_limit=8\n"
                         "set_brush_tool tool=polygroup\n"
                         "set_brush radius=0.25 strength=1.0\n"
                         "set_backend backend=cpp\n",
                         ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  default-group line %d: %s\n", r.line_no, r.error.c_str());
      return 1;
    }

    Mesh *m = scene.mesh;
    // 2, not 1: the stroke paints Brush::activeGroup, which defaults to 1, and a
    // default that equals the painted id could not tell the two fills apart.
    m->default_group_id = 2;
    test_assert(!m->f.attrs.has(AttrType::INT, "group"));

    r = script::run(scene, "stroke origin=0,0,0.25 normal=0,0,1\n", ".");
    test_assert(r.ok);
    if (!r.ok) {
      fprintf(stderr, "  default-group stroke line %d: %s\n", r.line_no, r.error.c_str());
      return 1;
    }

    AttrData<int> *gd = m->f.attrs.find_attribute(AttrType::INT, "group").get_data<int>();
    test_assert(gd != nullptr);
    int painted = 0, defaulted = 0, zeroed = 0;
    for (int i = 0; i < m->f.count; i++) {
      int g = (*gd)[i];
      if (g == 1)
        painted++;
      else if (g == 2)
        defaulted++;
      else if (g == 0)
        zeroed++;
    }
    fprintf(stderr,
            "default-group painted=%d defaulted=%d zeroed=%d\n",
            painted,
            defaulted,
            zeroed);
    test_assert(painted > 0);   // the dab still paints activeGroup
    test_assert(defaulted > 0); // and the faces it missed hold the default set
    test_assert(zeroed == 0);   // none of them read as "no group"
  }

  return test_end();
}
