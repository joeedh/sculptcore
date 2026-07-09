#include "mesh.h"

#include "boundary.h"
#include "mesh_path.h"
#include "utils/mesh_validate.h" // faceNewellNormal
#include "utils/modeling_walk.h" // box-modeling loop/boundary walks
#include "utils/select_derive.h" // selection derivation + region/movable queries
#include "utils/symmetrize.h"
#include "uvgen.h"

#include "litestl/math/geom.h"
#include "litestl/util/index_range.h"
#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdlib>

using namespace litestl;
using namespace litestl::util;
using namespace litestl::math;

namespace sculptcore::mesh {

/* Locality-aware allocation (alloc_near hints) is on by default; set
 * SCULPTCORE_NO_LOCALITY_ALLOC=1 to fall back to plain alloc() for A/B
 * profiling of the DRAM-defragmentation win. Read once. */
static bool locality_alloc_enabled()
{
  static const bool v = [] {
    const char *s = std::getenv("SCULPTCORE_NO_LOCALITY_ALLOC");
    return !(s && s[0] && s[0] != '0');
  }();
  return v;
}

void Mesh::recalc_normals()
{
  /* Walks the live loop/disk links; thaw if frozen. Not in the sculpt hot path
   * (the spatial tree owns per-frame normals), so the thaw cost is irrelevant. */
  if (topo_frozen)
    thawTopo();

  auto &no = f.no;
  for (int fi : IndexRange(0, f.capacity())) {
    if (f.freemap[fi]) {
      continue;
    }

    int li = f.l[fi];
    int ci = l.c[li];

    float3 &co1 = v.co[c.v[ci]];
    float3 &co2 = v.co[c.v[c.next[ci]]];
    float3 &co3 = v.co[c.v[c.next[c.next[ci]]]];

    f.no[fi] = triNormal(co1, co2, co3);
  }

  for (int vi : IndexRange(0, v.capacity())) {
    if (v.freemap[vi]) {
      continue;
    }
    v.no[vi] = float3();
    VertProxy vert(this, vi);

    for (EdgeProxy edge : vert.edges()) {
      int ci = e.c[edge.i];
      if (ci == ELEM_NONE) {
        continue;
      }

      int fi = l.f[c.l[ci]];
      v.no[vi] += f.no[fi];
    }

    v.no[vi].normalize();
  }
}

namespace {
inline void fire(const util::function<void(int)> &cb, int idx)
{
  if (cb) {
    cb(idx);
  }
}
} // namespace

void Mesh::freezeTopo()
{
  if (topo_frozen) {
    return;
  }

  /* Snapshot the live topology, then keep the brush 1-ring CSR current so the
   * sculpt path is served from cached data while the live links are gone. */
  topo_cache.frozen.build(*this);
  topo_cache.ensureRing1(*this);

  v.attrs.freeTopoPages();
  e.attrs.freeTopoPages();
  c.attrs.freeTopoPages();
  l.attrs.freeTopoPages();
  f.attrs.freeTopoPages();

  topo_frozen = true;
}

void Mesh::thawTopo()
{
  if (!topo_frozen) {
    return;
  }

  v.attrs.materializeTopoPages();
  e.attrs.materializeTopoPages();
  c.attrs.materializeTopoPages();
  l.attrs.materializeTopoPages();
  f.attrs.materializeTopoPages();

  topo_cache.frozen.rebuildLinks(*this);
  topo_frozen = false;
}

// kind 0 = EDGE_SEAM (user-marked seam), 1 = EDGE_SHARP (sharp crease). Both are
// source-of-truth boundary flags; the marking tool selects via `kind`.
static const char *edgeFlagNameForKind(int kind)
{
  return kind == 1 ? boundary::EDGE_SHARP : boundary::EDGE_SEAM;
}

int Mesh::markEdgePath(int vStart, int vEnd, int kind, int state)
{
  // shortestEdgePath + find_edge walk the live disk links; thaw if a prior
  // sculpt stroke left the mesh frozen.
  if (topo_frozen) {
    thawTopo();
  }
  util::Vector<int> path;
  if (!shortestEdgePath(this, vStart, vEnd, path) || path.size() < 2) {
    return -1;
  }
  const char *flag = edgeFlagNameForKind(kind);
  int marked = 0;
  for (int i = 0; i + 1 < int(path.size()); i++) {
    int e = find_edge(path[i], path[i + 1]);
    if (e != ELEM_NONE) {
      boundary::setEdgeFlag(this, flag, e, state != 0);
      marked++;
    }
  }
  boundary::recomputeDirty(this);
  return marked;
}

int Mesh::markSeamPath(int vStart, int vEnd, int state)
{
  return markEdgePath(vStart, vEnd, 0, state);
}

int Mesh::edgeFlagKind(int e, int kind)
{
  return boundary::edgeFlag(this, edgeFlagNameForKind(kind), e) ? 1 : 0;
}

void Mesh::setEdgeFlagKind(int e, int kind, int state)
{
  boundary::setEdgeFlag(this, edgeFlagNameForKind(kind), e, state != 0);
}

int Mesh::markSharpByAngle(float angle, int state)
{
  if (topo_frozen) {
    thawTopo();
  }
  const float cos_thr = std::cos(angle); // dihedral exceeds `angle` ⇔ cos below this
  int marked = 0;
  for (int e : this->e) {
    // Collect the (up to) two faces incident to e via its radial cycle.
    int c0 = this->e.c[e];
    if (c0 == ELEM_NONE) {
      continue; // wire edge
    }
    int fA = ELEM_NONE, fB = ELEM_NONE;
    int c = c0;
    do {
      int f = this->l.f[this->c.l[c]];
      if (f != fA && f != fB) {
        if (fA == ELEM_NONE) {
          fA = f;
        } else if (fB == ELEM_NONE) {
          fB = f;
        }
      }
      c = this->c.radial_next[c];
    } while (c != c0 && c != ELEM_NONE);
    if (fA == ELEM_NONE || fB == ELEM_NONE) {
      continue; // open boundary (1 face) — not a dihedral crease
    }
    math::float3 n1 = mesh::faceNewellNormal(*this, fA);
    math::float3 n2 = mesh::faceNewellNormal(*this, fB);
    float l1 = n1.length(), l2 = n2.length();
    if (l1 <= 1e-20f || l2 <= 1e-20f) {
      continue;
    }
    float d = n1.dot(n2) / (l1 * l2);
    d = d < -1.0f ? -1.0f : (d > 1.0f ? 1.0f : d);
    if (d < cos_thr) {
      boundary::setEdgeFlag(this, boundary::EDGE_SHARP, e, state != 0);
      marked++;
    }
  }
  boundary::recomputeDirty(this);
  return marked;
}

int Mesh::validateAndRepair(const std::function<void(const char *)> &log)
{
  if (topo_frozen) {
    thawTopo();
  }
  int errors = 0;
  char buf[256];
  auto report = [&](const char *msg) {
    errors++;
    fprintf(stderr, "meshrepair: %s\n", msg);
    repairLog.push_back(std::string(msg));
    if (log) {
      log(msg);
    }
  };

  const int vcap = int(v.capacity());
  const int ccap = int(c.capacity());
  auto vertOk = [&](int vi) { return vi >= 0 && vi < vcap && !v.freemap[vi]; };

  // Pass 1: edges with out-of-range / freed / degenerate endpoints — unrepairable.
  util::Vector<int> killEdges;
  for (int ei : this->e) {
    int a = e.vs[ei][0], b = e.vs[ei][1];
    if (!vertOk(a) || !vertOk(b) || a == b) {
      snprintf(buf, sizeof(buf), "edge %d bad endpoints %d,%d", ei, a, b);
      report(buf);
      killEdges.append(ei);
    }
  }

  // Pass 2: faces whose corner loop is broken or references a dead vert.
  util::Vector<int> killFaces;
  for (int fi : this->f) {
    if (f.list_count[fi] != 1) {
      snprintf(buf, sizeof(buf), "face %d has %d loops", fi, int(f.list_count[fi]));
      report(buf);
      killFaces.append(fi);
      continue;
    }
    int li = f.l[fi], c0 = l.c[li], cc = c0, n = 0;
    bool bad = c0 < 0 || c0 >= ccap;
    while (!bad) {
      if (cc < 0 || cc >= ccap || c.l[cc] != li || !vertOk(c.v[cc])) {
        bad = true;
        break;
      }
      int cn = c.next[cc];
      if (cn < 0 || cn >= ccap || c.prev[cn] != cc) {
        bad = true;
        break;
      }
      cc = cn;
      if (++n > 1000000) {
        bad = true;
        break;
      }
      if (cc == c0) {
        break;
      }
    }
    if (bad) {
      snprintf(buf, sizeof(buf), "face %d broken corner loop", fi);
      report(buf);
      killFaces.append(fi);
    }
  }

  // Pass 3: count broken vertex-disk cycles (repaired by the rebuild below).
  for (int vi : this->v) {
    int e0 = v.e[vi];
    if (e0 == ELEM_NONE) {
      continue;
    }
    if (e0 < 0 || e0 >= int(e.capacity()) || e.freemap[e0]) {
      snprintf(buf, sizeof(buf), "vert %d disk head is invalid edge %d", vi, e0);
      report(buf);
      continue;
    }
    int steps = 0, ec = e0;
    do {
      int side = e.vs[ec][0] == vi ? 0 : 1;
      int next = e.disk[ec][side * 2 + 1], prev = e.disk[ec][side * 2];
      if (next < 0 || next >= int(e.capacity()) || prev < 0 || prev >= int(e.capacity()) ||
          e.freemap[next] || e.freemap[prev]) {
        snprintf(buf, sizeof(buf), "vert %d disk link invalid at edge %d", vi, ec);
        report(buf);
        break;
      }
      int sn = e.vs[next][0] == vi ? 0 : 1, sp = e.vs[prev][0] == vi ? 0 : 1;
      if (e.disk[next][sn * 2] != ec || e.disk[prev][sp * 2 + 1] != ec) {
        snprintf(buf, sizeof(buf), "vert %d disk prev/next mismatch at edge %d", vi, ec);
        report(buf);
        break;
      }
      ec = next;
      if (++steps > 1000000) {
        snprintf(buf, sizeof(buf), "vert %d disk did not close", vi);
        report(buf);
        break;
      }
    } while (ec != e0);
  }

  // Pass 3b: count broken edge-radial cycles (also repaired by the rebuild).
  for (int ei : this->e) {
    int c0 = e.c[ei];
    if (c0 == ELEM_NONE) {
      continue;
    }
    if (c0 < 0 || c0 >= ccap) {
      snprintf(buf, sizeof(buf), "edge %d radial head is invalid corner %d", ei, c0);
      report(buf);
      continue;
    }
    int steps = 0, cc = c0;
    do {
      if (cc < 0 || cc >= ccap || c.e[cc] != ei) {
        snprintf(buf, sizeof(buf), "edge %d radial corner %d mismatch", ei, cc);
        report(buf);
        break;
      }
      int rn = c.radial_next[cc], rp = c.radial_prev[cc];
      if (rn < 0 || rn >= ccap || rp < 0 || rp >= ccap || c.radial_prev[rn] != cc ||
          c.radial_next[rp] != cc) {
        snprintf(buf, sizeof(buf), "edge %d radial prev/next mismatch at corner %d", ei, cc);
        report(buf);
        break;
      }
      cc = rn;
      if (++steps > 1000000) {
        snprintf(buf, sizeof(buf), "edge %d radial did not close", ei);
        report(buf);
        break;
      }
    } while (cc != c0);
  }

  // Clean mesh: skip the O(E) disk/radial rebuild so this is cheap to run eagerly
  // (e.g. on every load) on a healthy mesh.
  if (errors == 0) {
    return 0;
  }

  // Pass 4: kill the unrepairable elements (raw — no meshlog callbacks).
  for (int fi : killFaces) {
    if (!f.freemap[fi]) {
      kill_face(fi);
    }
  }
  for (int ei : killEdges) {
    if (!e.freemap[ei]) {
      kill_edge(ei);
    }
  }

  // Pass 5: rebuild every disk cycle from the (authoritative) edge endpoints.
  for (int vi : this->v) {
    v.e[vi] = ELEM_NONE;
  }
  for (int ei : this->e) {
    e.disk[ei][0] = e.disk[ei][1] = e.disk[ei][2] = e.disk[ei][3] = ei;
  }
  for (int ei : this->e) {
    disk_insert(ei, e.vs[ei][0]);
    disk_insert(ei, e.vs[ei][1]);
  }

  // Pass 6: rebuild radial cycles + corner edges from the surviving face loops.
  for (int ei : this->e) {
    e.c[ei] = ELEM_NONE;
  }
  for (int ci : this->c) {
    c.radial_next[ci] = c.radial_prev[ci] = ci;
  }
  for (int fi : this->f) {
    int li = f.l[fi], c0 = l.c[li], cc = c0, n = 0;
    do {
      int cn = c.next[cc];
      int ce = find_edge(c.v[cc], c.v[cn]);
      if (ce == ELEM_NONE) {
        ce = make_edge(c.v[cc], c.v[cn]);
        snprintf(buf, sizeof(buf), "face %d corner %d had no edge; created %d", fi, cc, ce);
        report(buf);
      }
      c.e[cc] = ce;
      radial_insert(ce, cc);
      cc = cn;
      if (++n > 1000000) {
        break;
      }
    } while (cc != c0);
  }

  if (errors > 0) {
    snprintf(buf, sizeof(buf),
             "validateAndRepair: %d problem(s); disk/radial cycles rebuilt", errors);
    report(buf);
  }
  return errors;
}

void Mesh::featureVerts(int kind, util::Vector<int> &outIdx, util::Vector<float> &outCo)
{
  outIdx.clear();
  outCo.clear();
  if (topo_frozen) {
    thawTopo();
  }
  BoolAttrView *view = boundary::findBoolEdgeView(this, edgeFlagNameForKind(kind));
  if (!view) {
    return;
  }
  // De-dup endpoints across all flagged edges via a per-vert seen bitmap.
  util::Vector<uint8_t> seen;
  seen.resize(size_t(v.count));
  for (int e : this->e) {
    if (!view->get(e)) {
      continue;
    }
    for (int side = 0; side < 2; side++) {
      int vi = this->e.vs[e][side];
      if (vi < 0 || vi >= v.count || seen[vi]) {
        continue;
      }
      seen[vi] = 1;
      math::float3 co = v.co[vi];
      outIdx.append(vi);
      outCo.append(co[0]);
      outCo.append(co[1]);
      outCo.append(co[2]);
    }
  }
}

void Mesh::edgePathEdges(int vStart, int vEnd, util::Vector<int> &out)
{
  out.clear();
  if (topo_frozen) {
    thawTopo();
  }
  util::Vector<int> path;
  if (!shortestEdgePath(this, vStart, vEnd, path)) {
    return;
  }
  for (int i = 0; i + 1 < int(path.size()); i++) {
    int e = find_edge(path[i], path[i + 1]);
    if (e != ELEM_NONE) {
      out.append(e);
    }
  }
}

int Mesh::edgeSeam(int e)
{
  return boundary::edgeFlag(this, boundary::EDGE_SEAM, e) ? 1 : 0;
}

void Mesh::setEdgeSeam(int e, int state)
{
  boundary::setEdgeFlag(this, boundary::EDGE_SEAM, e, state != 0);
}

void Mesh::recomputeBoundary()
{
  if (topo_frozen) {
    thawTopo();
  }
  boundary::recomputeDirty(this);
}

void Mesh::boundaryGraphStats(util::Vector<int> &out)
{
  if (topo_frozen) {
    thawTopo();
  }
  boundary::recomputeDirty(this);
  boundary::graphStats(this, out);
}

void Mesh::foldActiveSculptLayer(std::span<const int> verts)
{
  if (activeEditLayer < 0 || activeEditLayer >= int(sculptLayers.size())) {
    return;
  }
  AttrRef dref =
      v.attrs.find_attribute(AttrType::FLOAT3, sculptLayers[activeEditLayer].name);
  AttrRef rref = v.attrs.find_attribute(AttrType::FLOAT3, string(SCULPT_LAYER_REST_ATTR));
  if (!dref.exists() || !rref.exists()) {
    return;
  }
  AttrData<math::float3> *d = dref.get_data<math::float3>();
  AttrData<math::float3> *rest = rref.get_data<math::float3>();
  auto fold1 = [&](int vi) {
    d->materialize(vi);
    (*d)[vi] = v.co[vi] - rest->safe_get(vi);
  };
  if (verts.empty()) {
    for (int vi : v) {
      fold1(vi);
    }
  } else {
    for (int vi : verts) {
      fold1(vi);
    }
  }
}

void Mesh::sculptLayerFlattenAll()
{
  activeEditLayer = -1;
  auto dropColumn = [&](const string &name) {
    for (int i = 0; i < int(v.attrs.attrs.size()); i++) {
      if (v.attrs.attrs[i].type == AttrType::FLOAT3 && v.attrs.attrs[i].name == name) {
        v.attrs.remove_attr(i);
        return;
      }
    }
  };
  for (SculptLayerSettings &st : sculptLayers) {
    dropColumn(st.name);
  }
  dropColumn(string(SCULPT_LAYER_REST_ATTR));
  sculptLayers.clear();
}

void Mesh::sculptLayerPruneSettingsOnly()
{
  for (int i = int(sculptLayers.size()) - 1; i >= 0; i--) {
    if (!v.attrs.find_attribute(AttrType::FLOAT3, sculptLayers[i].name).exists()) {
      if (activeEditLayer == i) {
        activeEditLayer = -1;
      } else if (activeEditLayer > i) {
        activeEditLayer--;
      }
      sculptLayers.remove_at(i, /*swap_end_only=*/false);
    }
  }
}

void Mesh::edgePathCoords(int vStart, int vEnd, util::Vector<float> &out)
{
  out.clear();
  if (topo_frozen) {
    thawTopo();
  }
  util::Vector<int> path;
  if (!shortestEdgePath(this, vStart, vEnd, path)) {
    return;
  }
  for (int vi : path) {
    math::float3 co = v.co[vi];
    out.append(co[0]);
    out.append(co[1]);
    out.append(co[2]);
  }
}

void Mesh::dumpVertCo(util::Vector<float> &out)
{
  // Flat (idx,x,y,z) per live vert. No thaw (reading co needs no topo);
  // index-aligned so JS can address each vert by its index.
  out.clear();
  for (int vi : this->v) {
    math::float3 co = v.co[vi];
    out.append(float(vi));
    out.append(co[0]);
    out.append(co[1]);
    out.append(co[2]);
  }
}

static void dumpVertAttr3(Mesh &m, const char *name, util::Vector<float> &out)
{
  out.clear();
  AttrRef ref = m.v.attrs.find_attribute(AttrType::FLOAT3, name);
  if (!ref.exists()) {
    return;
  }
  auto *data = static_cast<AttrData<math::float3> *>(ref.data);
  for (int vi : m.v) {
    math::float3 val = data->safe_get(vi);
    out.append(val[0]);
    out.append(val[1]);
    out.append(val[2]);
  }
}

void Mesh::dumpFrameNormals(util::Vector<float> &out)
{
  dumpVertAttr3(*this, ".frames.v.normal", out);
}

void Mesh::dumpFrameTangents(util::Vector<float> &out)
{
  dumpVertAttr3(*this, ".frames.v.tangent", out);
}

void Mesh::setVertCo(int idx, float x, float y, float z)
{
  if (idx < 0 || idx >= int(v.capacity()) || v.freemap[idx]) {
    return;
  }
  v.co[idx] = math::float3(x, y, z);
}

void Mesh::symmetrize(int axis, int sign, float threshold)
{
  symmetrizeMesh(*this, axis, sign, threshold);
}

int Mesh::generateUVFromSeams(int marginMilli)
{
  // The unwrapper flood-fills + walks live loop/disk links, so thaw first.
  if (topo_frozen) {
    thawTopo();
  }
  // Names don't marshal, so own naming here: a unique "uv[.NNN]" corner layer
  // (shares addAttr's helper). generateUVFromSeams ensures the layer + tags it
  // AttrUse::UV.
  util::string name = uniqueAttrName(&c.attrs, "uv");
  float margin = float(marginMilli) / 1000.0f;
  return mesh::generateUVFromSeams(this, name.c_str(), margin);
}

void Mesh::markAllSeams()
{
  if (topo_frozen) {
    thawTopo();
  }
  // Seam every edge so each face becomes its own UV chart — generateUVFromSeams
  // then planar-projects per face, i.e. a cuboid map. A test/demo helper.
  for (int e0 : IndexRange(0, e.count)) {
    boundary::setEdgeFlag(this, boundary::EDGE_SEAM, e0, true);
  }
  boundary::recomputeDirty(this);
}

void Mesh::fillVertexColorFromPosition()
{
  // Fill the first vertex-domain FLOAT4 layer tagged COLOR with a deterministic
  // position->rgb gradient (normalized into the mesh bbox, alpha 1). Gives the
  // vertex-color attribute meaningful, backend-identical values without a brush
  // stroke. No-op if no such layer exists.
  AttrData<math::float4> *cdata = nullptr;
  for (AttrRef &a : v.attrs.attrs) {
    if (a.type == AttrType::FLOAT4 && (a.use & AttrUse::COLOR)) {
      cdata = a.get_data<math::float4>();
      break;
    }
  }
  if (!cdata || v.count == 0) {
    return;
  }

  math::float3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
  for (int i : IndexRange(0, v.count)) {
    math::float3 &co = v.co[i];
    for (int k = 0; k < 3; k++) {
      if (co[k] < mn[k])
        mn[k] = co[k];
      if (co[k] > mx[k])
        mx[k] = co[k];
    }
  }
  math::float3 size = mx - mn;
  for (int k = 0; k < 3; k++) {
    if (size[k] < 1e-6f)
      size[k] = 1.0f;
  }
  for (int i : IndexRange(0, v.count)) {
    math::float3 &co = v.co[i];
    float r = (co[0] - mn[0]) / size[0];
    float g = (co[1] - mn[1]) / size[1];
    float b = (co[2] - mn[2]) / size[2];
    (*cdata)[i] = math::float4(r, g, b, 1.0f);
  }
}

void Mesh::vertexColor(int vert, util::Vector<float> &out)
{
  out.clear();
  AttrData<math::float4> *cdata = nullptr;
  for (AttrRef &a : v.attrs.attrs) {
    if (a.type == AttrType::FLOAT4 && (a.use & AttrUse::COLOR)) {
      cdata = a.get_data<math::float4>();
      break;
    }
  }
  if (!cdata || vert < 0 || vert >= v.count) {
    out.append(1.0f);
    out.append(1.0f);
    out.append(1.0f);
    out.append(1.0f);
    return;
  }
  math::float4 c = (*cdata)[vert];
  out.append(c[0]);
  out.append(c[1]);
  out.append(c[2]);
  out.append(c[3]);
}

int Mesh::make_vertex(math::float3 co, MeshCallbacks *cb, int hint)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;
  int r = v.alloc_near(locality_alloc_enabled() ? hint : ELEM_NONE);

