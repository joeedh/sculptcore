# Convenience gdb commands for sculptcore debugging.
# Source via tools/gdb/run-gdb.sh / run-gdb.ps1, or by hand:
#   (gdb) source tools/gdb/helpers.gdb

set print pretty on
set print thread-events off
set pagination off

# --- sc-mesh-summary <Mesh*> ---
# Print a one-line summary of a Mesh: counts + AABB.
define sc-mesh-summary
  if $argc != 1
    echo Usage: sc-mesh-summary <Mesh*>\n
  else
    printf "Mesh @%p\n", $arg0
    printf "  v=%d e=%d c=%d f=%d\n", \
      ((sculptcore::mesh::Mesh *)$arg0)->v.count, \
      ((sculptcore::mesh::Mesh *)$arg0)->e.count, \
      ((sculptcore::mesh::Mesh *)$arg0)->c.count, \
      ((sculptcore::mesh::Mesh *)$arg0)->f.count
  end
end
document sc-mesh-summary
  Usage: sc-mesh-summary <Mesh*>
  Prints vert/edge/corner/face counts for a Mesh pointer.
end

# --- sc-spatial-walk <SpatialTree*> ---
# Walk the tree's `nodes` vector and print one line per node.
define sc-spatial-walk
  if $argc != 1
    echo Usage: sc-spatial-walk <SpatialTree*>\n
  else
    set $tree = (sculptcore::spatial::SpatialTree *)$arg0
    set $n = $tree->nodes.size_
    printf "SpatialTree @%p — %d nodes\n", $tree, $n
    set $i = 0
    while $i < $n
      set $node = $tree->nodes.data_[$i]
      printf "  [%d] ", $i
      output *$node
      printf "\n"
      set $i = $i + 1
    end
  end
end
document sc-spatial-walk
  Usage: sc-spatial-walk <SpatialTree*>
  Walks the tree's node vector and prints each (uses the SpatialNode pretty
  printer for the body).
end

# --- sc-break-bad-topo ---
# Stop wherever the mesh callbacks fire kill_* on the same index twice in a
# row — a common signature of half-deleted topology. Placeholder breakpoints
# at functions you'll most often want to inspect during a topo bug.
define sc-break-bad-topo
  rbreak sculptcore::mesh::Mesh::kill_vertex
  rbreak sculptcore::mesh::Mesh::kill_edge
  rbreak sculptcore::mesh::Mesh::kill_face
  echo Set breakpoints on Mesh::kill_{vertex,edge,face}\n
end
document sc-break-bad-topo
  Sets breakpoints on Mesh::kill_vertex / kill_edge / kill_face. Useful when
  hunting a non-manifold/dangling-corner regression.
end
