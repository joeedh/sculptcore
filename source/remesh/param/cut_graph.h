#pragma once

/* Cut graph for seamless parametrization (M4).
 *
 * A dual spanning tree is grown over the faces through interior manifold edges;
 * the complementary cotree edges are tagged as cuts in .remesh.e.is_cut. The
 * non-cut face adjacency is then a tree (a topological disk), which is exactly
 * what makes the per-face gauge rotation in seamless_param well defined, and
 * every interior vertex — hence every singularity and every handle loop — is
 * incident to at least one cut edge (the faces around an interior vertex form a
 * cycle the tree cannot fully contain), so all field holonomy is opened.
 *
 * XXX: the cotree cut is correct but non-minimal (it cuts every non-tree edge).
 * A minimal cut — shortest singularity-connecting paths plus a 2g homology basis
 * (tree/cotree leaf-trimming, Bommes 2009) — would mean far fewer seams and a
 * lighter M5 quantization; revisit when wiring M5/M6. */

namespace sculptcore::mesh {
struct Mesh;
}

namespace sculptcore::remesh {

struct CutGraphStats {
  int num_cut_edges = 0;     // cotree (cut) interior edges
  int num_tree_edges = 0;    // dual spanning-tree (non-cut) interior edges
  int num_singularities = 0; // interior verts with nonzero .remesh.v.pole_index
};

/* Tag cut edges in .remesh.e.is_cut (bool, TEMP). Roots the dual tree at a
 * singular face when .remesh.v.pole_index is present, else at the first face. */
CutGraphStats buildCutGraph(mesh::Mesh &m);

} // namespace sculptcore::remesh