  v.co[r] = co;
  v.e[r] = ELEM_NONE;

  if (cb) {
    fire(cb->onVertCreate, r);
  }

  return r;
}

int Mesh::make_edge(int v1, int v2, MeshCallbacks *cb, int hint)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;
  int r = e.alloc_near(locality_alloc_enabled() ? hint : ELEM_NONE);

  e.c[r] = ELEM_NONE;

  e.vs[r][0] = v1;
  e.vs[r][1] = v2;

  e.disk[r] = math::int4(ELEM_NONE);

  if (cb) {
    /* disk_insert rewires the existing disk neighbors at each endpoint; snapshot
     * their pre-insert links for the meshlog BEFORE they change, or undo leaves
     * those neighbors pointing at the (released) new edge. */
    int ends[2] = {v1, v2};
    for (int vv : ends) {
      if (v.e[vv] == ELEM_NONE) {
        continue;
      }
      int e2 = v.e[vv];
      int prevn = e.disk[e2][edge_side(e2, vv) * 2];
      fire(cb->onEdgeChange, e2);
      if (prevn != e2) {
        fire(cb->onEdgeChange, prevn);
      }
    }
  }

  disk_insert(r, v1);
  disk_insert(r, v2);

  if (cb) {
    fire(cb->onEdgeCreate, r);
    fire(cb->onVertChange, v1);
    fire(cb->onVertChange, v2);
  }

  return r;
}

