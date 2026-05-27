#pragma once

#include <cstdint>
#include <iosfwd>

namespace sculptcore::mesh {
struct Mesh;

namespace serial {

/* Bump when the on-disk layout or attribute semantics change, and register a
 * migration in mesh_serialize.cc::migrate() that upgrades the previous version
 * to this one. See that function for the extension point. */
constexpr uint32_t kMeshFormatVersion = 1;

/* Serialize every element domain of @p mesh — all builtin and custom
 * attributes plus topology — to @p out as a versioned, lz4hc-compressed blob.
 *
 * Dead/free element slots are compacted away: live elements are renumbered to
 * a dense [0, count) range and topology references are remapped accordingly,
 * so the live mesh is left untouched and the file holds no holes. Attributes
 * flagged AttrFlag::TEMP are dropped. Returns false on write failure.
 *
 * Not const because attribute reads and element iteration go through
 * non-const accessors; the mesh is not logically modified. */
bool writeMesh(Mesh &mesh, std::ostream &out);

/* Read a blob produced by writeMesh into @p mesh, which must be freshly
 * constructed (empty). Returns false on bad magic, a missing-compression flag,
 * decompression failure, or an unsupported/unmigratable version. */
bool readMesh(Mesh &mesh, std::istream &in);

} // namespace serial
} // namespace sculptcore::mesh
