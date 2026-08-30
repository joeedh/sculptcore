/* Per-layer merge policy (mesh/attr_merge.h) as exercised by the two topology
 * operators that create or merge a vertex: edge split (dst is a fresh vertex at
 * the endpoints' midpoint) and edge collapse (dst IS src0, placed at merged_co).
 *
 * The cases that matter are the ones the generic lerp gets wrong: a
 * generation-guarded, lazily-materialized snapshot column, and a rest-position
 * column whose meaning is a delta against the live position. */

#include "test_util.h"

#include "mesh/attr_merge.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "mesh/sculpt_layers.h"
#include "mesh/utils/edge_collapse.h"
#include "mesh/utils/edge_split.h"
#include "mesh/utils/triangulate.h"

#include "litestl/math/vector.h"

#include <cmath>
#include <cstdio>

test_init;

using namespace sculptcore;
using namespace sculptcore::mesh;
using namespace litestl::math;

static bool near3(float3 a, float3 b, float eps = 1e-5f)
{
  return (a - b).length() < eps;
}

/* mesh_shapes allocates through the tracked allocator, so the mesh must go back
 * the same way — plain `delete` corrupts the heap. */
struct MeshPtr {
  Mesh *m = nullptr;
  MeshPtr(int dimen)
  {
    m = createCube(dimen, 0.5f);
    m->thawTopo();
    triangulateMesh(*m);
    m->recalc_normals();
  }
  ~MeshPtr()
  {
    litestl::alloc::Delete<Mesh>(m);
  }
  MeshPtr(const MeshPtr &) = delete;
  Mesh *operator->()
  {
    return m;
  }
  Mesh &operator*()
  {
    return *m;
  }
};

/* Any interior edge of the cube, i.e. one whose collapse the link condition
 * accepts. Returns ELEM_NONE if none is found. */
static int findCollapsibleEdge(Mesh &m)
{
  for (int ei : m.e) {
    if (m.e.c[ei] != ELEM_NONE) {
      return ei;
    }
  }
  return ELEM_NONE;
}

/* --- resolveMergePolicy / AttrGroup::ensure stamping --- */
static void testPolicyResolution()
{
  test_assert(resolveMergePolicy(AttrType::FLOAT3, "positions").merge ==
              AttrMerge::DEFAULT);
  test_assert(resolveMergePolicy(AttrType::FLOAT3, "mycolors").merge ==
              AttrMerge::DEFAULT);
  /* Keyed on (type, name) together: the right name at the wrong type is a
   * different layer and must not inherit the policy. */
  test_assert(resolveMergePolicy(AttrType::FLOAT, ".brush.disp.vec").merge ==
              AttrMerge::DEFAULT);

  AttrMergePolicy disp = resolveMergePolicy(AttrType::FLOAT3, ".brush.disp.vec");
  test_assert(disp.merge == AttrMerge::CUSTOM && disp.fn != nullptr);
  test_assert(resolveMergePolicy(AttrType::INT, ".brush.disp.gen").merge ==
              AttrMerge::NONE);
  /* The normal snapshot shares the disp generation but not its handler. */
  AttrMergePolicy no = resolveMergePolicy(AttrType::FLOAT3, ".brush.orig.no");
  test_assert(no.merge == AttrMerge::CUSTOM && no.fn != nullptr && no.fn != disp.fn);
  test_assert(resolveMergePolicy(AttrType::INT, ".brush.dab.gen").merge ==
              AttrMerge::CUSTOM);
  test_assert(resolveMergePolicy(AttrType::FLOAT3, SCULPT_LAYER_REST_ATTR).merge ==
              AttrMerge::CUSTOM);

  /* ensure() is the single place a layer is born, including when the loader
   * rebuilds a domain — so a policy layer gets its policy back with no help
   * from the file format. */
  MeshPtr m(3);
  m->v.attrs.ensure(AttrType::FLOAT3, ".brush.disp.vec");
  m->v.attrs.ensure(AttrType::FLOAT3, "userdata");
  AttrRef a = m->v.attrs.find_attribute(AttrType::FLOAT3, ".brush.disp.vec");
  AttrRef b = m->v.attrs.find_attribute(AttrType::FLOAT3, "userdata");
  test_assert(a.merge == AttrMerge::CUSTOM && a.merge_fn == disp.fn);
  test_assert(b.merge == AttrMerge::DEFAULT && b.merge_fn == nullptr);
}