int Mesh::make_face(std::span<int> verts, std::span<int> edges, MeshCallbacks *cb, int hint)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;
  int fi = f.alloc_near(locality_alloc_enabled() ? hint : ELEM_NONE);
  int li = l.alloc();

  int vlen = verts.size();
  if (vlen > 3) {
    n_ngon_faces++;
  }

  f.l[fi] = li;
  f.list_count[fi] = 1;

  l.size[li] = vlen;
  l.f[li] = fi;
  l.next[li] = ELEM_NONE;

  util::Vector<int, 8> corners;
  int prev_c = ELEM_NONE;
  for (int i = 0; i < vlen; i++) {
    /* Place the corner near the adjacent face's corner on this edge (read
     * before radial_insert overwrites e.c), else chain off this face's prior
     * corner so the face's corners stay contiguous. */
    int radial_c = e.c[edges[i]];
    int hint_c = radial_c != ELEM_NONE ? radial_c : prev_c;
    int ci = c.alloc_near(locality_alloc_enabled() ? hint_c : ELEM_NONE);
    prev_c = ci;

    c.v[ci] = verts[i];
    c.e[ci] = edges[i];
    c.l[ci] = li;

    if (cb && radial_c != ELEM_NONE) {
      /* radial_insert rewires the edge's existing radial neighbors; snapshot
       * their pre-insert links for the meshlog before they change. */
      int c2 = radial_c;
      int c2prev = c.radial_prev[c2];
      fire(cb->onCornerChange, c2);
      if (c2prev != c2) {
        fire(cb->onCornerChange, c2prev);
      }
    }

    radial_insert(edges[i], ci);
    corners.append(ci);
  }

  l.c[li] = corners[0];

  for (int i = 0; i < vlen; i++) {
    int l1 = corners[(i - 1 + vlen) % vlen];
    int l2 = corners[i];
    int l3 = corners[(i + 1) % vlen];

    c.prev[l2] = l1;
    c.next[l2] = l3;
  }

  if (cb) {
    fire(cb->onFaceCreate, fi);
    fire(cb->onListCreate, li);
    for (int i = 0; i < vlen; i++) {
      fire(cb->onCornerCreate, corners[i]);
      fire(cb->onEdgeChange, edges[i]);
    }
  }

  return fi;
}

