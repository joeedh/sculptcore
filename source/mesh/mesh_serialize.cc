#include "mesh_serialize.h"

#include "boundary.h"
#include "deform_pool.h"
#include "mesh.h"

#include "io/binfile.h"
#include "io/compress.h"

#include "litestl/util/map.h"
#include "litestl/util/span.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <istream>
#include <iterator>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>

namespace sculptcore::mesh {

/* Column-oriented mesh payload, lz4hc-compressed inside a BinFile header.
 * See documentation in mesh_serialize.h and the approved plan. Layout:
 *
 *   [BinFile header: magic, LITTLE_ENDIAN|COMPRESSED, version]
 *   uint32 meshFormatVersion
 *   uint32 rawSize          // uncompressed payload length
 *   uint32 compSize         // compressed payload length
 *   uint8  payload[compSize]
 *
 * payload (uncompressed) is five domains in fixed order V,E,C,L,F, each:
 *   uint32 domain; uint32 count; uint32 attrCount
 *   per attr: string name; uint32 type; uint32 flag; uint32 elemSize;
 *             uint32 use (category — v2+; absent in v1, defaults to NONE)
 *             column bytes (count*elemSize; bool = count*1; TOPO ints remapped)
 * then (v3+) the sculpt-layer settings table:
 *   uint32 layerCount
 *   per layer: string name; uint32 mode; uint32 space; int32 parent;
 *              float weight; uint8 enabled; uint8 frozen; float clampFrac
 * then (v6+) the deform-weight pool, the side table an AttrType::WEIGHTS column
 * indexes into (see mesh/deform_pool.h). Slots are written in compacted order,
 * so a column's cells hold dense ids into this table rather than live pool
 * indices; dense 0 is always the empty run. slotCount 0 means "no pool":
 *   uint32 slotCount
 *   per slot: uint32 runLen; per entry: int32 group; float weight
 *   uint32 groupNameCount; per name: string
 */
namespace {

using litestl::util::string;
using litestl::util::Vector;

/* ---- intermediate representation migrations operate on ---- */

struct SerialColumn {
  string name;
  AttrType type = AttrType::NONE;
  AttrFlag flag;
  uint32_t elemSize = 0;
  AttrUse use = AttrUse::NONE;
  Vector<uint8_t> bytes;
};

struct SerialDomain {
  ElemType domain = VERTEX;
  uint32_t count = 0;
  Vector<SerialColumn> cols;
};

struct SerialMesh {
  uint32_t version = 0;
  SerialDomain domains[5];
  Vector<SculptLayerSettings> layers; // v3+ sculpt-layer settings table

  /** v6+ deform-weight pool, flattened: run `i` is `pool_runs[pool_offsets[i] ..
   * pool_offsets[i + 1])`. `has_pool` distinguishes a file that carried no pool
   * from one whose pool holds only the empty run. */
  bool has_pool = false;
  Vector<int> pool_offsets;
  Vector<DeformWeight> pool_runs;
  Vector<string> group_names;
};

/**
 * Slot-index compaction for AttrType::WEIGHTS columns.
 *
 * A cell holds a live DeformPool index, which is arbitrary (sharded, and
 * pockmarked by whatever the last sweep left). Dense ids are assigned in the
 * order the write pass meets them — domains in file order, elements in dense
 * order — so the file's pool has no holes and reloads with the same locality
 * the mesh had. Dense 0 is pinned to the empty run, matching the live pool.
 */
struct WeightRemap {
  DeformPool *pool = nullptr;
  litestl::util::Map<int, int> dense_by_slot;
  Vector<int> slots; // dense id -> live slot index

  explicit WeightRemap(DeformPool *pool_) : pool(pool_)
  {
    if (pool) {
      dense_by_slot[0] = 0;
      slots.append(0);
    }
  }