/* --- DEFAULT / COPY_SRC0 / NONE on a plain user layer --- */
static void testGenericPolicies()
{
  struct Case {
    AttrMerge merge;
    float want;
  };
  const Case cases[] = {
      {AttrMerge::DEFAULT, 3.0f},   /* lerp(1,5,0.5) */
      {AttrMerge::COPY_SRC0, 1.0f}, /* snap to the nearest source */
      /* Untouched: the new vertex keeps the layer's value-initialized default,
       * not either source (the other two live verts hold -1). */
      {AttrMerge::NONE, 0.0f},
  };

  for (const Case &c : cases) {
    MeshPtr m(3);
    AttrRef &attr = m->v.attrs.ensure(AttrType::FLOAT, "userdata", true);
    attr.merge = c.merge;
    auto *data = attr.get_data<float>();
    for (int vi : m->v) {
      (*data)[vi] = -1.0f;
    }

    int e = findCollapsibleEdge(*m);
    test_assert(e != ELEM_NONE);
    (*data)[m->e.vs[e][0]] = 1.0f;
    (*data)[m->e.vs[e][1]] = 5.0f;

    EdgeSplitResult res;
    test_assert(bool(splitEdge(*m, e, &res)));
    // `attr` may dangle if the split grew the layer set, so the test looks up
    // the attribute again by name below. The split does not grow the layer set
    // in this test, but the AttrData pointer returned is what the assertion uses.
    auto *out = m->v.attrs.find_attribute(AttrType::FLOAT, "userdata").get_data<float>();
    test_assert(std::fabs(out->safe_get(res.new_vert) - c.want) < 1e-5f);
  }
}

/* --- bool columns OR by default (NOCOPY opts back into a src0 copy) --- */
static void testBoolOrMerge()
{
  struct Case {
    bool b0, b1, nocopy;
    bool want;
  };
  const Case cases[] = {
      {false, true, false, true},
      {true, false, false, true},
      {false, false, false, false},
      {false, true, true, false}, /* NOCOPY: plain src0 copy */
  };

  for (const Case &c : cases) {
    // Splitting the edge produces dst, the fresh midpoint vertex.
    {
      MeshPtr m(3);
      AttrRef &attr = m->v.attrs.ensure(AttrType::BOOL, "flag", true);
      if (c.nocopy) {
        attr.flag |= AttrFlag::NOCOPY;
      }
      auto *view = static_cast<BoolAttrView *>(attr.data);
      int e = findCollapsibleEdge(*m);
      test_assert(e != ELEM_NONE);
      view->set(m->e.vs[e][0], c.b0);
      view->set(m->e.vs[e][1], c.b1);

      EdgeSplitResult res;
      test_assert(bool(splitEdge(*m, e, &res)));
      auto out = m->v.attrs.find_attribute(AttrType::BOOL, "flag");
      test_assert((*static_cast<BoolAttrView *>(out.data))[res.new_vert] == c.want);
    }
    /* Collapse: dst IS src0 (the survivor), ORing the killed endpoint in. */
    {
      MeshPtr m(3);
      AttrRef &attr = m->v.attrs.ensure(AttrType::BOOL, "flag", true);
      if (c.nocopy) {
        attr.flag |= AttrFlag::NOCOPY;
      }
      auto *view = static_cast<BoolAttrView *>(attr.data);
      int e = findCollapsibleEdge(*m);
      test_assert(e != ELEM_NONE);
      int v_keep = m->e.vs[e][0];
      view->set(v_keep, c.b0);
      view->set(m->e.vs[e][1], c.b1);

      const float3 mid = (m->v.co[m->e.vs[e][0]] + m->v.co[m->e.vs[e][1]]) * 0.5f;
      EdgeCollapseResult res;
      test_assert(bool(collapseEdge(*m, e, mid, 0.5f, &res)));
      auto out = m->v.attrs.find_attribute(AttrType::BOOL, "flag");
      test_assert((*static_cast<BoolAttrView *>(out.data))[v_keep] == c.want);
    }
  }
}