int Mesh::make_face(std::span<int> verts, MeshCallbacks *cb, int hint)
{
  /* find_edge below walks live disks, so thaw before touching connectivity. */
  if (topo_frozen)
    thawTopo();

  util::Vector<int, 6> edges;

  int vlen = verts.size();
  for (int i = 0; i < vlen; i++) {
    int v1 = verts[i], v2 = verts[(i + 1) % vlen];
    int e1 = find_edge(v1, v2);

    if (e1 == ELEM_NONE) {
      e1 = make_edge(v1, v2, cb);
    }

    edges.append(e1);
  }

  return make_face(verts, edges, cb, hint);
}

void Mesh::kill_vertex(int v1, MeshCallbacks *cb)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;
  while (v.e[v1] != ELEM_NONE) {
    kill_edge(v.e[v1], cb);
  }

  if (cb) {
    fire(cb->onVertKill, v1);
  }

  v.release(v1);
}

void Mesh::kill_edge(int e1, MeshCallbacks *cb)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;
  while (e.c[e1] != ELEM_NONE) {
    kill_face(l.f[c.l[e.c[e1]]], cb);
  }

  int va = e.vs[e1][0];
  int vb = e.vs[e1][1];

  if (cb) {
    /* disk_remove rewires e1's disk neighbors at each endpoint; snapshot them
     * before they change (same reason as make_edge). */
    int ends[2] = {va, vb};
    for (int vv : ends) {
      int side1 = edge_side(e1, vv);
      int prevn = e.disk[e1][side1 * 2];
      int nextn = e.disk[e1][side1 * 2 + 1];
      if (prevn != e1) {
        fire(cb->onEdgeChange, prevn);
      }
      if (nextn != e1 && nextn != prevn) {
        fire(cb->onEdgeChange, nextn);
      }
    }
  }

  disk_remove(e1, va);
  disk_remove(e1, vb);

  if (cb) {
    fire(cb->onEdgeKill, e1);
    fire(cb->onVertChange, va);
    fire(cb->onVertChange, vb);
  }

  e.release(e1);
}

