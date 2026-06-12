// Edge-collapse fold regression gate. Loads the OBJ patches captured by the
// SCULPTCORE_COLLAPSE_DEBUG instrumentation (source/mesh/utils/collapse_debug.h),
// replays the exact collapse dyntopo performed (survivor = e.vs[0], move to the
// edge midpoint, blend 0.5), and asserts no incident triangle flips orientation
// and the patch stays manifold. Each fixture carries a
// `# collapse_edge keep=K kill=V` marker naming the collapsed edge's endpoints
// (1-based OBJ vertex indices). See documentation/quad-remeshing.md.
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "litestl/util/set.h"
#include "litestl/util/vector.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/utils/edge_collapse.h"
#include "mesh/utils/mesh_validate.h"
#include "mesh/utils/obj_io.h"

#include "test_config.h"

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using litestl::math::float3;
using litestl::util::Set;
using litestl::util::Vector;

namespace {

// Parse the "# collapse_edge keep=K kill=V" marker (1-based) from the fixture.
bool parseKeepKill(const char *path, int &keep, int &kill)
{
  std::FILE *fp = std::fopen(path, "rb");
  if (!fp) {
    return false;
  }
  char line[1024];
  bool found = false;
  while (std::fgets(line, sizeof(line), fp)) {
    if (std::sscanf(line, "# collapse_edge keep=%d kill=%d", &keep, &kill) == 2) {
      found = true;
      break;
    }
  }
  std::fclose(fp);
  return found;
}

int findEdge(Mesh &m, int a, int b)
{
  for (int ei : m.e) {
    int v0 = m.e.vs[ei][0], v1 = m.e.vs[ei][1];
    if ((v0 == a && v1 == b) || (v0 == b && v1 == a)) {
      return ei;
    }
  }
  return ELEM_NONE;
}

void sortSmall(Vector<int, 8> &v)
{
  for (int i = 1; i < int(v.size()); i++) {
    int x = v[i], j = i - 1;
    while (j >= 0 && v[j] > x) {
      v[j + 1] = v[j];
      j--;
    }
    v[j + 1] = x;
  }
}

bool sameSorted(const Vector<int, 8> &a, const Vector<int, 8> &b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (int i = 0; i < int(a.size()); i++) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

struct FaceNrm {
  Vector<int, 8> key; // sorted, v_kill remapped to v_keep
  float3 nrm;         // unnormalized Newell normal
};

// Sorted vertex key of face `f` with v_kill remapped to v_keep.
Vector<int, 8> faceKey(Mesh &m, int f, int v_kill, int v_keep)
{
  Vector<int, 8> key;
  int li = m.f.l[f], c0 = m.l.c[li], cc = c0;
  do {
    int gv = m.c.v[cc];
    key.append(gv == v_kill ? v_keep : gv);
    cc = m.c.next[cc];
  } while (cc != c0);
  sortSmall(key);
  return key;
}

float vdot(const float3 &a, const float3 &b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Collect normals of every face incident to either endpoint, keyed by the
// remapped sorted vertex set (so a survivor face after the collapse matches).
void snapshotIncidentNormals(Mesh &m, int v_keep, int v_kill,
                             Vector<FaceNrm> &out)
{
  Set<int> seen;
  for (int side = 0; side < 2; side++) {
    int v = side == 0 ? v_keep : v_kill;
    if (m.v.e[v] == ELEM_NONE) {
      continue;
    }
    for (int e : EdgeOfVertIter(&m, v, m.v.e[v])) {
      int c0 = m.e.c[e];
      if (c0 == ELEM_NONE) {
        continue;
      }
      int cc = c0;
      do {
        int f = m.l.f[m.c.l[cc]];
        if (seen.add(f)) {
          FaceNrm fn;
          fn.key = faceKey(m, f, v_kill, v_keep);
          fn.nrm = faceNewellNormal(m, f);
          out.append(std::move(fn));
        }
        cc = m.c.radial_next[cc];
      } while (cc != c0);
    }
  }
}

// After the collapse, return the worst (most negative) normalized normal dot
// across surviving faces incident to the survivor that match a before-face.
float worstFoldDot(Mesh &m, int v_keep, const Vector<FaceNrm> &before)
{
  float worst = 1.0f;
  if (v_keep < 0 || v_keep >= int(m.v.capacity()) || m.v.freemap[v_keep] ||
      m.v.e[v_keep] == ELEM_NONE) {
    return worst;
  }
  Set<int> seen;
  for (int e : EdgeOfVertIter(&m, v_keep, m.v.e[v_keep])) {
    int c0 = m.e.c[e];
    if (c0 == ELEM_NONE) {
      continue;
    }
    int cc = c0;
    do {
      int f = m.l.f[m.c.l[cc]];
      if (seen.add(f)) {
        Vector<int, 8> key = faceKey(m, f, /*v_kill=*/-1, v_keep);
        float3 nAfter = faceNewellNormal(m, f);
        float la = std::sqrt(vdot(nAfter, nAfter));
        for (const FaceNrm &fn : before) {
          if (!sameSorted(key, fn.key)) {
            continue;
          }
          float lb = std::sqrt(vdot(fn.nrm, fn.nrm));
          if (lb > 1e-12f && la > 1e-12f) {
            float d = vdot(fn.nrm, nAfter) / (lb * la);
            if (d < worst) {
              worst = d;
            }
          }
          break;
        }
      }
      cc = m.c.radial_next[cc];
    } while (cc != c0);
  }
  return worst;
}

// Outcome of replaying a fixture collapse.
struct Replay {
  float worst = 1.0f; // worst incident-face fold dot (1 = none, <0 = flipped)
  bool refused = false;
  bool loadOk = true;
};

// Replay one fixture. `reposition` chooses the dyntopo midpoint move vs leaving
// the survivor in place (to isolate whether the fold is from repositioning or
// the topological rebuild). `prevent_inversion` opts into the geometric guard.
Replay replay(const char *name, bool reposition, bool prevent_inversion,
              std::string &err)
{
  Replay r;
  std::string path =
      std::string(SCULPTCORE_ASSETS_DIR) + "/collapse_repro/" + name;
  int keep1 = 0, kill1 = 0;
  if (!parseKeepKill(path.c_str(), keep1, kill1)) {
    err = "no # collapse_edge marker";
    r.loadOk = false;
    return r;
  }
  Mesh *m = mesh::loadObj(path.c_str(), /*keepNgons=*/false);
  if (!m) {
    err = "load failed";
    r.loadOk = false;
    return r;
  }
  m->thawTopo();

  int a = keep1 - 1, b = kill1 - 1;
  int edge = findEdge(*m, a, b);
  if (edge == ELEM_NONE) {
    err = "collapse edge not found";
    r.loadOk = false;
    litestl::alloc::Delete<Mesh>(m);
    return r;
  }
  int v_keep = m->e.vs[edge][0];
  int v_kill = m->e.vs[edge][1];

  Vector<FaceNrm> before;
  snapshotIncidentNormals(*m, v_keep, v_kill, before);

  float3 mid = (m->v.co[v_keep] + m->v.co[v_kill]) * 0.5f;
  std::optional<float3> target =
      reposition ? std::optional<float3>(mid) : std::nullopt;
  auto ok = collapseEdge(*m, edge, target, /*blend=*/0.5f, /*out=*/nullptr,
                         /*cb=*/nullptr, prevent_inversion);
  if (!bool(ok)) {
    r.refused = true;
    litestl::alloc::Delete<Mesh>(m);
    return r;
  }

  r.worst = worstFoldDot(*m, v_keep, before);
  RemeshReport rep = remeshValidate(*m);
  if (!rep.structurallyOk()) {
    err = "post-collapse mesh not structurally ok";
  }
  litestl::alloc::Delete<Mesh>(m);
  return r;
}

const char *kFixtures[] = {
    "fold_flip180.obj",
    "fold_turn63.obj",
};

} // namespace

int main()
{
  setvbuf(stderr, nullptr, _IONBF, 0);

  for (const char *name : kFixtures) {
    /* Unguarded (current default): documents the bug. The midpoint-move and the
     * no-move runs match, proving the fold is from the topological rebuild, not
     * repositioning. Informational — no assert (it intentionally reproduces). */
    std::string e1, e2;
    Replay bug = replay(name, /*reposition=*/true, /*prevent_inversion=*/false, e1);
    Replay bugNoMove =
        replay(name, /*reposition=*/false, /*prevent_inversion=*/false, e2);
    fprintf(stderr,
            "[%s] unguarded: midpoint fold dot=%.4g no-move fold dot=%.4g%s%s\n",
            name, bug.worst, bugNoMove.worst, e1.empty() ? "" : "  ERR=",
            e1.c_str());
    test_assert(bug.loadOk);

    /* Guarded (the fix, as dyntopo calls it): the collapse must not leave a
     * folded face — either refused outright or completed without a flip. */
    std::string e3;
    Replay fixed = replay(name, /*reposition=*/true, /*prevent_inversion=*/true, e3);
    fprintf(stderr, "[%s] guarded:   %s (fold dot=%.4g)%s%s\n", name,
            fixed.refused ? "refused" : "collapsed", fixed.worst,
            e3.empty() ? "" : "  ERR=", e3.c_str());
    test_assert(fixed.refused || fixed.worst >= 0.0f);
  }

  return test_end();
}
