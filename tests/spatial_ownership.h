#pragma once

#include "mesh/mesh.h"
#include "spatial/spatial.h"

#include <cstdio>

namespace sculptcore::test {

/** Checks that every live face and vert is owned by exactly one leaf, that owner ids agree,
 * and that the per-leaf unique lists hold only live elements. Returns the owned-face count,
 * or -1 on any inconsistency. */
inline int validateOwnership(spatial::SpatialTree *tree, mesh::Mesh *m, const char *tag)
{
  int owned = 0;
  auto leaves = tree->leaves();
  for (auto *leaf : leaves) {
    if (!leaf->data) {
      continue;
    }
    for (int f : leaf->data->unique_faces) {
      if (m->f.freemap[f]) {
        fprintf(stderr, "[%s] leaf %d owns dead face %d\n", tag, leaf->id, f);
        return -1;
      }
      if (tree->treeMesh.f.node[f] != leaf->id) {
        fprintf(stderr,
                "[%s] face %d owner %d != leaf %d\n",
                tag,
                f,
                tree->treeMesh.f.node[f],
                leaf->id);
        return -1;
      }
      owned++;
    }
    /* Vert ownership: a regression here (new verts mis-attributed to a parent's
     * leaf) makes node_needs_split undercount, so the tree never rebalances. */
    for (int v : leaf->data->unique_verts) {
      if (m->v.freemap[v]) {
        fprintf(stderr, "[%s] leaf %d owns dead vert %d\n", tag, leaf->id, v);
        return -1;
      }
      if (tree->treeMesh.v.node[v] != leaf->id) {
        fprintf(stderr,
                "[%s] vert %d owner %d != leaf %d\n",
                tag,
                v,
                tree->treeMesh.v.node[v],
                leaf->id);
        return -1;
      }
    }
  }
  /* Every live face and vert is owned by exactly one leaf (complete coverage). */
  int ownedV = 0;
  for (auto *leaf : leaves) {
    if (leaf->data)
      ownedV += int(leaf->data->unique_verts.size());
  }
  if (ownedV != m->v.count) {
    fprintf(stderr, "[%s] %d verts owned but mesh has %d\n", tag, ownedV, m->v.count);
    return -1;
  }
  for (int f : m->f) {
    if (tree->treeMesh.f.node[f] == 0) {
      fprintf(stderr, "[%s] live face %d is unowned\n", tag, f);
      return -1;
    }
  }
  return owned;
}

} // namespace sculptcore::test