void Mesh::kill_face(int f1, MeshCallbacks *cb)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;
  int l1 = f.l[f1];
  if (l1 != ELEM_NONE && l.size[l1] > 3) {
    n_ngon_faces--;
  }
  while (l1 != ELEM_NONE) {
    int next = l.next[l1];

    int c1 = l.c[l1], startc1 = c1;
    int cnext;
    do {
      cnext = c.next[c1];

      int eid = c.e[c1];
      if (cb) {
        /* radial_remove rewires c1's radial neighbors and may change the edge's
         * first-corner pointer; snapshot them before the change. */
        int rnext = c.radial_next[c1];
        int rprev = c.radial_prev[c1];
        if (rnext != c1) {
          fire(cb->onCornerChange, rnext);
        }
        if (rprev != c1 && rprev != rnext) {
          fire(cb->onCornerChange, rprev);
        }
        fire(cb->onEdgeChange, eid);
      }

      radial_remove(eid, c1);

      if (cb) {
        fire(cb->onCornerKill, c1);
      }

      c.release(c1);
    } while ((c1 = cnext) != startc1);

    if (cb) {
      fire(cb->onListKill, l1);
    }

    l.release(l1);
    l1 = next;
  }

  if (cb) {
    fire(cb->onFaceKill, f1);
  }

  f.release(f1);
}