/* --- corner float layers average across a collapse, per wedge --- */
static void testCornerWedgeBlend()
{
  /* Seed a float4 corner layer (a corner color) with A on every corner at
   * v_keep and B on every corner at v_kill; after a blend-0.5 collapse every
   * corner at the survivor must read (A+B)/2 — the same wedge machinery UV
   * maps use, generalized. Corners elsewhere must be untouched. */
  MeshPtr m(3);
  AttrRef &attr = m->c.attrs.ensure(AttrType::FLOAT4, "ccolor", true);
  auto *data = attr.get_data<float4>();

  int e = findCollapsibleEdge(*m);
  test_assert(e != ELEM_NONE);
  int v0 = m->e.vs[e][0], v1 = m->e.vs[e][1];
  const float4 A(1.0f, 0.0f, 0.0f, 1.0f), B(0.0f, 1.0f, 0.0f, 1.0f);
  const float4 other(0.0f, 0.0f, 1.0f, 1.0f);
  for (int ci : m->c) {
    (*data)[ci] = (m->c.v[ci] == v0) ? A : (m->c.v[ci] == v1) ? B : other;
  }

  const float3 mid = (m->v.co[v0] + m->v.co[v1]) * 0.5f;
  EdgeCollapseResult res;
  test_assert(bool(collapseEdge(*m, e, mid, 0.5f, &res)));

  auto *out = m->c.attrs.find_attribute(AttrType::FLOAT4, "ccolor").get_data<float4>();
  const float4 want = (A + B) * 0.5f;
  int at_survivor = 0, elsewhere = 0;
  for (int ci : m->c) {
    const float4 got = out->safe_get(ci);
    if (m->c.v[ci] == v0) {
      test_assert((got - want).length() < 1e-5f);
      at_survivor++;
    } else {
      test_assert((got - other).length() < 1e-5f);
      elsewhere++;
    }
  }
  test_assert(at_survivor > 0 && elsewhere > 0);
}

/* --- `.brush.disp.*`: the gen-guarded, lazily-paged displacement field --- */
static void testDispFieldSplit()
{
  /* (stamp v0, stamp v1). An unstamped endpoint has displaced by zero this
   * stroke, so that — not the unmaterialized page default — is what the other
   * endpoint's displacement blends against. */
  struct Case {
    bool s0, s1;
    const char *tag;
  };
  const Case cases[] = {
      {true, true, "both"},
      {true, false, "src0-only"},
      {false, true, "src1-only"},
      {false, false, "neither"},
  };

  for (const Case &c : cases) {
    MeshPtr m(3);
    m->v.attrs.ensure(AttrType::FLOAT3, ".brush.disp.vec");
    m->v.attrs.ensure(AttrType::INT, ".brush.disp.gen");
    auto *disp =
        m->v.attrs.find_attribute(AttrType::FLOAT3, ".brush.disp.vec").get_data<float3>();
    auto *gen =
        m->v.attrs.find_attribute(AttrType::INT, ".brush.disp.gen").get_data<int>();

    int e = findCollapsibleEdge(*m);
    test_assert(e != ELEM_NONE);
    int v0 = m->e.vs[e][0], v1 = m->e.vs[e][1];

    /* Stroke: push the stamped verts off the surface, recording the push as
     * their displacement (the §2 invariant). */
    const float3 rest0 = m->v.co[v0], rest1 = m->v.co[v1];
    const float3 push(0.0f, 0.0f, 10.0f);
    if (c.s0) {
      disp->materialize(v0);
      (*disp)[v0] = push;
      gen->materialize(v0);
      (*gen)[v0] = 7;
      m->v.co[v0] = rest0 + push;
    }
    if (c.s1) {
      disp->materialize(v1);
      (*disp)[v1] = push;
      gen->materialize(v1);
      (*gen)[v1] = 7;
      m->v.co[v1] = rest1 + push;
    }

    EdgeSplitResult res;
    test_assert(bool(splitEdge(*m, e, &res)));
    int vm = res.new_vert;

    auto *outDisp =
        m->v.attrs.find_attribute(AttrType::FLOAT3, ".brush.disp.vec").get_data<float3>();
    auto *outGen =
        m->v.attrs.find_attribute(AttrType::INT, ".brush.disp.gen").get_data<int>();

    if (!c.s0 && !c.s1) {
      // Neither source vertex carries the generation stamp, so the split must
      // not fabricate one for the new midpoint vertex.
      test_assert(outGen->safe_get(vm) == 0);
      continue;
    }
    test_assert(outGen->safe_get(vm) == 7);
    /* What the field exists for: the derived base `co - disp` lands on the
     * stroke-start surface's own midpoint in all three cases — exactly the
     * invariant a naive lerp of the raw column breaks. */
    float3 base = m->v.co[vm] - outDisp->safe_get(vm);
    float3 want = (rest0 + rest1) * 0.5f;
    if (!near3(base, want)) {
      fprintf(stderr,
              "  [%s] base = %.4f,%.4f,%.4f want %.4f,%.4f,%.4f\n",
              c.tag,
              base[0],
              base[1],
              base[2],
              want[0],
              want[1],
              want[2]);
    }
    test_assert(near3(base, want));
  }
}

