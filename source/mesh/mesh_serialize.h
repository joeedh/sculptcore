#pragma once

#include <cstdint>
#include <iosfwd>

namespace sculptcore::mesh {
struct Mesh;

namespace serial {

/* Bump when the on-disk layout or attribute semantics change, and register a
 * migration in mesh_serialize.cc::migrate() that upgrades the previous version
 * to this one. See that function for the extension point. */
constexpr uint32_t kMeshFormatVersion = 3;

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

/* The uncompressed half of writeMesh: writes only the column-oriented payload
 * (the same five-domain dump, host-endian, no BinFile header or lz4 step) to
 * @p out. The autosave split path (plan §5.1) grabs this cheap snapshot on the
 * main thread and runs the lz4hc compression + header framing off-thread, where
 * the JS lz4 codec (scripts/util/lz4.ts) reproduces writeMesh's container.
 * @p out must be an iostream (BinFile needs read+write); returns false on
 * write failure. */
bool writeMeshRaw(Mesh &mesh, std::iostream &out);

/* Read a blob produced by writeMesh into @p mesh, which must be freshly
 * constructed (empty). Returns false on bad magic, a missing-compression flag,
 * decompression failure, or an unsupported/unmigratable version. */
bool readMesh(Mesh &mesh, std::istream &in);

} // namespace serial
} // namespace sculptcore::mesh