void Mesh::relink_edge_verts(int e1, int nv0, int nv1, MeshCallbacks *cb)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;

  int ov0 = e.vs[e1][0];
  int ov1 = e.vs[e1][1];

  if (cb) {
    /* Capture e1's pre-splice row (vs + disk are TOPO) for the meshlog, and
     * its old disk neighbours before disk_remove rewires them. */
    fire(cb->onEdgeChange, e1);
    int olds[2] = {ov0, ov1};
    for (int vv : olds) {
      int side = edge_side(e1, vv);
      int prevn = e.disk[e1][side * 2];
      int nextn = e.disk[e1][side * 2 + 1];
      if (prevn != e1) {
        fire(cb->onEdgeChange, prevn);
      }
      if (nextn != e1 && nextn != prevn) {
        fire(cb->onEdgeChange, nextn);
      }
    }
  }

  disk_remove(e1, ov0);
  disk_remove(e1, ov1);

  e.vs[e1][0] = nv0;
  e.vs[e1][1] = nv1;
  e.disk[e1] = math::int4(ELEM_NONE);

  if (cb) {
    /* New endpoints' disk neighbours, before disk_insert rewires them
     * (mirrors make_edge). */
    int news[2] = {nv0, nv1};
    for (int vv : news) {
      if (v.e[vv] == ELEM_NONE) {
        continue;
      }
      int e2 = v.e[vv];
      int prevn = e.disk[e2][edge_side(e2, vv) * 2];
      fire(cb->onEdgeChange, e2);
      if (prevn != e2) {
        fire(cb->onEdgeChange, prevn);
      }
    }
  }

  disk_insert(e1, nv0);
  disk_insert(e1, nv1);

  if (cb) {
    fire(cb->onVertChange, ov0);
    fire(cb->onVertChange, ov1);
    fire(cb->onVertChange, nv0);
    fire(cb->onVertChange, nv1);
  }
}

void Mesh::clear_face_contents(int f1, MeshCallbacks *cb)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;

  /* The face id survives; fire onFaceChange BEFORE any mutation so the meshlog
   * snapshots the pre-rewrite face row and the spatial tree re-flags the leaf
   * that currently owns it (reads the OLD verts, which are still wired here). */
  if (cb) {
    fire(cb->onFaceChange, f1);
  }

  int l1 = f.l[f1];
  if (l1 != ELEM_NONE && l.size[l1] > 3) {
    n_ngon_faces--;
  }
  while (l1 != ELEM_NONE) {
    int next = l.next[l1];

    int c1 = l.c[l1], startc1 = c1;
    int cnext;
    do {
      cnext = c.next[c1];

      int eid = c.e[c1];
      if (cb) {
        int rnext = c.radial_next[c1];
        int rprev = c.radial_prev[c1];
        if (rnext != c1) {
          fire(cb->onCornerChange, rnext);
        }
        if (rprev != c1 && rprev != rnext) {
          fire(cb->onCornerChange, rprev);
        }
        fire(cb->onEdgeChange, eid);
      }

      radial_remove(eid, c1);

      if (cb) {
        fire(cb->onCornerKill, c1);
      }

      c.release(c1);
    } while ((c1 = cnext) != startc1);

    if (cb) {
      fire(cb->onListKill, l1);
    }

    l.release(l1);
    l1 = next;
  }

  f.l[f1] = ELEM_NONE; /* dangling until reinit_face repopulates it */
}

void Mesh::reinit_face(int f1, std::span<int> verts, std::span<int> edges,
                       MeshCallbacks *cb)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;
  int li = l.alloc();

  int vlen = verts.size();
  if (vlen > 3) {
    n_ngon_faces++;
  }

  f.l[f1] = li;
  f.list_count[f1] = 1;

  l.size[li] = vlen;
  l.f[li] = f1;
  l.next[li] = ELEM_NONE;

  util::Vector<int, 8> corners;
  int prev_c = ELEM_NONE;
  for (int i = 0; i < vlen; i++) {
    /* Place the corner near the adjacent face's corner on this edge (read
     * before radial_insert overwrites e.c), else chain off this face's prior
     * corner so the face's corners stay contiguous. */
    int radial_c = e.c[edges[i]];
    int hint_c = radial_c != ELEM_NONE ? radial_c : prev_c;
    int ci = c.alloc_near(locality_alloc_enabled() ? hint_c : ELEM_NONE);
    prev_c = ci;

    c.v[ci] = verts[i];
    c.e[ci] = edges[i];
    c.l[ci] = li;

    if (cb && radial_c != ELEM_NONE) {
      /* radial_insert rewires the edge's existing radial neighbors; snapshot
       * their pre-insert links for the meshlog before they change. */
      int c2 = radial_c;
      int c2prev = c.radial_prev[c2];
      fire(cb->onCornerChange, c2);
      if (c2prev != c2) {
        fire(cb->onCornerChange, c2prev);
      }
    }

    radial_insert(edges[i], ci);
    corners.append(ci);
  }

  l.c[li] = corners[0];

  for (int i = 0; i < vlen; i++) {
    int l1 = corners[(i - 1 + vlen) % vlen];
    int l2 = corners[i];
    int l3 = corners[(i + 1) % vlen];

    c.prev[l2] = l1;
    c.next[l2] = l3;
  }

  if (cb) {
    fire(cb->onListCreate, li);
    for (int i = 0; i < vlen; i++) {
      fire(cb->onCornerCreate, corners[i]);
      fire(cb->onEdgeChange, edges[i]);
    }
    /* The face id survived the rewrite; fire onFaceChange AFTER so the spatial
     * tree's touch_face reads the new vertex set (the meshlog already
     * snapshotted in clear_face_contents, so this is a no-op there). */
    fire(cb->onFaceChange, f1);
  }
}

