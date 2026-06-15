/* M4 isolation: meshlog undo/redo of dyntopo operators (split/collapse),
 * with NO brush, NO freeze/thaw, NO spatial tree. Tells us whether the
 * meshlog's replay of the operators' make/kill cascade is itself correct
 * (this passes) or broken (this crashes/fails), separate from the debug-app
 * freeze-thaw + tree-rebuild interaction. */
#include "test_util.h"

#include "litestl/math/vector.h"
#include "litestl/util/alloc.h"
#include "dyntopo/dyntopo.h"
#include "mesh/attribute.h"
#include "mesh/boundary.h"
#include "mesh/mesh.h"
#include "mesh/mesh_iter.h"
#include "mesh/utils/edge_collapse.h"
#include "mesh/utils/edge_split.h"
#include "meshlog/meshlog_base.h"
#include "spatial/spatial.h"

#include <cstdio>

test_init;

#define TASSERT(expr)                                                                    \
  do {                                                                                   \
    if (!(expr)) {                                                                        \
      retval = 1;                                                                         \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                  \
      fflush(stderr);                                                                     \
    }                                                                                     \
  } while (0)

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace sculptcore::meshlog;
using litestl::math::float3;

namespace {

struct Counts {
  int v, e, c, l, f;
};
Counts counts(Mesh &m)
{
  return {m.v.count, m.e.count, m.c.count, m.l.count, m.f.count};
}
bool eq(const Counts &a, const Counts &b)
{
  return a.v == b.v && a.e == b.e && a.c == b.c && a.l == b.l && a.f == b.f;
}
void pr(const char *tag, const Counts &c)
{
  printf("  %s: v=%d e=%d c=%d l=%d f=%d\n", tag, c.v, c.e, c.c, c.l, c.f);
}

/* Disk + radial + loop cycle integrity (no freed refs, triangles only). */
bool manifold(Mesh &m, const char *tag)
{
  for (int ei : m.e) {
    int v1 = m.e.vs[ei][0], v2 = m.e.vs[ei][1];
    if (v1 < 0 || v2 < 0 || m.v.freemap[v1] || m.v.freemap[v2] || v1 == v2) {
      fprintf(stderr, "[%s] edge %d bad verts %d %d\n", tag, ei, v1, v2);
      return false;
    }
  }
  for (int vi : m.v) {
    int e0 = m.v.e[vi];
    if (e0 == ELEM_NONE) continue;
    int steps = 0, ec = e0;
    do {
      int side = m.e.vs[ec][0] == vi ? 0 : 1;
      int next = m.e.disk[ec][side * 2 + 1], prev = m.e.disk[ec][side * 2];
      int sn = m.e.vs[next][0] == vi ? 0 : 1, sp = m.e.vs[prev][0] == vi ? 0 : 1;
      if (m.e.disk[next][sn * 2] != ec || m.e.disk[prev][sp * 2 + 1] != ec) {
        fprintf(stderr, "[%s] disk mismatch v=%d e=%d\n", tag, vi, ec);
        return false;
      }
      ec = next;
      if (++steps > 1000000) { fprintf(stderr, "[%s] disk loop\n", tag); return false; }
    } while (ec != e0);
  }
  for (int ei : m.e) {
    int c0 = m.e.c[ei];
    if (c0 == ELEM_NONE) continue;
    int steps = 0, cc = c0;
    do {
      if (m.c.e[cc] != ei) { fprintf(stderr, "[%s] radial c.e\n", tag); return false; }
      int rn = m.c.radial_next[cc], rp = m.c.radial_prev[cc];
      if (m.c.radial_prev[rn] != cc || m.c.radial_next[rp] != cc) {
        fprintf(stderr, "[%s] radial mismatch e=%d c=%d\n", tag, ei, cc);
        return false;
      }
      cc = rn;
      if (++steps > 1000000) { fprintf(stderr, "[%s] radial loop\n", tag); return false; }
    } while (cc != c0);
  }
  for (int fi : m.f) {
    int li = m.f.l[fi], c0 = m.l.c[li], cc = c0, n = 0;
    do {
      if (m.c.l[cc] != li) { fprintf(stderr, "[%s] c.l\n", tag); return false; }
      int cn = m.c.next[cc];
      if (m.c.prev[cn] != cc) { fprintf(stderr, "[%s] loop prev/next\n", tag); return false; }
      cc = cn;
      if (++n > 1000000) { fprintf(stderr, "[%s] face loop\n", tag); return false; }
    } while (cc != c0);
    if (n != m.l.size[li]) { fprintf(stderr, "[%s] size\n", tag); return false; }
  }
  return true;
}

int findEdge(Mesh &m, int a, int b)
{
  for (int ei : m.e) {
    int v0 = m.e.vs[ei][0], v1 = m.e.vs[ei][1];
    if ((v0 == a && v1 == b) || (v0 == b && v1 == a)) return ei;
  }
  return ELEM_NONE;
}

/* Two triangles sharing edge v0-v2. */
void buildTwoTris(Mesh &m, int &sharedEdge, int &v0_, int &v2_)
{
  int v0 = m.make_vertex(float3(0, 0, 0));
  int v1 = m.make_vertex(float3(1, 0, 0));
  int v2 = m.make_vertex(float3(1, 1, 0));
  int v3 = m.make_vertex(float3(0, 1, 0));
  int t0[3] = {v0, v1, v2};
  int t1[3] = {v0, v2, v3};
  m.make_face(std::span<int>(t0, 3));
  m.make_face(std::span<int>(t1, 3));
  sharedEdge = findEdge(m, v0, v2);
  v0_ = v0;
  v2_ = v2;
}

/* Triangulated n*n grid in z=0, spanning [-0.5, 0.5]. */
Mesh *makeTriGrid(int n)
{
  Mesh *m = litestl::alloc::New<Mesh>("undo grid");
  litestl::util::Vector<int> grid;
  grid.resize(n * n);
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      float fx = float(x) / float(n - 1) - 0.5f;
      float fy = float(y) / float(n - 1) - 0.5f;
      grid[y * n + x] = m->make_vertex(float3(fx, fy, 0.0f));
    }
  }
  for (int y = 0; y < n - 1; y++) {
    for (int x = 0; x < n - 1; x++) {
      int a = grid[y * n + x], b = grid[y * n + x + 1];
      int c = grid[(y + 1) * n + x + 1], d = grid[(y + 1) * n + x];
      int t0[3] = {a, b, c};
      int t1[3] = {a, c, d};
      m->make_face(std::span<int>(t0, 3));
      m->make_face(std::span<int>(t1, 3));
    }
  }
  return m;
}

