#pragma once

/* Quantization graph (M5).
 *
 * The plan's "T-mesh" is a separatrix-traced coarse quad layout whose sides each
 * carry a real length that quantization snaps to an integer. On singularity-free
 * inputs (grid / cylinder / torus) there are no separatrices to trace, so we
 * realize the *equivalent* integer-grid-map structure directly on the M4 cut
 * graph: every cut edge is a "side" carrying a real translation, and the cut
 * graph's cycles are the "closed loops" whose signed-length sums must vanish in
 * the integer grid. Quantization (quantize_ilp) snaps each side's translation to
 * an integer; loopClosureResidual is the programmatic no-spiral check — every
 * interior-vertex one-ring (the most local closed loop) must compose back to the
 * identity once the translations are integers. */

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::remesh {

// One "side" = one interior cut edge, oriented fa = face(e.c) -> fb = radial. A
// single affine transition (R(90*period), t) must hold at BOTH endpoints, so we
// record the corner-class pair at each endpoint (1 = vertex v(e.c), 2 = the other
// vertex) and tie them to one shared integer translation.
struct QuantGraph {
  litestl::util::Vector<int> edge;                     // mesh edge id
  litestl::util::Vector<int> cla;                      // fa corner class at endpoint 1
  litestl::util::Vector<int> clb;                      // fb corner class at endpoint 1
  litestl::util::Vector<int> cla2;                     // fa corner class at endpoint 2
  litestl::util::Vector<int> clb2;                     // fb corner class at endpoint 2
  litestl::util::Vector<int> period;                   // period jump fa->fb (0..3)
  litestl::util::Vector<int> ga;                       // gauge of fa
  litestl::util::Vector<int> gb;                       // gauge of fb
  litestl::util::Vector<litestl::math::float2> t_real; // realized real translation
  litestl::util::Vector<litestl::math::float2> t_int;  // snapped integer translation
  litestl::util::Vector<int> sideOfEdge;               // [ecap] edge -> side, -1 if none

  int num_sides() const
  {
    return int(edge.size());
  }
};

/* Collect every cut interior edge (.remesh.e.is_cut) into a QuantGraph, recording
 * its corner classes, period and gauges. t_real / t_int are left zero for the
 * solver to fill. */
QuantGraph buildQuantGraph(mesh::Mesh &m, const litestl::util::Vector<int> &cornerClass,
                           const litestl::util::Vector<int> &gauge,
                           const litestl::util::Vector<int> &periodEC);

/* Max over interior (non-singular) vertices of the one-ring loop-closure
 * residual: compose the integer transitions (rotation by 90*period + integer
 * translation g.t_int) around each vertex; a valid integer-grid map returns to
 * the identity, so a nonzero residual means the quantized map spirals there.
 * periodEC is the full per-edge period array. */
double loopClosureResidual(mesh::Mesh &m, const QuantGraph &g,
                           const litestl::util::Vector<int> &periodEC);

} // namespace sculptcore::remesh