void Mesh::recountNgons()
{
  if (topo_frozen) {
    thawTopo();
  }
  int64_t n = 0;
  for (int fi : f) {
    int li = f.l[fi];
    if (li != ELEM_NONE && l.size[li] > 3) {
      n++;
    }
  }
  n_ngon_faces = n;
}

/* Box-modeling loop/boundary query wrappers (see utils/modeling_walk.h). Thin so
 * the macro-ops can call either these or the free functions directly. */
void Mesh::selectionBoundaryEdges(util::Vector<int> &out)
{
  regionBoundaryEdges(*this, out);
}
void Mesh::movableVerts(util::Vector<int> &out)
{
  gatherMovableVerts(*this, out);
}
void Mesh::edgeRing(int e, util::Vector<int> &out)
{
  walkEdgeRing(*this, e, out);
}
void Mesh::faceLoop(int e, util::Vector<int> &out)
{
  walkFaceLoop(*this, e, out);
}
void Mesh::edgeLoop(int e, util::Vector<int> &out)
{
  walkEdgeLoop(*this, e, out);
}
void Mesh::loopCutPreviewCoords(int seedEdge, util::Vector<float> &out)
{
  if (topo_frozen) {
    thawTopo();
  }
  if (seedEdge < 0 || seedEdge >= int(e.capacity()) || e.freemap[seedEdge]) {
    return;
  }
  util::Vector<int> ring, faces;
  walkEdgeRing(*this, seedEdge, ring);
  walkFaceLoop(*this, seedEdge, faces);
  util::Set<int> ringSet;
  for (int ei : ring) {
    ringSet.add(ei);
  }
  auto mid = [&](int ei) {
    return (v.co[e.vs[ei][0]] + v.co[e.vs[ei][1]]) * 0.5f;
  };
  // One preview segment per quad of the face loop: the midpoints of its two
  // ring edges (where the cut verts will land).
  for (int fi : faces) {
    int e1 = ELEM_NONE, e2 = ELEM_NONE;
    int li = f.l[fi], c0 = l.c[li], cc = c0;
    do {
      int ei = c.e[cc];
      if (ringSet.contains(ei)) {
        if (e1 == ELEM_NONE) {
          e1 = ei;
        } else if (ei != e1) {
          e2 = ei;
        }
      }
      cc = c.next[cc];
    } while (cc != c0);
    if (e1 == ELEM_NONE || e2 == ELEM_NONE) {
      continue;
    }
    float3 a = mid(e1), b = mid(e2);
    for (int k = 0; k < 3; k++) {
      out.append(a[k]);
    }
    for (int k = 0; k < 3; k++) {
      out.append(b[k]);
    }
  }
}
int Mesh::faceEdgeNearest(int f, const math::float3 &p)
{
  return faceEdgeNearestPoint(*this, f, p);
}
void Mesh::faceVertList(int fi, util::Vector<int> &outVerts, util::Vector<float> &outCoords)
{
  if (topo_frozen) {
    thawTopo();
  }
  if (fi < 0 || fi >= int(f.capacity()) || f.freemap[fi]) {
    return;
  }
  for (int li = f.l[fi]; li != ELEM_NONE; li = l.next[li]) {
    int c0 = l.c[li], cc = c0;
    do {
      int vi = c.v[cc];
      outVerts.append(vi);
      const float3 &a = v.co[vi];
      for (int k = 0; k < 3; k++) {
        outCoords.append(a[k]);
      }
      cc = c.next[cc];
    } while (cc != c0);
  }
}

void Mesh::faceEdgeList(int fi, util::Vector<int> &outEdges, util::Vector<float> &outCoords)
{
  if (topo_frozen) {
    thawTopo();
  }
  if (fi < 0 || fi >= int(f.capacity()) || f.freemap[fi]) {
    return;
  }
  for (int li = f.l[fi]; li != ELEM_NONE; li = l.next[li]) {
    int c0 = l.c[li], cc = c0;
    do {
      int ei = c.e[cc];
      outEdges.append(ei);
      const float3 &a = v.co[e.vs[ei][0]];
      const float3 &b = v.co[e.vs[ei][1]];
      for (int k = 0; k < 3; k++) {
        outCoords.append(a[k]);
      }
      for (int k = 0; k < 3; k++) {
        outCoords.append(b[k]);
      }
      cc = c.next[cc];
    } while (cc != c0);
  }
}

namespace {
inline int remap(util::span<int> map, int idx)
{
  return idx == ELEM_NONE ? ELEM_NONE : map[idx];
}
} // namespace

void Mesh::reorder_verts(util::span<int> vmap, const ReorderMoved &moved)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;

  if (moved.active) {
    /* Refs into a moved vert come only from a moved edge (e.vs) or moved corner
     * (c.v): a moved vert is interior, so its incident edges/corners are moved. */
    for (int e1 : moved.e) {
      e.vs[e1][0] = remap(vmap, e.vs[e1][0]);
      e.vs[e1][1] = remap(vmap, e.vs[e1][1]);
    }
    for (int c1 : moved.c) {
      c.v[c1] = remap(vmap, c.v[c1]);
    }
    v.reorderScoped(vmap, moved.v);
    return;
  }

  for (int e1 : e) {
    e.vs[e1][0] = remap(vmap, e.vs[e1][0]);
    e.vs[e1][1] = remap(vmap, e.vs[e1][1]);
  }
  for (int c1 : c) {
    c.v[c1] = remap(vmap, c.v[c1]);
  }
  v.reorder(vmap);
}