/* Every live face is owned by exactly one leaf, owner ids agree, the per-leaf
 * unique_faces hold only live faces, and every live face/vert is covered.
 * Returns the owned-face count, or -1 on any inconsistency. (Mirrors
 * test_spatial_dyntopo's validator — the invariant the tree must hold after an
 * undo/redo that reconciles ownership.) */
int validateOwnership(spatial::SpatialTree *tree, Mesh *m, const char *tag)
{
  int owned = 0, ownedV = 0;
  for (auto *leaf : tree->leaves()) {
    if (!leaf->data) continue;
    for (int f : leaf->data->unique_faces) {
      if (m->f.freemap[f]) {
        fprintf(stderr, "[%s] leaf %d owns dead face %d\n", tag, leaf->id, f);
        return -1;
      }
      if (tree->treeMesh.f.node[f] != leaf->id) {
        fprintf(stderr, "[%s] face %d owner %d != leaf %d\n", tag, f,
                tree->treeMesh.f.node[f], leaf->id);
        return -1;
      }
      owned++;
    }
    for (int v : leaf->data->unique_verts) {
      if (m->v.freemap[v]) {
        fprintf(stderr, "[%s] leaf %d owns dead vert %d\n", tag, leaf->id, v);
        return -1;
      }
      ownedV++;
    }
  }
  for (int f : m->f) {
    if (tree->treeMesh.f.node[f] == 0) {
      fprintf(stderr, "[%s] live face %d is unowned\n", tag, f);
      return -1;
    }
  }
  if (ownedV != m->v.count) {
    fprintf(stderr, "[%s] %d verts owned but mesh has %d\n", tag, ownedV, m->v.count);
    return -1;
  }
  return owned;
}

} // namespace

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  /* --- split, undo, redo --- */
  {
    Mesh m;
    int e, v0, v2;
    buildTwoTris(m, e, v0, v2);
    TASSERT(manifold(m, "split-pre"));
    MeshLog log;
    log.setActiveMesh(&m);

    Counts before = counts(m);
    log.beginStep(true);
    EdgeSplitResult res;
    auto ok = splitEdge(m, e, &res, log.callbacks());
    TASSERT(bool(ok));
    log.endStep();
    Counts after = counts(m);
    pr("split before", before);
    pr("split after", after);
    TASSERT(manifold(m, "split-post"));

    log.undo(&m, nullptr);
    pr("split undo", counts(m));
    TASSERT(manifold(m, "split-undo"));
    TASSERT(eq(counts(m), before));

    log.redo(&m, nullptr);
    pr("split redo", counts(m));
    TASSERT(manifold(m, "split-redo"));
    TASSERT(eq(counts(m), after));
  }

  /* --- collapse, undo, redo --- */
  {
    Mesh m;
    int e, v0, v2;
    buildTwoTris(m, e, v0, v2);
    /* split first to make an interior vertex that can collapse cleanly. */
    splitEdge(m, e, nullptr);
    TASSERT(manifold(m, "collapse-pre"));
    /* collapse an edge incident to the midpoint back. */
    int mid = -1;
    for (int vi : m.v) {
      if (vi >= 4) { mid = vi; break; }
    }
    int ce = ELEM_NONE;
    if (mid >= 0) {
      ce = m.v.e[mid];
    }

    MeshLog log;
    log.setActiveMesh(&m);
    Counts before = counts(m);
    log.beginStep(true);
    if (ce != ELEM_NONE) {
      collapseEdge(m, ce, std::nullopt, 0.5f, nullptr, log.callbacks());
    }
    log.endStep();
    Counts after = counts(m);
    pr("collapse before", before);
    pr("collapse after", after);
    TASSERT(manifold(m, "collapse-post"));

    log.undo(&m, nullptr);
    pr("collapse undo", counts(m));
    TASSERT(manifold(m, "collapse-undo"));
    TASSERT(eq(counts(m), before));

    log.redo(&m, nullptr);
    TASSERT(manifold(m, "collapse-redo"));
    TASSERT(eq(counts(m), after));
  }

  /* --- full runDyntopoRemesh (many ops) in one logged step, undo, redo.
   *     No tree, no freeze: isolates the many-op meshlog replay. --- */
  {
    Mesh m;
    /* triangulated 7x7 grid in z=0, spanning [-0.5,0.5]. */
    const int N = 7;
    int grid[N * N];
    for (int y = 0; y < N; y++) {
      for (int x = 0; x < N; x++) {
        float fx = float(x) / float(N - 1) - 0.5f;
        float fy = float(y) / float(N - 1) - 0.5f;
        grid[y * N + x] = m.make_vertex(float3(fx, fy, 0.0f));
      }
    }
    for (int y = 0; y < N - 1; y++) {
      for (int x = 0; x < N - 1; x++) {
        int a = grid[y * N + x], b = grid[y * N + x + 1];
        int c = grid[(y + 1) * N + x + 1], d = grid[(y + 1) * N + x];
        int t0[3] = {a, b, c};
        int t1[3] = {a, c, d};
        m.make_face(std::span<int>(t0, 3));
        m.make_face(std::span<int>(t1, 3));
      }
    }
    TASSERT(manifold(m, "dab-pre"));

    MeshLog log;
    log.setActiveMesh(&m);
    Counts before = counts(m);

    dyntopo::DynTopoParams p;
    p.l_max = 0.08f;
    p.l_min = 0.01f;
    p.mode = dyntopo::DynTopoMode::Subdivide;

    log.beginStep(true);
    dyntopo::DynTopoStats st = dyntopo::runDyntopoRemesh(
        m, float3(0, 0, 0), 0.3f, p, /*seed=*/123u, log.callbacks());
    log.endStep();
    Counts after = counts(m);
    pr("dab before", before);
    pr("dab after", after);
    printf("  dab: %d splits, %d rounds\n", st.splits, st.rounds);
    TASSERT(st.splits > 0);
    TASSERT(manifold(m, "dab-post"));

    log.undo(&m, nullptr);
    pr("dab undo", counts(m));
    TASSERT(manifold(m, "dab-undo"));
    TASSERT(eq(counts(m), before));

    log.redo(&m, nullptr);
    pr("dab redo", counts(m));
    TASSERT(manifold(m, "dab-redo"));
    TASSERT(eq(counts(m), after));
  }

  /* --- polygroup preservation across a dab + undo/redo.
   *     Paint a uniform face "group" = 7, run a full dab (split + collapse +
   *     flip), and require every face to still read 7. A flip that rebuilds its
   *     two faces via make_face without carrying the face attr row would zero
   *     their group — so a uniform group is an unambiguous detector (no
   *     poly-group boundary edges exist, so the flip sweep is unconstrained).
   *     Then round-trip the meshlog topo chunk and require the group survives
   *     undo and redo. --- */
  {
    Mesh m;
    const int N = 7;
    int grid[N * N];
    for (int y = 0; y < N; y++) {
      for (int x = 0; x < N; x++) {
        float fx = float(x) / float(N - 1) - 0.5f;
        float fy = float(y) / float(N - 1) - 0.5f;
        grid[y * N + x] = m.make_vertex(float3(fx, fy, 0.0f));
      }
    }
    for (int y = 0; y < N - 1; y++) {
      for (int x = 0; x < N - 1; x++) {
        int a = grid[y * N + x], b = grid[y * N + x + 1];
        int c = grid[(y + 1) * N + x + 1], d = grid[(y + 1) * N + x];
        int t0[3] = {a, b, c};
        int t1[3] = {a, c, d};
        m.make_face(std::span<int>(t0, 3));
        m.make_face(std::span<int>(t1, 3));
      }
    }

    /* Paint a single uniform polygroup. */
    AttrRef &gref = m.f.attrs.ensure(AttrType::INT, boundary::FACE_GROUP, true);
    auto *group = static_cast<AttrData<int> *>(gref.data);
    for (int fi : m.f) {
      (*group)[fi] = 7;
    }

    auto allSeven = [&](const char *tag) -> bool {
      auto *g = static_cast<AttrData<int> *>(
          m.f.attrs.find_attribute(AttrType::INT, boundary::FACE_GROUP).data);
      int bad = 0, total = 0;
      for (int fi : m.f) {
        total++;
        if ((*g)[fi] != 7) bad++;
      }
      if (bad) {
        fprintf(stderr, "[%s] %d/%d faces lost their polygroup\n", tag, bad, total);
      }
      return bad == 0;
    };

    MeshLog log;
    log.setActiveMesh(&m);
    Counts before = counts(m);

    dyntopo::DynTopoParams p;
    p.l_max = 0.08f;
    p.l_min = 0.01f;
    p.mode = dyntopo::DynTopoMode::Both; /* default; exercises the flip sweep */

    log.beginStep(true);
    dyntopo::DynTopoStats st = dyntopo::runDyntopoRemesh(
        m, float3(0, 0, 0), 0.3f, p, /*seed=*/123u, log.callbacks());
    log.endStep();
    pr("pg dab after", counts(m));
    printf("  pg dab: %d splits, %d collapses, %d flips\n", st.splits, st.collapses,
           st.flips);
    TASSERT(st.flips > 0); /* the test only detects the bug if flips ran */
    TASSERT(manifold(m, "pg-dab"));
    TASSERT(allSeven("pg-dab")); /* the flip-preservation check */

    log.undo(&m, nullptr);
    TASSERT(manifold(m, "pg-undo"));
    TASSERT(eq(counts(m), before));
    TASSERT(allSeven("pg-undo")); /* topo log must restore face attrs */

    log.redo(&m, nullptr);
    TASSERT(manifold(m, "pg-redo"));
    TASSERT(allSeven("pg-redo"));
  }

  /* --- meshlog undo/redo WITH a live spatial tree.
   *     The topo log restores mesh elements, but the tree's incremental face
   *     ownership (`.spatial.f.node`, a TEMP attr) is NOT logged — so undo/redo
   *     must reconcile it via add_face/remove_face, or the tree (and the GPU
   *     buffers / rendering) go stale and the undo is invisible. Run a dab
   *     through the combined meshlog+spatial callbacks, then undo + redo, and
   *     require the tree to own every live face exactly once at each step. --- */
  /* Subdivide-only and Both (split + collapse + flip): both kill & recreate
   * faces, so both exercise the ownership reconcile; Both also flips (faces
   * rewired in place) and collapses (verts killed). */
  for (dyntopo::DynTopoMode mode :
       {dyntopo::DynTopoMode::Subdivide, dyntopo::DynTopoMode::Both}) {
    Mesh *m = makeTriGrid(13); /* spacing ~0.083 */
    auto *tree = litestl::alloc::New<spatial::SpatialTree>("undo tree", m);
    tree->leaf_limit = 96;
    tree->buildAll();

    int fBefore = m->f.count;
    TASSERT(validateOwnership(tree, m, "tree-build") == fBefore);

    MeshLog log;
    log.setActiveMesh(m);

    /* Combined callbacks: meshlog first (snapshots), then the tree's
     * incremental-ownership handlers — mirrors brush_executor's fan-out. */
    mesh::MeshCallbacks combined = *log.callbacks();
    mesh::MeshCallbacks *sp = tree->getSpatialCallbacks();
    {
      auto mlFC = combined.onFaceCreate, spFC = sp->onFaceCreate;
      combined.onFaceCreate = [mlFC, spFC](int f) { if (mlFC) mlFC(f); if (spFC) spFC(f); };
      auto mlFK = combined.onFaceKill, spFK = sp->onFaceKill;
      combined.onFaceKill = [mlFK, spFK](int f) { if (mlFK) mlFK(f); if (spFK) spFK(f); };
      auto mlVK = combined.onVertKill, spVK = sp->onVertKill;
      combined.onVertKill = [mlVK, spVK](int v) { if (mlVK) mlVK(v); if (spVK) spVK(v); };
      auto mlFCh = combined.onFaceChange, spFCh = sp->onFaceChange;
      combined.onFaceChange = [mlFCh, spFCh](int f) { if (mlFCh) mlFCh(f); if (spFCh) spFCh(f); };
    }

    dyntopo::DynTopoParams p;
    p.l_max = 0.05f;
    p.l_min = 0.02f; /* > spacing/2 so post-split edges collapse in Both mode */
    p.mode = mode;

    log.beginStep(true);
    dyntopo::DynTopoStats st = dyntopo::runDyntopoRemesh(
        *m, float3(0, 0, 0), 0.3f, p, /*seed=*/42u, &combined);
    log.endStep();
    TASSERT(st.splits > 0);
    int fAfter = m->f.count;
    tree->applyDeferredNodeSplit();
    TASSERT(validateOwnership(tree, m, "tree-dab") == fAfter);

    log.undo(m, tree);
    tree->applyDeferredNodeSplit();
    printf("  tree undo (mode %d): %d -> %d faces (%d splits, %d collapses, %d flips)\n",
           int(mode), fAfter, m->f.count, st.splits, st.collapses, st.flips);
    TASSERT(m->f.count == fBefore);
    TASSERT(validateOwnership(tree, m, "tree-undo") == fBefore); /* fails pre-fix */

    log.redo(m, tree);
    tree->applyDeferredNodeSplit();
    TASSERT(m->f.count == fAfter);
    TASSERT(validateOwnership(tree, m, "tree-redo") == fAfter);

    litestl::alloc::Delete(tree);
    litestl::alloc::Delete(m);
  }

  printf("dyntopo_undo test: done\n");
  return test_end();
}
