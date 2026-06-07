#include "remesh/field/feature_tag.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "mesh/utils/mesh_validate.h" // faceNewellNormal

#include "litestl/math/vector.h"

#include <cmath>

namespace sculptcore::remesh {

using litestl::math::float3;
using sculptcore::mesh::AttrFlag;
using sculptcore::mesh::BuiltinAttr;
using sculptcore::mesh::Mesh;

void computeFeatureTags(Mesh &m, float sharp_angle)
{
  m.recalc_normals(); // thaws topology

  BuiltinAttr<bool, ".remesh.e.is_sharp", AttrFlag::TEMP> is_sharp;
  BuiltinAttr<bool, ".remesh.e.is_boundary", AttrFlag::TEMP> is_boundary;
  is_sharp.ensure(m.e.attrs);
  is_boundary.ensure(m.e.attrs);

  const float cos_thresh = std::cos(sharp_angle);

  for (int e : m.e) {
    int c1 = m.e.c[e];
    bool boundary = false, sharp = false;

    if (c1 == ELEM_NONE) {
      boundary = true; // wire edge (no incident face)
    } else {
      int c2 = m.c.radial_next[c1];
      if (c2 == c1 || m.c.radial_next[c2] != c1) {
        boundary = true; // open boundary (1 face) or non-manifold (>2)
      } else {
        float3 n1 = mesh::faceNewellNormal(m, m.l.f[m.c.l[c1]]);
        float3 n2 = mesh::faceNewellNormal(m, m.l.f[m.c.l[c2]]);
        float l1 = n1.length(), l2 = n2.length();
        if (l1 > 1e-20f && l2 > 1e-20f) {
          float d = n1.dot(n2) / (l1 * l2);
          d = d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d);
          // Sharp when the dihedral exceeds the threshold (cos below cos thr).
          sharp = d < cos_thresh;
        }
      }
    }

    is_sharp.set(e, sharp);
    is_boundary.set(e, boundary);
  }
}

} // namespace sculptcore::remesh