void Mesh::reorder_edges(util::span<int> emap, const ReorderMoved &moved)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;

  if (moved.active) {
    util::Set<int> movedEdge;
    for (int e1 : moved.e)
      movedEdge.add(e1);

    for (int c1 : moved.c) {
      c.e[c1] = remap(emap, c.e[c1]);  // corner → edge
    }
    for (int e1 : moved.e) {
      /* v.e of a moved edge's endpoints: only an endpoint can hold this edge. */
      for (int s = 0; s < 2; s++) {
        int w = e.vs[e1][s];  // already-remapped (new) vert index
        if (w != ELEM_NONE && v.e[w] == e1) {
          v.e[w] = emap[e1];
        }
      }
      /* e.disk: remap this edge's own links; patch the back-link of each
       * non-moved disk neighbor (moved neighbors fix themselves). Read all
       * neighbors before remapping our own slots. Slot layout: [side*2]=prev,
       * [side*2+1]=next around vert e.vs[e1][side]. */
      int nb[4];
      for (int k = 0; k < 4; k++)
        nb[k] = e.disk[e1][k];
      for (int s = 0; s < 2; s++) {
        int w = e.vs[e1][s];
        int P = nb[s * 2], N = nb[s * 2 + 1];
        if (P != ELEM_NONE && P != e1 && !movedEdge.contains(P)) {
          int sp = (e.vs[P][0] == w) ? 0 : 1;
          e.disk[P][sp * 2 + 1] = emap[e1];  // P.next around w == e1
        }
        if (N != ELEM_NONE && N != e1 && !movedEdge.contains(N)) {
          int sn = (e.vs[N][0] == w) ? 0 : 1;
          e.disk[N][sn * 2] = emap[e1];  // N.prev around w == e1
        }
      }
      for (int k = 0; k < 4; k++)
        e.disk[e1][k] = remap(emap, nb[k]);
    }
    e.reorderScoped(emap, moved.e);
    return;
  }

  for (int v1 : v) {
    v.e[v1] = remap(emap, v.e[v1]);
  }
  for (int e1 : e) {
    for (int k = 0; k < 4; k++) {
      e.disk[e1][k] = remap(emap, e.disk[e1][k]);
    }
  }
  for (int c1 : c) {
    c.e[c1] = remap(emap, c.e[c1]);
  }
  e.reorder(emap);
}

void Mesh::reorder_corners(util::span<int> cmap, const ReorderMoved &moved)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;

  if (moved.active) {
    util::Set<int> movedCorner;
    for (int c1 : moved.c)
      movedCorner.add(c1);

    for (int c1 : moved.c) {
      int ed = c.e[c1];  // new edge index (edges already permuted)
      if (ed != ELEM_NONE && e.c[ed] == c1) {
        e.c[ed] = cmap[c1];  // edge → corner
      }
      /* Loop-cycle neighbors (c.next/prev) are corners of the same face, always
       * moved → remap own links only. */
      c.next[c1] = remap(cmap, c.next[c1]);
      c.prev[c1] = remap(cmap, c.prev[c1]);
      /* Radial neighbors may be non-moved (across a boundary edge): patch their
       * back-links, read before remapping own. */
      int rn = c.radial_next[c1], rp = c.radial_prev[c1];
      if (rn != c1 && !movedCorner.contains(rn))
        c.radial_prev[rn] = cmap[c1];
      if (rp != c1 && !movedCorner.contains(rp))
        c.radial_next[rp] = cmap[c1];
      c.radial_next[c1] = remap(cmap, rn);
      c.radial_prev[c1] = remap(cmap, rp);
    }
    for (int l1 : moved.l) {
      l.c[l1] = remap(cmap, l.c[l1]);  // list → corner
    }
    c.reorderScoped(cmap, moved.c);
    return;
  }

  for (int e1 : e) {
    e.c[e1] = remap(cmap, e.c[e1]);
  }
  for (int c1 : c) {
    c.next[c1] = remap(cmap, c.next[c1]);
    c.prev[c1] = remap(cmap, c.prev[c1]);
    c.radial_next[c1] = remap(cmap, c.radial_next[c1]);
    c.radial_prev[c1] = remap(cmap, c.radial_prev[c1]);
  }
  for (int l1 : l) {
    l.c[l1] = remap(cmap, l.c[l1]);
  }
  c.reorder(cmap);
}

void Mesh::reorder_lists(util::span<int> lmap, const ReorderMoved &moved,
                         util::span<int> cmap)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;

  if (moved.active) {
    /* corners already permuted: the moved corner formerly at c1 now lives at
     * cmap[c1], so its c.l (corner → list) ref is read/written there. */
    for (int c1 : moved.c) {
      int nc = remap(cmap, c1);
      c.l[nc] = remap(lmap, c.l[nc]);
    }
    for (int l1 : moved.l) {
      l.next[l1] = remap(lmap, l.next[l1]);  // list-cycle neighbors always moved
    }
    for (int f1 : moved.f) {
      f.l[f1] = remap(lmap, f.l[f1]);  // face → list
    }
    l.reorderScoped(lmap, moved.l);
    return;
  }

  for (int c1 : c) {
    c.l[c1] = remap(lmap, c.l[c1]);
  }
  for (int l1 : l) {
    l.next[l1] = remap(lmap, l.next[l1]);
  }
  for (int f1 : f) {
    f.l[f1] = remap(lmap, f.l[f1]);
  }
  l.reorder(lmap);
}

void Mesh::reorder_faces(util::span<int> fmap, const ReorderMoved &moved,
                         util::span<int> lmap)
{
  if (topo_frozen)
    thawTopo();
  topo_stamp++;

  if (moved.active) {
    /* lists already permuted: the moved list formerly at l1 now lives at
     * lmap[l1], so its l.f (list → face) ref is read/written there. */
    for (int l1 : moved.l) {
      int nl = remap(lmap, l1);
      l.f[nl] = remap(fmap, l.f[nl]);
    }
    f.reorderScoped(fmap, moved.f);
    return;
  }

  for (int l1 : l) {
    l.f[l1] = remap(fmap, l.f[l1]);
  }
  f.reorder(fmap);
}

int Mesh::freeTrailingStorage()
{
  int freed = 0;
  freed += v.free_trailing_pages();
  freed += e.free_trailing_pages();
  freed += c.free_trailing_pages();
  freed += l.free_trailing_pages();
  freed += f.free_trailing_pages();
  return freed;
}

} // namespace sculptcore::mesh
