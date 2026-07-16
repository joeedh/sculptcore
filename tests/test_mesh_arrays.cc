#include "test_util.h"

#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <cstdio>
#include <cstring>

// Blender-layout bulk conversion API (mesh/c-api/mesh_c_api.cc).
extern "C" {
sculptcore::mesh::Mesh *Mesh_fromArrays(const float *positions,
                                        int verts_num,
                                        const int *corner_verts,
                                        int corners_num,
                                        const int *face_offsets,
                                        int faces_num);
void Mesh_arraySizes(sculptcore::mesh::Mesh *m,
                     int *r_verts_num,
                     int *r_corners_num,
                     int *r_faces_num,
                     int *r_verts_domain_size);
int Mesh_toArrays(sculptcore::mesh::Mesh *m,
                  float *positions,
                  int *corner_verts,
                  int *face_offsets,
                  int *r_vert_map);
}

test_init;

using litestl::util::Vector;

struct Arrays {
  Vector<float> positions;
  Vector<int> corner_verts;
  Vector<int> face_offsets;
  Vector<int> vert_map;
  int remapped = 0;
};

static Arrays export_arrays(sculptcore::mesh::Mesh *m)
{
  Arrays out;
  int verts_num = 0, corners_num = 0, faces_num = 0, domain_size = 0;
  Mesh_arraySizes(m, &verts_num, &corners_num, &faces_num, &domain_size);
  out.positions.resize(verts_num * 3);
  out.corner_verts.resize(corners_num);
  out.face_offsets.resize(faces_num + 1);
  out.vert_map.resize(domain_size);
  out.remapped = Mesh_toArrays(
      m, out.positions.data(), out.corner_verts.data(), out.face_offsets.data(),
      out.vert_map.data());
  return out;
}

int main()
{
  using namespace sculptcore::mesh;
  using namespace litestl;

  // Mixed quad/tri/n-gon sheet plus one loose vertex:
  //   6 -- 5 -- 4
  //   | f1 . f0 |     f0 = pentagon (0,1,4,5,2)? keep simple:
  //   0 -- 1 -- 2 ... vertex 7 is loose.
  {
    const float positions[] = {
        0, 0, 0, /**/ 1, 0, 0, /**/ 2, 0, 0, /**/ 3, 0, 0,
        0, 1, 0, /**/ 1, 1, 0, /**/ 2, 1, 0, /**/ 9, 9, 9,
    };
    const int corner_verts[] = {
        0, 1, 5, 4,      /* quad */
        1, 2, 5,         /* tri */
        2, 3, 6, 5,      /* quad — makes vertex 5 a 4-face hub */
    };
    const int face_offsets[] = {0, 4, 7, 11};

    Mesh *m = Mesh_fromArrays(positions, 8, corner_verts, 11, face_offsets, 3);
    test_assert(m != nullptr);
    test_assert(m->v.count == 8); /* loose vertex 7 kept */
    test_assert(m->f.count == 3);
    test_assert(m->ngonFaceCount() == 2); /* the two quads */
    test_assert(m->repairMesh() == 0);    /* topology is valid */

    Arrays a = export_arrays(m);
    test_assert(a.remapped == 0); /* fresh build: identity indices */
    test_assert(int(a.positions.size()) == 8 * 3);
    test_assert(int(a.corner_verts.size()) == 11);
    test_assert(a.face_offsets[3] == 11);
    test_assert(std::memcmp(a.positions.data(), positions, sizeof(positions)) == 0);
    test_assert(std::memcmp(a.corner_verts.data(), corner_verts, sizeof(corner_verts)) == 0);
    test_assert(std::memcmp(a.face_offsets.data(), face_offsets, sizeof(face_offsets)) == 0);

    alloc::Delete<Mesh>(m);
  }

  // Freelist gaps: killing a face + its exclusive vertex forces a remap.
  {
    const float positions[] = {
        0, 0, 0, /**/ 1, 0, 0, /**/ 0, 1, 0, /**/ 5, 0, 0, /**/ 6, 0, 0, /**/ 5, 1, 0,
    };
    const int corner_verts[] = {0, 1, 2, 3, 4, 5};
    const int face_offsets[] = {0, 3, 6};

    Mesh *m = Mesh_fromArrays(positions, 6, corner_verts, 6, face_offsets, 2);
    test_assert(m != nullptr);

    /* Kill the first triangle and its now-loose vertices. */
    int f0 = -1;
    for (int fi : m->f) {
      f0 = fi;
      break;
    }
    m->kill_face(f0);
    m->kill_vertex(0);
    m->kill_vertex(1);
    m->kill_vertex(2);

    Arrays a = export_arrays(m);
    test_assert(a.remapped == 1);
    test_assert(int(a.positions.size()) == 3 * 3);
    test_assert(a.face_offsets[1] == 3);
    /* Engine verts 3,4,5 export as 0,1,2; the map records it. */
    test_assert(a.vert_map[0] == -1 && a.vert_map[1] == -1 && a.vert_map[2] == -1);
    test_assert(a.vert_map[3] == 0 && a.vert_map[4] == 1 && a.vert_map[5] == 2);
    test_assert(a.positions[0] == 5.0f && a.positions[3] == 6.0f && a.positions[7] == 1.0f);
    test_assert(a.corner_verts[0] == 0 && a.corner_verts[1] == 1 && a.corner_verts[2] == 2);

    alloc::Delete<Mesh>(m);
  }

  // Round-trip stability on a real shape: export(cube) -> import -> export
  // must be byte-identical.
  {
    Mesh *cube = createCube(8);
    Arrays a = export_arrays(cube);

    Mesh *m2 = Mesh_fromArrays(a.positions.data(),
                               int(a.positions.size()) / 3,
                               a.corner_verts.data(),
                               int(a.corner_verts.size()),
                               a.face_offsets.data(),
                               int(a.face_offsets.size()) - 1);
    test_assert(m2 != nullptr);
    test_assert(m2->repairMesh() == 0);

    Arrays b = export_arrays(m2);
    test_assert(b.remapped == 0);
    test_assert(a.positions.size() == b.positions.size());
    test_assert(a.corner_verts.size() == b.corner_verts.size());
    test_assert(std::memcmp(a.positions.data(),
                            b.positions.data(),
                            a.positions.size() * sizeof(float)) == 0);
    test_assert(std::memcmp(a.corner_verts.data(),
                            b.corner_verts.data(),
                            a.corner_verts.size() * sizeof(int)) == 0);
    test_assert(std::memcmp(a.face_offsets.data(),
                            b.face_offsets.data(),
                            a.face_offsets.size() * sizeof(int)) == 0);

    alloc::Delete<Mesh>(cube);
    alloc::Delete<Mesh>(m2);
  }

  // Invalid input: mismatched offsets are rejected, bad faces are skipped.
  {
    const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const int corner_verts[] = {0, 1, 2};
    const int bad_offsets[] = {0, 2}; /* face with 2 corners */
    Mesh *m = Mesh_fromArrays(positions, 3, corner_verts, 2, bad_offsets, 1);
    test_assert(m != nullptr && m->f.count == 0); /* degenerate face skipped */
    alloc::Delete<Mesh>(m);

    const int wrong_total[] = {0, 5};
    test_assert(Mesh_fromArrays(positions, 3, corner_verts, 3, wrong_total, 1) == nullptr);
  }

  return test_end();
}
