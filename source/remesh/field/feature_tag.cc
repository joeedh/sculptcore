#include "remesh/field/feature_tag.h"

#include "mesh/attribute_builtin.h"
#include "mesh/mesh.h"
#include "mesh/utils/mesh_validate.h" // faceNewellNormal

#include "litestl/math/vector.h"
#include "litestl/util/boolvector.h"
#include "litestl/util/vector.h"

#include <cmath>

namespace sculptcore::remesh {

using litestl::math::float3;
using litestl::util::BoolVector;
using litestl::util::Vector;
using sculptcore::mesh::AttrFlag;
using sculptcore::mesh::BuiltinAttr;
using sculptcore::mesh::Mesh;

void computeFeatureTags(Mesh &m, float sharp_angle, float feature_hysteresis)
{
  m.recalc_normals(); // thaws topology

  BuiltinAttr<bool, ".remesh.e.is_sharp", AttrFlag::TEMP> is_sharp;
  BuiltinAttr<bool, ".remesh.e.is_boundary", AttrFlag::TEMP> is_boundary;
  is_sharp.ensure(m.e.attrs);
  is_boundary.ensure(m.e.attrs);

  const float cos_strong = std::cos(sharp_angle);

  // Tier 7a: weak threshold = sharp_angle - hysteresis, clamped to
  // [0, sharp_angle] so a large band can't drive it negative (which would
  // weak-tag nearly every edge).
  float hyst = feature_hysteresis;
  hyst = hyst < 0.0f ? 0.0f : (hyst > sharp_angle ? sharp_angle : hyst);
  const bool do_hyst = hyst > 0.0f;
  const float cos_weak = std::cos(sharp_angle - hyst);

  BoolVector<> weak;
  Vector<int> stack;
  if (do_hyst) {
    weak.resize(m.e.capacity());
  }

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
          sharp = d < cos_strong;
          if (do_hyst && !sharp && d < cos_weak) {
            weak.set(e, true); // weak-sharp candidate, kept only if connected
          }
        }
      }
    }

    is_sharp.set(e, sharp);
    is_boundary.set(e, boundary);
    if (do_hyst && sharp) {
      stack.append(e);
    }
  }

  // Tier 7a flood: a weak edge joins the sharp set when it shares a vertex
  // with a strong edge or an already-kept weak edge.
  while (stack.size()) {
    int e = stack.pop_back();
    for (int side = 0; side < 2; side++) {
      int v = m.e.vs[e][side];
      int e0 = m.v.e[v];
      if (e0 == ELEM_NONE) {
        continue;
      }
      int ec = e0;
      do {
        if (weak[ec] && !is_sharp[ec]) {
          is_sharp.set(ec, true);
          stack.append(ec);
        }
        int vside = m.e.vs[ec][0] == v ? 0 : 1;
        ec = m.e.disk[ec][vside * 2 + 1];
      } while (ec != e0);
    }
  }
}

} // namespace sculptcore::remesh