/* The collapse case: dst == src0 and the survivor lands at merged_co, so the
 * handler must still read the sources' pre-collapse values. */
static void testDispFieldCollapse()
{
  MeshPtr m(4);
  m->v.attrs.ensure(AttrType::FLOAT3, ".brush.disp.vec");
  m->v.attrs.ensure(AttrType::INT, ".brush.disp.gen");
  auto *disp =
      m->v.attrs.find_attribute(AttrType::FLOAT3, ".brush.disp.vec").get_data<float3>();
  auto *gen = m->v.attrs.find_attribute(AttrType::INT, ".brush.disp.gen").get_data<int>();

  int e = findCollapsibleEdge(*m);
  test_assert(e != ELEM_NONE);
  int v0 = m->e.vs[e][0], v1 = m->e.vs[e][1];
  const float3 rest0 = m->v.co[v0], rest1 = m->v.co[v1];

  const float3 push(0.0f, 0.0f, 10.0f);
  disp->materialize(v0);
  (*disp)[v0] = push;
  gen->materialize(v0);
  (*gen)[v0] = 3;
  m->v.co[v0] = rest0 + push; /* v1 stays unstamped */

  const float3 mid = (m->v.co[v0] + m->v.co[v1]) * 0.5f;
  EdgeCollapseResult res;
  test_assert(bool(collapseEdge(*m, e, mid, 0.5f, &res)));

  auto *outDisp =
      m->v.attrs.find_attribute(AttrType::FLOAT3, ".brush.disp.vec").get_data<float3>();
  auto *outGen =
      m->v.attrs.find_attribute(AttrType::INT, ".brush.disp.gen").get_data<int>();
  test_assert(outGen->safe_get(v0) == 3);
  test_assert(near3(m->v.co[v0] - outDisp->safe_get(v0), (rest0 + rest1) * 0.5f));
}

/* --- `.slayer.rest`: rest is only meaningful as `co - rest` --- */
static void testSculptLayerRest()
{
  MeshPtr m(4);
  AttrRef &restAttr = m->v.attrs.ensure(AttrType::FLOAT3, SCULPT_LAYER_REST_ATTR, true);
  auto *rest = restAttr.get_data<float3>();
  for (int vi : m->v) {
    (*rest)[vi] = m->v.co[vi];
  }

  int e = findCollapsibleEdge(*m);
  test_assert(e != ELEM_NONE);
  int v0 = m->e.vs[e][0], v1 = m->e.vs[e][1];

  const float3 d0(0.0f, 0.0f, 2.0f), d1(0.0f, 0.0f, 6.0f);
  m->v.co[v0] = (*rest)[v0] + d0;
  m->v.co[v1] = (*rest)[v1] + d1;

  /* Deliberately NOT the endpoint midpoint — a quadric/QEM placement, where the
   * plain lerp's `co == lerp(sources)` premise fails. */
  const float3 placed = m->v.co[v0] * 0.25f + m->v.co[v1] * 0.75f;
  EdgeCollapseResult res;
  test_assert(bool(collapseEdge(*m, e, placed, 0.5f, &res)));

  auto *outRest = m->v.attrs.find_attribute(AttrType::FLOAT3, SCULPT_LAYER_REST_ATTR)
                      .get_data<float3>();
  float3 delta = m->v.co[v0] - outRest->safe_get(v0);
  test_assert(near3(delta, (d0 + d1) * 0.5f));
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  testPolicyResolution();
  testGenericPolicies();
  testBoolOrMerge();
  testCornerWedgeBlend();
  testDispFieldSplit();
  testDispFieldCollapse();
  testSculptLayerRest();

  printf("attr_merge test done\n");
  return test_end();
}
