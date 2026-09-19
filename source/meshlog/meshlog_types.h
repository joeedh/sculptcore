/**
# Meshlog shared types

Enum tags and the `LogChunk` polymorphic base every chunk type
(`LogChunkElems`, `LogChunkTopo`, `LogChunkReorder`) derives from. Split out
of `meshlog_base.h` so the per-chunk-type headers (`meshlog_chunk.h`,
`meshlog_row.h`, `meshlog_topo.h`, `meshlog_reorder.h`) can depend on it
without depending on each other or on `MeshLog` itself.
*/

#pragma once

#include "litestl/math/vector.h"
#include "litestl/util/compiler_util.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"
#include "mesh/mesh_enums.h"
#include "spatial/spatial.h"
#include <cstdint>

namespace sculptcore::meshlog {
enum _LogChunkTypes {
  Topo = 1,
  Reorder = 2,
  Elems = 3,
  /* A foreign undo channel riding the step (e.g. the VDM tile-delta chunk,
   * source/vdm/vdm_undo.h): opaque to MeshLog beyond the undo/redo virtuals. */
  External = 4,
  PreparedData = 5,
};
MAKE_ENUM_CLASS(LogChunkTypes, _LogChunkTypes, int);

enum class LogElemKind : uint8_t { Vert = 0, Edge = 1, Corner = 2, List = 3, Face = 4 };
enum class LogOrigin : uint8_t { Existed, Created };
enum class LogFate : uint8_t { Live, Dead };

struct LogChunk {
  LogChunkTypes type;
  LogChunk(LogChunkTypes type) : type(type)
  {
  }
  virtual ~LogChunk()
  {
  }
  virtual void undo(mesh::Mesh *m, spatial::SpatialTree *tree)
  {
  }
  virtual void redo(mesh::Mesh *m, spatial::SpatialTree *tree)
  {
  }
  /** Estimated heap bytes retained by this chunk (undo memory accounting). */
  virtual double memSize()
  {
    return double(sizeof(LogChunk));
  }
};

} // namespace sculptcore::meshlog