  int denseOf(int slot)
  {
    int *found = dense_by_slot.lookup_ptr(slot);
    if (found) {
      return *found;
    }
    int dense = int(slots.size());
    dense_by_slot[slot] = dense;
    slots.append(slot);
    return dense;
  }
};

/* Fixed domain order shared by the file layout and the maps[] / eds[] arrays. */
int domainIndex(ElemType t)
{
  switch (t) {
  case VERTEX:
    return 0;
  case EDGE:
    return 1;
  case CORNER:
    return 2;
  case LIST:
    return 3;
  case FACE:
    return 4;
  }
  return -1;
}

/* Byte size of one scalar component of an attribute type (for byte-swapping). */
uint32_t scalarSize(AttrType t)
{
  switch (t) {
  case AttrType::SHORT:
    return 2;
  case AttrType::BYTE:
  case AttrType::BOOL:
    return 1;
  default:
    return 4; /* every float/int variant is built from 4-byte scalars */
  }
}

/* Target domain of a builtin TOPO attribute — a hand-maintained mirror of the
 * remap tables in Mesh::reorder_*. Returns ElemType(0) for unknown names; the
 * writer aborts on that, since custom TOPO attrs are unsupported in v1. */
ElemType topoTarget(const string &name)
{
  auto is = [&](const char *s) { return std::strcmp(name.c_str(), s) == 0; };

  if (is(".vert.e")) {
    return EDGE;
  }
  if (is(".edge.vs")) {
    return VERTEX;
  }
  if (is(".edge.c")) {
    return CORNER;
  }
  if (is(".edge.vs.disk")) {
    return EDGE;
  }
  if (is(".corner.v")) {
    return VERTEX;
  }
  if (is(".corner.e")) {
    return EDGE;
  }
  if (is(".corner.l")) {
    return LIST;
  }
  if (is(".corner.next") || is(".corner.prev") || is(".corner.radial_next") ||
      is(".corner.radial_prev")) {
    return CORNER;
  }
  if (is(".list.c")) {
    return CORNER;
  }
  if (is(".list.f")) {
    return FACE;
  }
  if (is(".list.next")) {
    return LIST;
  }
  if (is(".face.list")) {
    return LIST;
  }
  return ElemType(0);
}

/* Flag bits that MUST be present on a deserialized builtin attribute, keyed by
 * name. These are the non-persistent (TEMP) derived attrs a correct writer drops
 * from the file: the spatial node-ownership ids and the derived/dirty boundary
 * layers. A mesh saved before these flags were assigned (the "improper attribute
 * setup" some old .wproj files have) carries them on disk with stale flags;
 * buildDomain would then restore the stale flag, and a non-NOINTERP
 * .spatial.{v,f}.node makes dyntopo interpolate a parent's leaf-ownership id onto
 * new geometry — a stale index that mis-partitions a freshly built tree and
 * crashes. Re-asserting the canonical bits on load makes such a file behave like
 * one that never serialized them: inert to interpolation/copy, and TEMP-dropped
 * from the next save. Returns NONE for names that aren't known non-persistent
 * builtins (custom + persistent attrs keep their file flags untouched). */
AttrFlag mandatoryBuiltinFlags(const string &name)
{
  auto is = [&](const char *s) { return std::strcmp(name.c_str(), s) == 0; };
  if (is(".spatial.v.node") || is(".spatial.f.node")) {
    return AttrFlag::TEMP | AttrFlag::NOINTERP | AttrFlag::NOCOPY;
  }
  if (is(boundary::EDGE_POLYGROUP) || is(boundary::EDGE_UVCHART) ||
      is(boundary::EDGE_DIRTY) || is(boundary::VERT_DIRTY) ||
      is(boundary::VERT_CLASS)) {
    return AttrFlag::TEMP;
  }
  /* Derived builtins the current writer drops and readMesh rebuilds
   * (rebuildDerivedTopo). A pre-v5 file still carries these columns with flag
   * DERIVED unset; re-assert the bit so the loaded mesh drops them from its next
   * save (same pattern as TEMP). Non-topology derived columns: */
  if (is("normals") || is(".face.normal") || is(".list.size") ||
      is(".face.list_count")) {
    return AttrFlag::DERIVED;
  }
  /* Radial-edge link columns — the disk head, radial cycles, corner edges/loops,
   * and the list back-pointer, all rebuilt from .edge.vs + the face corner loops. */
  if (is(".vert.e") || is(".edge.c") || is(".edge.vs.disk") || is(".corner.e") ||
      is(".corner.l") || is(".corner.prev") || is(".corner.radial_next") ||
      is(".corner.radial_prev") || is(".list.f")) {
    return AttrFlag::TOPO | AttrFlag::DERIVED;
  }
  return AttrFlag::NONE;
}

/* ---- write helpers ---- */

/* Gather a non-bool attribute's live values into @p buf in dense order, indexed
 * by @p map[old] = new. elemSize must equal sizeof(T). */
void gatherColumn(ElemData &ed,
                  AttrRef &ref,
                  Vector<int> &map,
                  uint32_t elemSize,
                  Vector<uint8_t> &buf,
                  uint32_t count)
{
  buf.resize(size_t(count) * elemSize);
  detail::type_dispatch(ref.type, [&]<typename T>() {
    if constexpr (!std::is_same_v<T, bool>) {
      auto *data = static_cast<AttrData<T> *>(ref.data);
      for (int old : ed) {
        int dst = map[old];
        T val = data->safe_get(old);
        std::memcpy(buf.data() + size_t(dst) * elemSize, &val, elemSize);
      }
    }
  });
}

/* Remap every int component of a (dense) topo column through @p targetMap.
 * ELEM_NONE passes through unchanged. @p packedDisk: the column is the
 * side-bit-encoded `.edge.vs.disk` (diskPack) — remap only the id half. */
void remapTopoColumn(Vector<uint8_t> &buf, Vector<int> &targetMap, bool packedDisk)
{
  int32_t *p = reinterpret_cast<int32_t *>(buf.data());
  size_t n = buf.size() / sizeof(int32_t);
  for (size_t i = 0; i < n; i++) {
    int32_t v = p[i];
    if (v == ELEM_NONE) {
      continue;
    }
    p[i] = packedDisk ? diskPack(targetMap[diskEdge(v)], diskSide(v)) : targetMap[v];
  }
}

/** Rewrite a gathered WEIGHTS column from live slot indices to dense ids. */
void remapWeightColumn(Vector<uint8_t> &buf, WeightRemap &remap)
{
  if (!remap.pool) {
    return; // unreachable: AttrGroup::ensure refuses a WEIGHTS column without one
  }
  int32_t *p = reinterpret_cast<int32_t *>(buf.data());
  size_t n = buf.size() / sizeof(int32_t);
  for (size_t i = 0; i < n; i++) {
    p[i] = remap.denseOf(p[i]);
  }
}

void writeDomain(
    io::BinFile &pbf, ElemData &ed, Vector<int> *maps, bool includeTemp, WeightRemap &remap)
{
  Vector<int> &selfMap = maps[domainIndex(ed.domain)];
  uint32_t count = uint32_t(ed.count);

  /* DERIVED columns are always dropped (rebuilt on load); TEMP columns are
   * dropped unless the mesh opts in (Mesh::serialize_temp — debug/repro saves).
   * The format is name+type+flag self-describing, so readers need no change. */
  const AttrFlag skipMask =
      includeTemp ? AttrFlag::DERIVED : (AttrFlag::TEMP | AttrFlag::DERIVED);

  uint32_t attrCount = 0;
  for (AttrRef &attr : ed.attrs.attrs) {
    if (attr.flag & skipMask) {
      continue;
    }
    attrCount++;
  }

  pbf.writeUint32(uint32_t(ed.domain));
  pbf.writeUint32(count);
  pbf.writeUint32(attrCount);

  Vector<uint8_t> buf;
  for (AttrRef &attr : ed.attrs.attrs) {
    if (attr.flag & skipMask) {
      continue;
    }

    uint32_t elemSize = 1;
    if (attr.type != AttrType::BOOL) {
      detail::type_dispatch(attr.type, [&]<typename T>() {
        if constexpr (!std::is_same_v<T, bool>) {
          elemSize = uint32_t(sizeof(T));
        }
      });
    }

    pbf.writeString(attr.name);
    pbf.writeUint32(uint32_t(attr.type));
    pbf.writeUint32(uint32_t(int(attr.flag)));
    pbf.writeUint32(elemSize);
    pbf.writeUint32(uint32_t(int(attr.use))); // category (v2+)

    if (attr.type == AttrType::BOOL) {
      buf.resize(count);
      auto *view = static_cast<BoolAttrView *>(attr.data);
      for (int old : ed) {
        buf[selfMap[old]] = view->get(old) ? 1 : 0;
      }
    } else {
      gatherColumn(ed, attr, selfMap, elemSize, buf, count);
      if (attr.type == AttrType::WEIGHTS) {
        remapWeightColumn(buf, remap);
      }
      /* Identify topology columns by name: attr.flag carries TOPO, but only
       * the name tells us which domain the column references (and thus which
       * remap to apply). The name table is authoritative for that. */
      ElemType tgt = topoTarget(attr.name);
      if (int(tgt) != 0) {
        bool packedDisk = std::strcmp(attr.name.c_str(), ".edge.vs.disk") == 0;
        remapTopoColumn(buf, maps[domainIndex(tgt)], packedDisk);
      } else if (attr.flag & AttrFlag::TOPO) {
        printf("mesh_serialize: unknown TOPO attr '%s' (custom topo attrs "
               "unsupported in v1)\n",
               attr.name.c_str());
        abort();
      }
    }

    if (buf.size() > 0) {
      pbf.stream.write(reinterpret_cast<const char *>(buf.data()),
                       std::streamsize(buf.size()));
    }
  }
}

void writeLayerTable(io::BinFile &pbf, Mesh &mesh)
{
  pbf.writeUint32(uint32_t(mesh.sculptLayers.size()));
  for (SculptLayerSettings &st : mesh.sculptLayers) {
    pbf.writeString(st.name);
    pbf.writeUint32(uint32_t(st.mode));
    pbf.writeUint32(uint32_t(st.space));
    pbf.writeInt32(st.parent);
    pbf.writeFloat(st.weight);
    pbf.writeUint8(st.enabled ? 1 : 0);
    pbf.writeUint8(st.frozen ? 1 : 0);
    pbf.writeFloat(st.clampFrac);
  }
}

/** Write the compacted pool (v6+): runs in dense order, then the vertex group
 * names. Must run after every domain, since that is what fills `remap`. */
void writePoolSection(io::BinFile &pbf, WeightRemap &remap)
{
  if (!remap.pool) {
    pbf.writeUint32(0); // slotCount
    pbf.writeUint32(0); // groupNameCount
    return;
  }

  pbf.writeUint32(uint32_t(remap.slots.size()));

  Vector<DeformWeight> run;
  for (int slot : remap.slots) {
    const int n = remap.pool->runSize(WeightSlot(slot));
    run.resize(size_t(n));
    remap.pool->copyRun(WeightSlot(slot), run.data(), n);

    pbf.writeUint32(uint32_t(n));
    for (int i = 0; i < n; i++) {
      pbf.writeInt32(run[i].group);
      pbf.writeFloat(run[i].weight);
    }
  }

  pbf.writeUint32(uint32_t(remap.pool->group_names.size()));
  for (const string &name : remap.pool->group_names) {
    pbf.writeString(name);
  }
}

/* ---- read helpers ---- */

void swapColumn(SerialColumn &col)
{
  uint32_t ss = scalarSize(col.type);
  if (ss <= 1) {
    return;
  }
  uint8_t *p = col.bytes.data();
  size_t n = col.bytes.size();
  for (size_t off = 0; off + ss <= n; off += ss) {
    for (uint32_t i = 0; i < ss / 2; i++) {
      std::swap(p[off + i], p[off + ss - 1 - i]);
    }
  }
}

void readDomain(io::BinFile &pbf, SerialMesh &sm, bool needSwap, uint32_t version)
{
  uint32_t domRaw = pbf.readUint32();
  uint32_t count = pbf.readUint32();
  uint32_t attrCount = pbf.readUint32();

  SerialDomain &sd = sm.domains[domainIndex(ElemType(domRaw))];
  sd.domain = ElemType(domRaw);
  sd.count = count;
  sd.cols.resize(0);

  for (uint32_t a = 0; a < attrCount; a++) {
    SerialColumn col;
    col.name = pbf.readString();
    col.type = AttrType(pbf.readUint32());
    col.flag = AttrFlag(int(pbf.readUint32()));
    col.elemSize = pbf.readUint32();
    if (version >= 2) {
      col.use = AttrUse(int(pbf.readUint32())); // category (v2+); else NONE
    }

    size_t nbytes = size_t(count) * col.elemSize;
    col.bytes.resize(nbytes);
    if (nbytes > 0) {
      pbf.stream.read(reinterpret_cast<char *>(col.bytes.data()),
                      std::streamsize(nbytes));
      if (needSwap && col.type != AttrType::BOOL) {
        swapColumn(col);
      }
    }
    sd.cols.append(std::move(col));
  }
}

void readLayerTable(io::BinFile &pbf, SerialMesh &sm)
{
  uint32_t n = pbf.readUint32();
  for (uint32_t i = 0; i < n; i++) {
    SculptLayerSettings st;
    st.name = pbf.readString();
    st.mode = int(pbf.readUint32());
    st.space = int(pbf.readUint32());
    st.parent = pbf.readInt32();
    st.weight = pbf.readFloat();
    st.enabled = pbf.readUint8() != 0;
    st.frozen = pbf.readUint8() != 0;
    st.clampFrac = pbf.readFloat();
    sm.layers.append(std::move(st));
  }
}

void readPoolSection(io::BinFile &pbf, SerialMesh &sm)
{
  uint32_t slotCount = pbf.readUint32();
  sm.has_pool = slotCount > 0;

  sm.pool_offsets.append(0);
  for (uint32_t s = 0; s < slotCount; s++) {
    uint32_t runLen = pbf.readUint32();
    for (uint32_t i = 0; i < runLen; i++) {
      DeformWeight w;
      w.group = pbf.readInt32();
      w.weight = pbf.readFloat();
      sm.pool_runs.append(w);
    }
    sm.pool_offsets.append(int(sm.pool_runs.size()));
  }

  uint32_t nameCount = pbf.readUint32();
  for (uint32_t i = 0; i < nameCount; i++) {
    sm.group_names.append(pbf.readString());
  }
}

bool hasWeightColumn(SerialMesh &sm)
{
  for (SerialDomain &sd : sm.domains) {
    for (SerialColumn &col : sd.cols) {
      if (col.type == AttrType::WEIGHTS) {
        return true;
      }
    }
  }
  return false;
}

/* Rebuild one element domain from its serialized columns into a fresh ElemData.
 * Builtins already exist from the domain ctor (ensure is a no-op); custom attrs
 * are created. alloc() on a fresh domain hands out dense 0..count-1, matching
 * the dense column layout, so topology refs (already remapped on write) are
 * written verbatim. */
void buildDomain(ElemData &ed, SerialDomain &sd)
{
  for (SerialColumn &col : sd.cols) {
    ed.attrs.ensure(col.type, col.name);
  }

  /* Preserve attr flags + category from the file (builtins keep their ctor
   * flag; use defaults to NONE for v1 files). Re-assert the mandatory flags for
   * non-persistent builtins so a file with stale/missing flags (e.g. a
   * serialized .spatial.{v,f}.node) can't crash dyntopo — see
   * mandatoryBuiltinFlags. */
  for (AttrRef &attr : ed.attrs.attrs) {
    for (SerialColumn &col : sd.cols) {
      if (attr.type == col.type &&
          std::strcmp(attr.name.c_str(), col.name.c_str()) == 0) {
        attr.flag = col.flag | mandatoryBuiltinFlags(col.name);
        attr.use = col.use;
      }
    }
  }

  for (uint32_t i = 0; i < sd.count; i++) {
    ed.alloc();
  }

  for (SerialColumn &col : sd.cols) {
    AttrRef ref = ed.attrs.find_attribute(col.type, col.name);
    if (!ref.exists()) {
      continue;
    }

    if (col.type == AttrType::WEIGHTS) {
      // A raw copy would install dense ids as slot indices and take no
      // reference; restoreWeights re-interns the runs once the pool is built.
      continue;
    }

    if (col.type == AttrType::BOOL) {
      auto *view = static_cast<BoolAttrView *>(ref.data);
      for (uint32_t i = 0; i < sd.count; i++) {
        view->set(int(i), col.bytes[i] != 0);
      }
    } else {
      detail::type_dispatch(col.type, [&]<typename T>() {
        if constexpr (!std::is_same_v<T, bool>) {
          if (col.elemSize != uint32_t(sizeof(T))) {
            printf("mesh_serialize: elemSize mismatch for '%s' (%u != %zu)\n",
                   col.name.c_str(),
                   col.elemSize,
                   sizeof(T));
            abort();
          }
          auto *data = static_cast<AttrData<T> *>(ref.data);
          for (uint32_t i = 0; i < sd.count; i++) {
            data->materialize(int(i));
            std::memcpy(data->getElemData(int(i)),
                        col.bytes.data() + size_t(i) * col.elemSize,
                        col.elemSize);
          }
        }
      });
    }
  }
}

/** Re-intern the file's pool and rewrite every WEIGHTS column's dense ids as
 * live slots. Runs after buildDomain, which allocated the elements but left
 * those columns at the empty run. The pool itself must already exist —
 * AttrGroup::ensure refuses a WEIGHTS column without one — so readMesh creates
 * it before building the domains. */
void restoreWeights(Mesh &mesh, SerialMesh &sm)
{
  DeformPool *pool = mesh.deformPoolOrNull();
  if (!pool) {
    return;
  }
  pool->group_names = sm.group_names;

  Vector<WeightSlot> dense;
  for (size_t i = 0; i + 1 < sm.pool_offsets.size(); i++) {
    const int start = sm.pool_offsets[int(i)];
    const int end = sm.pool_offsets[int(i) + 1];
    dense.append(pool->intern(
        litestl::util::span<const DeformWeight>(sm.pool_runs.data() + start, end - start)));
  }

  ElemData *eds[5] = {&mesh.v, &mesh.e, &mesh.c, &mesh.l, &mesh.f};
  for (int d = 0; d < 5; d++) {
    SerialDomain &sd = sm.domains[d];
    for (SerialColumn &col : sd.cols) {
      if (col.type != AttrType::WEIGHTS) {
        continue;
      }
      AttrRef ref = eds[d]->attrs.find_attribute(col.type, col.name);
      if (!ref.exists()) {
        continue;
      }

      auto *data = static_cast<AttrData<WeightSlot> *>(ref.data);
      const int32_t *ids = reinterpret_cast<const int32_t *>(col.bytes.data());
      for (uint32_t i = 0; i < sd.count; i++) {
        const int32_t id = ids[i];
        WeightSlot slot = (id >= 0 && size_t(id) < dense.size()) ? dense[id] : WeightSlot(0);
        data->materialize(int(i));
        pool->reassign((*data)[int(i)], slot);
      }
    }
  }

  // Each intern() above handed back a reference of its own; the columns now
  // hold every reference the loaded mesh needs.
  for (WeightSlot slot : dense) {
    pool->release(slot);
  }
}

/* Upgrade @p sm from its file version to kMeshFormatVersion. Returns false for
 * an unmigratable version. Extension point: when bumping kMeshFormatVersion,
 * add a `case N:` that transforms a v_N SerialMesh into v_{N+1} and sets
 * sm.version = N + 1. */
bool migrate(SerialMesh &sm)
{
  while (sm.version < serial::kMeshFormatVersion) {
    switch (sm.version) {
    case 1:
      /* v1 → v2 added the per-attr `use` (category) field. Old files have no
       * categories; readDomain already left every col.use == NONE. */
      sm.version = 2;
      break;
    case 2:
      /* v2 → v3 added the trailing sculpt-layer settings table. Old files
       * have no sculpt layers; sm.layers is already empty. */
      sm.version = 3;
      break;
    case 3: {
      /* v3 → v4: `.edge.vs.disk` links became side-bit encoded (diskPack).
       * Re-encode each link in place using `.edge.vs` for the shared-vert
       * side — order-preserving, so disk iteration order survives the load. */
      SerialDomain &ed = sm.domains[domainIndex(EDGE)];
      SerialColumn *disk = nullptr, *vs = nullptr;
      for (SerialColumn &col : ed.cols) {
        if (std::strcmp(col.name.c_str(), ".edge.vs.disk") == 0) {
          disk = &col;
        } else if (std::strcmp(col.name.c_str(), ".edge.vs") == 0) {
          vs = &col;
        }
      }
      if (disk && vs) {
        int32_t *d = reinterpret_cast<int32_t *>(disk->bytes.data());
        const int32_t *w = reinterpret_cast<const int32_t *>(vs->bytes.data());
        size_t ecount = disk->bytes.size() / (4 * sizeof(int32_t));
        for (size_t ei = 0; ei < ecount; ei++) {
          for (int s = 0; s < 2; s++) {
            int32_t shared = w[ei * 2 + s];
            for (int k = 0; k < 2; k++) {
              int32_t t = d[ei * 4 + s * 2 + k];
              if (t != ELEM_NONE) {
                d[ei * 4 + s * 2 + k] = diskPack(t, w[t * 2 + 0] == shared ? 0 : 1);
              }
            }
          }
        }
      }
      sm.version = 4;
      break;
    }
    case 4:
      /* v4 → v5 dropped the DERIVED columns (normals, ngon counts, and the
       * radial-edge links) from the payload. Nothing to transform in the
       * SerialMesh IR — a v4 file simply carries stale copies of those columns,
       * which serial::readMesh's rebuildDerivedTopo() overwrites after building
       * the live Mesh (migrate has no Mesh to rebuild against). */
      sm.version = 5;
      break;
    case 5: {
      // v6 added the pool section, so a v5 WEIGHTS column indexes a pool that was
      // never written. Zero it to the empty run: no file with real weights
      // predates v6, the pool and the version bump landed together.
      for (SerialDomain &sd : sm.domains) {
        for (SerialColumn &col : sd.cols) {
          if (col.type == AttrType::WEIGHTS && col.bytes.size() > 0) {
            std::memset(col.bytes.data(), 0, col.bytes.size());
          }
        }
      }
      sm.version = 6;
      break;
    }
    default:
      return false;
    }
  }
  return sm.version == serial::kMeshFormatVersion;
}

} // namespace

namespace serial {

bool writeMeshRaw(Mesh &mesh, std::iostream &out)
{
  /* The live TOPO link columns (.edge.vs.disk, .vert.e, .corner.*, …) are freed
   * while the mesh is topo_frozen — the state a mesh is left in after a sculpt
   * stroke (the brush executor freezes for non-live brushes). The column gather
   * below reads those layers, so thaw first to re-materialize them from the
   * frozen snapshot (mirrors the auto-thaw every topology mutator does). */
  if (mesh.topo_frozen) {
    mesh.thawTopo();
  }

  // The edit target's delta column is stale while a layer is targeted (V2
  // implicit-active model) — fold so the file stores the current delta.
  mesh.foldActiveSculptLayer();

  ElemData *eds[5] = {&mesh.v, &mesh.e, &mesh.c, &mesh.l, &mesh.f};

  /* Compaction maps: maps[d][old] = dense new index, ELEM_NONE for free slots. */
  Vector<int> maps[5];
  for (int d = 0; d < 5; d++) {
    ElemData &ed = *eds[d];
    size_t cap = ed.capacity();
    maps[d].resize(cap);
    for (size_t i = 0; i < cap; i++) {
      maps[d][int(i)] = ELEM_NONE;
    }
    int dense = 0;
    for (int old : ed) {
      maps[d][old] = dense++;
    }
  }

  io::BinFile pbf(out); // payload uses no file header; host-endian columns
  WeightRemap remap(mesh.deformPoolOrNull());
  for (int d = 0; d < 5; d++) {
    writeDomain(pbf, *eds[d], maps, mesh.serialize_temp, remap);
  }
  writeLayerTable(pbf, mesh);  // v3+
  writePoolSection(pbf, remap); // v6+; last, so `remap` is complete
  return bool(out);
}

bool writeMesh(Mesh &mesh, std::ostream &out)
{
  /* Split point (autosave plan §5.1): writeMeshRaw produces the uncompressed
   * column payload (a near-memcpy pass, main-thread cheap); the lz4hc step + the
   * BinFile header below is what an autosave worker does off-thread instead. */
  std::stringstream payloadStream(std::ios::in | std::ios::out | std::ios::binary);
  if (!writeMeshRaw(mesh, payloadStream)) {
    return false;
  }

  std::string payload = payloadStream.str();
  size_t rawSize = payload.size();

  Vector<uint8_t> comp;
  size_t compSize = io::compressBlock(payload.data(), rawSize, comp);
  if (compSize == 0) {
    return false;
  }

  std::stringstream fs(std::ios::in | std::ios::out | std::ios::binary);
  io::BinFile obf(fs);
  obf.compressed = true;
  obf.writeHeader();
  obf.writeUint32(kMeshFormatVersion);
  obf.writeUint32(uint32_t(rawSize));
  obf.writeUint32(uint32_t(compSize));
  obf.stream.write(reinterpret_cast<const char *>(comp.data()),
                   std::streamsize(compSize));

  std::string s = fs.str();
  out.write(s.data(), std::streamsize(s.size()));
  return bool(out);
}

bool readMesh(Mesh &mesh, std::istream &in)
{
  std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::stringstream fs(raw, std::ios::in | std::ios::out | std::ios::binary);

  io::BinFile obf(fs);
  if (!obf.readHeader()) {
    return false;
  }
  if (!obf.compressed) {
    return false;
  }

  uint32_t version = obf.readUint32();
  uint32_t rawSize = obf.readUint32();
  uint32_t compSize = obf.readUint32();

  if (version == 0 || version > kMeshFormatVersion) {
    return false;
  }

  Vector<uint8_t> comp;
  comp.resize(compSize);
  if (compSize > 0) {
    obf.stream.read(reinterpret_cast<char *>(comp.data()), std::streamsize(compSize));
  }

  Vector<uint8_t> rawBuf;
  if (!io::decompressBlock(comp.data(), compSize, rawSize, rawBuf)) {
    return false;
  }

  std::string payloadStr(reinterpret_cast<const char *>(rawBuf.data()), rawSize);
  std::stringstream ps(payloadStr, std::ios::in | std::ios::out | std::ios::binary);

  io::BinFile pbf(ps);
  pbf.littleEndian = obf.littleEndian;
  bool needSwap = obf.littleEndian != io::hostLittleEndian;

  SerialMesh sm;
  for (int d = 0; d < 5; d++) {
    readDomain(pbf, sm, needSwap, version);
  }
  if (version >= 3) {
    readLayerTable(pbf, sm);
  }
  if (version >= 6) {
    readPoolSection(pbf, sm);
  }
  sm.version = version;

  if (!migrate(sm)) {
    return false;
  }

  // Create the pool before the columns that index it: AttrGroup::ensure refuses a
  // WEIGHTS column without one, and a pre-v6 file can carry such a column with no
  // pool section behind it.
  if (sm.has_pool || hasWeightColumn(sm)) {
    mesh.deformPool();
  }

  ElemData *eds[5] = {&mesh.v, &mesh.e, &mesh.c, &mesh.l, &mesh.f};
  for (int d = 0; d < 5; d++) {
    buildDomain(*eds[d], sm.domains[d]);
  }
  restoreWeights(mesh, sm);
  mesh.sculptLayers = std::move(sm.layers);

  /* Rebuild every DERIVED column dropped from the blob (disk/radial links, corner
   * prev/l, list back-ptr + counts) from the authoritative columns. Runs for all
   * versions: a pre-v5 file's stale link columns loaded above are overwritten —
   * wasted read, but one always-exercised path. A false return means a face loop
   * referenced an edge absent from .edge.vs (corrupt file); fall back to the full
   * repair, which can synthesize the missing edge. */
  if (!mesh.rebuildDerivedTopo()) {
    mesh.validateAndRepair();
  }

  /* n-gon counter dyntopo's triangulate-prepass skip relies on (over the
   * l.size rebuilt by rebuildDerivedTopo). */
  mesh.recountNgons();

  /* Normals (v.normals / .face.normal) are DERIVED and dropped; recompute after
   * the link rebuild, since vertex normals walk the disk/radial cycles. */
  mesh.recalc_normals();
  /* The derived boundary overlay (EDGE_POLYGROUP / VERT_CLASS) is TEMP and not
   * serialized, and boundaryDirty defaults false — so a freshly loaded mesh
   * carries the source flags (seam/sharp/group) but no recomputed classification.
   * Mark everything dirty so the first buildSeamBatch / stroke rebuilds it. */
  boundary::markAllDirty(&mesh);
  return true;
}

} // namespace serial
} // namespace sculptcore::mesh
