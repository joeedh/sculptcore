// Plane-brush frame policy (brush/plane_frame.h): the executor's per-dab
// substitution of a `@planeFrame` kernel's surfacePos/surfaceNo, matching
// Blender's `calc_brush_plane` semantics. Drives the C++ mesh executor
// directly (applyDab / execProgram / applyResolvedProgram) and asserts:
//   (a) AREA normal on a sphere is radial; the centre sits inside, on the axis
//   (b) a bump under the cursor tilts the point normal but not the AREA normal;
//       the AREA centre is near the region mean, not the hit height
//   (c) centre = AREA re-gathers the region around the resolved centre, so
//       verts outside the cursor filter but inside the falloff move
//   (d) non-accumulate reads the stroke-start surface (frame fixed across
//       dabs); accumulate follows the stroke's own edits
//   (e) VIEW / X / Y / Z fixed normals, and the kernel really uses them
//   (f) original-normal / original-plane hold one quantity, the other updates
//   (g) a mirror image takes the reflected primary frame, not its own gather
//   (h) an empty gather (degenerate normal) skips the dab
//   (i) the stabiliser: 0/0 is a no-op, 0.5/0.5 blends toward the ring mean
//   (j) a program restores the cursor frame between sub-commands, on the raw
//       and the resolved path
//   (k) the codegen flag reaches the metadata query
//   (l) the grid executor: the same policy over a multires domain — AREA
//       frame, data vintage, fixed axes, mirror, skip, and re-gather
#include "test_util.h"

#include "debug/scene.h"
#include "debug/script.h"

#include "brush/brush_executor.h"
#include "brush/brush_program.h"
#include "brush/grid_executor.h"
#include "brush/plane_frame.h"
#include "mesh/mesh.h"
#include "mesh/mesh_shapes.h"
#include "subdiv/grid_domain.h"
#include "subdiv/grid_tree.h"
#include "subdiv/multires.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>

test_init;

using namespace sculptcore::debug_app;
using namespace sculptcore::mesh;
using namespace sculptcore::brush;
using namespace litestl::math;

namespace {

constexpr float kDeg = 3.14159265f / 180.0f;

float angleDeg(const float3 &a, const float3 &b)
{
  const float d = std::fmin(1.0f, std::fmax(-1.0f, a.normalized().dot(b.normalized())));
  return std::acos(d) / kDeg;
}

bool near3(const float3 &a, const float3 &b, float eps)
{
  return (a - b).length() <= eps;
}

struct Fixture {
  Scene scene{64, 64, /*headless=*/true};
  Mesh *m = nullptr;
  std::unique_ptr<CommandExecutor> exec;
  litestl::util::Vector<float3> start;
  SculptBrushes tool = SculptBrushes::CLAY;
  int strokeGen = 0;

  bool run(const char *src)
  {
    auto r = script::run(scene, src, ".");
    if (!r.ok) {
      fprintf(stderr, "  script line %d: %s\n", r.line_no, r.error.c_str());
    }
    test_assert(r.ok);
    return r.ok;
  }

  /** XY grid over [-size/2, size/2]^2 with `height(x, y)` as z. */
  void grid(int n,
            float size,
            const std::function<float(float, float)> &height,
            int leafLimit = 64)
  {
    char buf[256];
    snprintf(buf, sizeof(buf), "make_shape kind=grid n=%d m=%d size=%g\n", n, n, size);
    run(buf);
    m = scene.mesh;
    for (int v : m->v) {
      float3 co = m->v.co[v];
      co[2] = height(co[0], co[1]);
      m->v.co[v] = co;
    }
    m->recalc_normals();
    snprintf(buf, sizeof(buf), "build_spatial leaf_limit=%d depth_limit=12\n", leafLimit);
    run(buf);
    snapshot();
  }

  /** Unit UV sphere; `xform` may distort it (asymmetric geometry). */
  void sphere(const std::function<float3(float3)> &xform = nullptr)
  {
    run("make_shape kind=sphere n=48 m=64 radius=1\n");
    m = scene.mesh;
    if (xform) {
      for (int v : m->v) {
        m->v.co[v] = xform(m->v.co[v]);
      }
    }
    m->recalc_normals();
    run("build_spatial leaf_limit=64 depth_limit=12\n");
    snapshot();
  }

  void snapshot()
  {
    start.clear();
    start.resize(m->v.capacity());
    for (int v : m->v) {
      start[v] = m->v.co[v];
    }
  }

  void
  brush(SculptBrushes t, float radius, float strength, float planeoff, float planeSide)
  {
    tool = t;
    scene.currentTool = t;
    scene.brush.radius = radius;
    scene.brush.strength = strength;
    scene.brush.invert = false;
    scene.brush.planeoff = planeoff;
    scene.brush.planeSide = planeSide;
    scene.brush.writeProps();
  }

  void begin(bool nonAccum)
  {
    exec = std::make_unique<CommandExecutor>(scene.tree, &scene.brush);
    exec->meshLog = &scene.meshLog;
    exec->setNonAccum(nonAccum);
    exec->setStrokeGen(++strokeGen);
    exec->beginStep(false);
  }

  void policy(PlaneNormalMode normal,
              PlaneCenterMode center,
              bool originalNormal = false,
              bool originalPlane = false,
              float nrf = 1.0f,
              float arf = 1.0f,
              float stabN = 0.0f,
              float stabP = 0.0f,
              float3 view = float3(0.0f, 0.0f, 1.0f))
  {
    exec->setPlaneFrame(int(normal),
                        int(center),
                        originalNormal,
                        originalPlane,
                        nrf,
                        arf,
                        stabN,
                        stabP,
                        view[0],
                        view[1],
                        view[2]);
  }

  int dab(float3 center, float3 normal)
  {
    return exec->applyDab(tool, center, normal, scene.brush.radius, nullptr, 0);
  }

  void end()
  {
    exec->endStep();
  }

  /** Normal of the vertex nearest `p` — the "point normal" a raycast returns. */
  float3 pointNormal(float3 p) const
  {
    int best = -1;
    float bestD = 1e30f;
    for (int v : m->v) {
      const float d = (m->v.co[v] - p).length();
      if (d < bestD) {
        bestD = d;
        best = v;
      }
    }
    return m->v.no[best];
  }

  float maxDz() const
  {
    float r = -1e30f;
    for (int v : m->v) {
      r = std::fmax(r, m->v.co[v][2] - start[v][2]);
    }
    return r;
  }

  int movedCount(float eps = 1e-7f) const
  {
    int n = 0;
    for (int v : m->v) {
      n += (m->v.co[v] - start[v]).length() > eps;
    }
    return n;
  }
};

/** Grids twin of Fixture: a sphere-ish cube cage subdivided to `kLevel`,
 * driven through GridBrushExecutor::applyDab. */
struct GridFixture {
  static constexpr int kLevel = 3;
  Mesh *cage = nullptr;
  sculptcore::subdiv::Multires mr;
  sculptcore::subdiv::GridLevelDomain *d = nullptr;
  Brush brush;
  std::unique_ptr<GridBrushExecutor> exec;
  litestl::util::Vector<float3> start;
  SculptBrushes tool = SculptBrushes::CLAY;

  /** `sphereFac` 1 = the cube's verts pushed onto the sphere. */
  explicit GridFixture(float sphereFac = 1.0f)
  {
    cage = createCube(4, 0.5f, sphereFac);
    mr.init(*cage, kLevel);
    d = mr.gridDomain(kLevel);
    start.resize(d->vertCount());
    for (int v = 0; v < d->vertCount(); v++) {
      start[v] = d->pos()[v];
    }
  }
  ~GridFixture()
  {
    exec.reset();
    litestl::alloc::Delete(cage);
  }

  void
  setBrush(SculptBrushes t, float radius, float strength, float planeoff, float planeSide)
  {
    tool = t;
    brush.radius = radius;
    brush.strength = strength;
    brush.invert = false;
    brush.planeoff = planeoff;
    brush.planeSide = planeSide;
    brush.writeProps();
  }

  void begin(bool nonAccum)
  {
    exec = std::make_unique<GridBrushExecutor>(d, &brush, nullptr);
    exec->nonAccum = nonAccum;
    exec->beginStep();
  }

  void policy(PlaneNormalMode normal,
              PlaneCenterMode center,
              float nrf = 1.0f,
              float arf = 1.0f,
              float3 view = float3(0.0f, 0.0f, 1.0f))
  {
    exec->setPlaneFrame(int(normal),
                        int(center),
                        false,
                        false,
                        nrf,
                        arf,
                        0.0f,
                        0.0f,
                        view[0],
                        view[1],
                        view[2]);
  }

  int dab(float3 center, float3 normal)
  {
    return exec->applyDab(tool, center, normal);
  }

  void end()
  {
    exec->endStep();
  }

  int movedCount(float eps = 1e-7f) const
  {
    int n = 0;
    for (int v = 0; v < d->vertCount(); v++) {
      n += (d->pos()[v] - start[v]).length() > eps;
    }
    return n;
  }

  /** Height of the domain's top along +Z. */
  float topZ() const
  {
    float z = -1e30f;
    for (int v = 0; v < d->vertCount(); v++) {
      z = std::fmax(z, d->pos()[v][2]);
    }
    return z;
  }
};

} // namespace

static void gridCases()
{
  const float3 up(0, 0, 1);

  // (l-a) AREA frame on the sphere-ish cube: radial normal at the top, the
  // centre on the axis and inside, the hit normal ignored.
  {
    GridFixture f;
    const float top = f.topZ();
    f.setBrush(SculptBrushes::FILL, 0.2f, 1.0f, 0.0f, 1.0f);
    f.begin(false);
    f.policy(PlaneNormalMode::Area, PlaneCenterMode::Area);
    f.dab(float3(0, 0, top), float3(0.3f, 0, 1).normalized());
    test_assert(f.exec->lastPlaneResolved);
    const float3 n = f.exec->lastPlaneNormal, c = f.exec->lastPlaneCenter;
    fprintf(stderr,
            "(l-a) top=%.4f n=(%.4f %.4f %.4f) c=(%.4f %.4f %.4f)\n",
            top,
            n[0],
            n[1],
            n[2],
            c[0],
            c[1],
            c[2]);
    test_assert(angleDeg(n, up) < 0.5f);
    test_assert(std::fabs(c[0]) < 1e-3f && std::fabs(c[1]) < 1e-3f);
    test_assert(c[2] < top && c[2] > top - 0.05f);
    f.end();
  }

  // (l-d) data vintage: non-accumulate keeps the stroke-start centre while
  // the clay plane raises the region; accumulate follows it. Grids have no
  // orig-normal stamp, so the non-accum normal may drift by the refresh.
  for (int nonAccum = 0; nonAccum < 2; nonAccum++) {
    GridFixture f;
    const float top = f.topZ();
    f.setBrush(SculptBrushes::CLAY, 0.2f, 1.0f, 0.4f, 1.0f);
    f.begin(nonAccum != 0);
    f.policy(PlaneNormalMode::Area, PlaneCenterMode::Area);
    f.dab(float3(0, 0, top), up);
    const float3 c1 = f.exec->lastPlaneCenter, n1 = f.exec->lastPlaneNormal;
    f.dab(float3(0, 0, top), up);
    f.dab(float3(0, 0, top), up);
    const float3 c3 = f.exec->lastPlaneCenter, n3 = f.exec->lastPlaneNormal;
    fprintf(stderr,
            "(l-d) nonAccum=%d c1.z=%.5f c3.z=%.5f n tilt=%.3f deg moved=%d\n",
            nonAccum,
            c1[2],
            c3[2],
            angleDeg(n1, n3),
            f.movedCount());
    test_assert(f.movedCount() > 0);
    if (nonAccum) {
      // `pos - disp` recovers the base to within an ulp, not bit-exactly.
      fprintf(stderr, "(l-d) |c3-c1|=%g\n", (c3 - c1).length());
      test_assert(near3(c3, c1, 1e-6f));
      test_assert(angleDeg(n1, n3) < 1.0f);
    } else {
      test_assert(c3[2] > c1[2] + 1e-5f);
    }
    f.end();
  }

  // (l-e) fixed axes: the kernel deforms along the requested normal only.
  {
    struct Case {
      PlaneNormalMode mode;
      float3 view;
      float3 expect;
    } cases[] = {
        {PlaneNormalMode::View, float3(0, 1, 0), float3(0, 1, 0)},
        {PlaneNormalMode::X, float3(0, 0, 1), float3(1, 0, 0)},
    };
    for (const Case &cs : cases) {
      GridFixture f;
      const float top = f.topZ();
      f.setBrush(SculptBrushes::CLAY, 0.2f, 1.0f, 0.4f, 1.0f);
      f.begin(false);
      f.policy(cs.mode, PlaneCenterMode::Cursor, 1, 1, cs.view);
      f.dab(float3(0, 0, top), float3(1, 1, 1).normalized());
      test_assert(near3(f.exec->lastPlaneNormal, cs.expect, 1e-6f));
      int moved = 0;
      bool alongAxis = true;
      for (int v = 0; v < f.d->vertCount(); v++) {
        const float3 dv = f.d->pos()[v] - f.start[v];
        if (dv.length() > 1e-7f) {
          moved++;
          const float3 off = dv - cs.expect * dv.dot(cs.expect);
          alongAxis &= off.length() < 1e-6f;
        }
      }
      fprintf(stderr,
              "(l-e) mode=%d moved=%d alongAxis=%d\n",
              int(cs.mode),
              moved,
              int(alongAxis));
      test_assert(moved > 0);
      test_assert(alongAxis);
      f.end();
    }
  }

  // (l-g) mirror image: the reflected primary frame, no gather of its own.
  {
    GridFixture f;
    const float top = f.topZ();
    const float3 B = float3(0.5f, 0, top).normalized() * top;
    const float3 Bm(-B[0], B[1], B[2]);
    f.setBrush(SculptBrushes::FILL, 0.2f, 0.2f, 0.0f, 1.0f);
    f.begin(false);
    f.policy(PlaneNormalMode::Area, PlaneCenterMode::Area);
    f.exec->setImageSign(1, 1, 1, false);
    f.dab(B, B.normalized());
    const float3 nP = f.exec->lastPlaneNormal, cP = f.exec->lastPlaneCenter;
    f.exec->setImageSign(-1, 1, 1, true);
    f.dab(Bm, Bm.normalized());
    const float3 nM = f.exec->lastPlaneNormal, cM = f.exec->lastPlaneCenter;
    test_assert(nM[0] == -nP[0] && nM[1] == nP[1] && nM[2] == nP[2]);
    test_assert(cM[0] == -cP[0] && cM[1] == cP[1] && cM[2] == cP[2]);
    test_assert(near3(f.exec->planeFrameState_.primaryNormal, nP, 0.0f));
    f.end();
  }

  // (l-h) empty gather skips the dab.
  {
    GridFixture f;
    const float top = f.topZ();
    f.setBrush(SculptBrushes::CLAY, 0.2f, 1.0f, 0.4f, 1.0f);
    f.begin(false);
    f.policy(PlaneNormalMode::Area, PlaneCenterMode::Cursor, 1e-5f, 1.0f);
    f.dab(float3(1e-3f, 1e-3f, top), up);
    test_assert(!f.exec->lastPlaneResolved);
    test_assert(f.movedCount() == 0);
    f.end();
  }

  // (l-c) re-gather: a cursor at the edge of the plain cube's top face, with
  // a gather that only sees one side, moves the centre; the leaf set follows
  // it, and the host frame is back afterwards.
  {
    GridFixture f(0.0f);
    f.setBrush(SculptBrushes::CLAY, 0.3f, 1.0f, 0.4f, 1.0f);
    f.begin(false);
    f.policy(PlaneNormalMode::Z, PlaneCenterMode::Area);
    // On the +Z face (z = 0.25 for size 0.5), at its +X edge.
    f.dab(float3(0.25f, 0, 0.25f), up);
    const float3 c = f.exec->lastPlaneCenter;
    float minMovedX = 1e30f;
    for (int v = 0; v < f.d->vertCount(); v++) {
      if (f.d->pos()[v][2] - f.start[v][2] > 1e-6f) {
        minMovedX = std::fmin(minMovedX, f.start[v][0]);
      }
    }
    fprintf(stderr,
            "(l-c) c=(%.4f %.4f %.4f) min moved x=%.4f\n",
            c[0],
            c[1],
            c[2],
            minMovedX);
    test_assert(c[0] < 0.25f - 0.02f);
    test_assert(minMovedX < c[0] - 0.3f + 0.05f);
    test_assert(near3(f.exec->ctx.surfacePos, float3(0.25f, 0, 0.25f), 0.0f));
    f.end();
  }
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  // (k) codegen flag → metadata.
  {
    test_assert(brushDefFlagsFor(SculptBrushes::CLAY).usesPlaneFrame);
    test_assert(brushDefFlagsFor(SculptBrushes::FILL).usesPlaneFrame);
    test_assert(!brushDefFlagsFor(SculptBrushes::DRAW).usesPlaneFrame);
    test_assert(!brushDefFlagsFor(SculptBrushes::SMOOTH).usesPlaneFrame);
  }

  // Default policy: no substitution, the host frame passes straight through.
  {
    Fixture f;
    f.grid(65, 2.0f, [](float, float) { return 0.0f; });
    f.brush(SculptBrushes::CLAY, 0.3f, 1.0f, 0.4f, 1.0f);
    f.begin(false);
    f.dab(float3(0, 0, 0), float3(0, 0, 1));
    test_assert(!f.exec->lastPlaneResolved);
    test_assert(f.maxDz() > 1e-4f); // the dab itself still ran
    f.end();
  }

  // (a) sphere: AREA normal radial, centre on the axis and inside.
  {
    Fixture f;
    f.sphere();
    f.brush(SculptBrushes::FILL, 0.4f, 1.0f, 0.0f, 1.0f);
    f.begin(false);
    f.policy(PlaneNormalMode::Area, PlaneCenterMode::Area);
    // Deliberately wrong hit normal: the policy must ignore it.
    f.dab(float3(0, 0, 1), float3(0.3f, 0, 1).normalized());
    test_assert(f.exec->lastPlaneResolved);
    const float3 n = f.exec->lastPlaneNormal, c = f.exec->lastPlaneCenter;
    fprintf(stderr,
            "(a) n=(%.5f %.5f %.5f) c=(%.5f %.5f %.5f)\n",
            n[0],
            n[1],
            n[2],
            c[0],
            c[1],
            c[2]);
    test_assert(angleDeg(n, float3(0, 0, 1)) < 0.01f);
    test_assert(std::fabs(c[0]) < 1e-4f && std::fabs(c[1]) < 1e-4f);
    test_assert(c[2] < 1.0f && c[2] > 0.9f);
    f.end();
  }

  // (b) bump under the cursor: point normal tilts, AREA normal stays flat,
  // centre height is the region mean (~0), not the hit height.
  {
    Fixture f;
    const float h = 0.06f, sigma = 0.04f;
    f.grid(129, 2.0f, [=](float x, float y) {
      return h * std::exp(-(x * x + y * y) / (2 * sigma * sigma));
    });
    const float3 cursor(sigma, 0, h * std::exp(-0.5f));
    const float3 pn = f.pointNormal(cursor);
    const float tilt = angleDeg(pn, float3(0, 0, 1));
    fprintf(stderr, "(b) point-normal tilt=%.2f deg\n", tilt);
    test_assert(tilt >= 20.0f);
    f.brush(SculptBrushes::SCRAPE, 0.4f, 1.0f, 0.0f, -1.0f);
    f.begin(false);
    f.policy(PlaneNormalMode::Area, PlaneCenterMode::Area);
    f.dab(cursor, pn);
    const float3 n = f.exec->lastPlaneNormal, c = f.exec->lastPlaneCenter;
    fprintf(stderr,
            "(b) area n tilt=%.3f deg  c.z=%.5f (hit z=%.5f)\n",
            angleDeg(n, float3(0, 0, 1)),
            c[2],
            cursor[2]);
    test_assert(angleDeg(n, float3(0, 0, 1)) < 1.0f);
    test_assert(c[2] >= 0.0f && c[2] < 0.2f * h);
    // A flat scrape plane at ~z=0 cuts the bump down.
    float peak = -1e30f;
    for (int v : f.m->v) {
      peak = std::fmax(peak, f.m->v.co[v][2]);
    }
    test_assert(peak < h * 0.9f);
    f.end();
  }

  // (c) centre = AREA at the grid edge: the centre is pulled inward, and the
  // region is re-gathered around it — verts beyond the cursor's own filter
  // radius are lifted by the clay plane.
  {
    Fixture f;
    f.grid(129, 2.0f, [](float, float) { return 0.0f; }, /*leafLimit=*/16);
    const float radius = 0.5f;
    const float3 cursor(1.0f, 0, 0);
    f.brush(SculptBrushes::CLAY, radius, 1.0f, 0.4f, 1.0f);
    f.begin(false);
    f.policy(PlaneNormalMode::Area, PlaneCenterMode::Area);
    f.dab(cursor, float3(0, 0, 1));
    const float3 c = f.exec->lastPlaneCenter;
    float minMovedX = 1e30f;
    for (int v : f.m->v) {
      if (f.m->v.co[v][2] - f.start[v][2] > 1e-6f) {
        minMovedX = std::fmin(minMovedX, f.start[v][0]);
      }
    }
    fprintf(stderr,
            "(c) c.x=%.4f (cursor %.4f)  min moved x=%.4f  filter edge=%.4f\n",
            c[0],
            cursor[0],
            minMovedX,
            cursor[0] - radius);
    test_assert(c[0] < cursor[0] - 0.05f);
    // Inside the resolved falloff...
    test_assert(minMovedX < c[0] - radius + 0.05f);
    // ...and beyond what the cursor-centred filter could reach.
    test_assert(minMovedX < cursor[0] - radius - 0.05f);
    f.end();
  }

  // (d) data vintage: non-accumulate keeps the stroke-start frame across dabs
  // that raise the surface; accumulate follows the raised surface.
  for (int nonAccum = 0; nonAccum < 2; nonAccum++) {
    Fixture f;
    f.grid(65, 2.0f, [](float, float) { return 0.0f; });
    f.brush(SculptBrushes::CLAY, 0.3f, 1.0f, 0.4f, 1.0f);
    f.begin(nonAccum != 0);
    f.policy(PlaneNormalMode::Area, PlaneCenterMode::Area);
    f.dab(float3(0, 0, 0), float3(0, 0, 1));
    const float3 c1 = f.exec->lastPlaneCenter, n1 = f.exec->lastPlaneNormal;
    const float dz1 = f.maxDz();
    f.dab(float3(0, 0, 0), float3(0, 0, 1));
    f.dab(float3(0, 0, 0), float3(0, 0, 1));
    const float3 c3 = f.exec->lastPlaneCenter, n3 = f.exec->lastPlaneNormal;
    fprintf(stderr,
            "(d) nonAccum=%d c1.z=%.6f c3.z=%.6f dz1=%.5f dz3=%.5f\n",
            nonAccum,
            c1[2],
            c3[2],
            dz1,
            f.maxDz());
    test_assert(dz1 > 1e-4f);
    if (nonAccum) {
      test_assert(c3[2] == c1[2] && c3[0] == c1[0] && c3[1] == c1[1]);
      test_assert(n3[0] == n1[0] && n3[1] == n1[1] && n3[2] == n1[2]);
      test_assert(f.maxDz() > dz1 * 1.5f); // still additive
    } else {
      test_assert(c3[2] > c1[2] + 1e-5f);
    }
    f.end();
  }

  // (d') the same on a bump the stroke scrapes flat: non-accumulate keeps the
  // stroke-start normals (`.brush.orig.no` is stamped), accumulate reads the
  // flattened surface and its AREA normal changes. The gather takes normals
  // at the host's cadence (a frame's updateNormals between dabs here).
  for (int nonAccum = 0; nonAccum < 2; nonAccum++) {
    Fixture f;
    const float h = 0.06f, sigma = 0.04f;
    f.grid(129, 2.0f, [=](float x, float y) {
      return h * std::exp(-(x * x + y * y) / (2 * sigma * sigma));
    });
    // Off-centre gather so the bump tilts the area normal measurably.
    const float3 cursor(2.5f * sigma, 0, 0);
    f.brush(SculptBrushes::SCRAPE, 0.12f, 1.0f, 0.0f, -1.0f);
    f.begin(nonAccum != 0);
    f.policy(PlaneNormalMode::Area, PlaneCenterMode::Area);
    f.dab(cursor, float3(0, 0, 1));
    const float3 n1 = f.exec->lastPlaneNormal, c1 = f.exec->lastPlaneCenter;
    for (int i = 0; i < 4; i++) {
      f.scene.tree->updateNormals();
      f.dab(cursor, float3(0, 0, 1));
    }
    const float3 n5 = f.exec->lastPlaneNormal, c5 = f.exec->lastPlaneCenter;
    const bool stamped = f.m->v.attrs.has(AttrType::FLOAT3, ".brush.orig.no");
    fprintf(stderr,
            "(d') nonAccum=%d tilt1=%.3f tilt5=%.3f c1.z=%.5f c5.z=%.5f orig.no=%d\n",
            nonAccum,
            angleDeg(n1, float3(0, 0, 1)),
            angleDeg(n5, float3(0, 0, 1)),
            c1[2],
            c5[2],
            int(stamped));
    test_assert(angleDeg(n1, float3(0, 0, 1)) > 0.5f); // the bump is felt
    if (nonAccum) {
      test_assert(stamped);
      test_assert(near3(n5, n1, 0.0f));
      test_assert(near3(c5, c1, 0.0f));
    } else {
      test_assert(!stamped);
      test_assert(angleDeg(n5, n1) > 0.05f);
      test_assert(c5[2] < c1[2]);
    }
    f.end();
  }

  // (e) fixed normals: VIEW / X / Y / Z, and the kernel deforms along them.
  {
    struct Case {
      PlaneNormalMode mode;
      float3 view;
      float3 expect;
    } cases[] = {
        {PlaneNormalMode::View, float3(0, 1, 0), float3(0, 1, 0)},
        {PlaneNormalMode::X, float3(0, 0, 1), float3(1, 0, 0)},
        {PlaneNormalMode::Y, float3(0, 0, 1), float3(0, 1, 0)},
        {PlaneNormalMode::Z, float3(0, 0, 1), float3(0, 0, 1)},
    };
    for (const Case &cs : cases) {
      Fixture f;
      f.grid(65, 2.0f, [](float, float) { return 0.0f; });
      f.brush(SculptBrushes::CLAY, 0.3f, 1.0f, 0.4f, 1.0f);
      f.begin(false);
      f.policy(cs.mode, PlaneCenterMode::Cursor, false, false, 1, 1, 0, 0, cs.view);
      // The hit normal is wrong on purpose.
      f.dab(float3(0, 0, 0), float3(1, 1, 1).normalized());
      test_assert(near3(f.exec->lastPlaneNormal, cs.expect, 1e-6f));
      test_assert(near3(f.exec->lastPlaneCenter, float3(0, 0, 0), 1e-6f));
      int moved = 0;
      bool alongAxis = true;
      for (int v : f.m->v) {
        const float3 d = f.m->v.co[v] - f.start[v];
        if (d.length() > 1e-7f) {
          moved++;
          const float3 off = d - cs.expect * d.dot(cs.expect);
          alongAxis &= off.length() < 1e-6f;
        }
      }
      fprintf(stderr,
              "(e) mode=%d moved=%d alongAxis=%d\n",
              int(cs.mode),
              moved,
              int(alongAxis));
      test_assert(moved > 0);
      test_assert(alongAxis);
      f.end();
    }
  }

  // (f) original-normal / original-plane: the held quantity stays at the
  // first primary dab's value while the other keeps updating.
  {
    const float3 A(0, 0, 1);
    const float3 B(std::sin(40 * kDeg), 0, std::cos(40 * kDeg));
    for (int which = 0; which < 2; which++) {
      Fixture f;
      f.sphere();
      f.brush(SculptBrushes::FILL, 0.3f, 0.2f, 0.0f, 1.0f);
      f.begin(false);
      f.policy(PlaneNormalMode::Area, PlaneCenterMode::Area, which == 0, which == 1);
      f.dab(A, A);
      const float3 nA = f.exec->lastPlaneNormal, cA = f.exec->lastPlaneCenter;
      f.dab(B, B);
      const float3 nB = f.exec->lastPlaneNormal, cB = f.exec->lastPlaneCenter;
      fprintf(stderr,
              "(f) hold=%s  nB·A=%.4f nB·B=%.4f  |cB-cA|=%.4f\n",
              which == 0 ? "normal" : "plane",
              nB.dot(A),
              nB.dot(B),
              (cB - cA).length());
      if (which == 0) {
        test_assert(near3(nB, nA, 0.0f));       // normal held
        test_assert((cB - cA).length() > 0.1f); // centre moved to B
        test_assert(angleDeg(cB, B) < 2.0f);
      } else {
        test_assert(near3(cB, cA, 0.0f)); // centre held
        // The normal followed B. A UV sphere's rings crowd toward the pole, so
        // the vertex-normal average leans a couple of degrees poleward of the
        // exact radial direction; that is the gather, not the policy.
        test_assert(angleDeg(nB, B) < 3.0f);
      }
      f.end();
    }
  }

  // (g) mirror image: the reflected primary frame, even where the mirror
  // side's own gather would disagree (the -X half is squashed).
  {
    const float3 B(std::sin(40 * kDeg), 0, std::cos(40 * kDeg));
    const float3 Bm(-B[0], B[1], B[2]);
    auto squash = [](float3 co) {
      if (co[0] < 0) {
        co[0] *= 0.6f;
        co[2] *= 0.8f;
      }
      return co;
    };
    Fixture f;
    f.sphere(squash);
    f.brush(SculptBrushes::FILL, 0.3f, 0.2f, 0.0f, 1.0f);
    f.begin(false);
    f.policy(PlaneNormalMode::Area, PlaneCenterMode::Area);
    f.exec->setImageSign(1, 1, 1, false);
    f.dab(B, B);
    const float3 nP = f.exec->lastPlaneNormal, cP = f.exec->lastPlaneCenter;
    f.exec->setImageSign(-1, 1, 1, true);
    f.dab(Bm, Bm);
    const float3 nM = f.exec->lastPlaneNormal, cM = f.exec->lastPlaneCenter;
    test_assert(nM[0] == -nP[0] && nM[1] == nP[1] && nM[2] == nP[2]);
    test_assert(cM[0] == -cP[0] && cM[1] == cP[1] && cM[2] == cP[2]);
    // The mirror did not overwrite the primary memory.
    test_assert(near3(f.exec->planeFrameState_.primaryNormal, nP, 0.0f));
    f.end();

    // What the squashed side's own gather says, for contrast.
    Fixture g;
    g.sphere(squash);
    g.brush(SculptBrushes::FILL, 0.3f, 0.2f, 0.0f, 1.0f);
    g.begin(false);
    g.policy(PlaneNormalMode::Area, PlaneCenterMode::Area);
    g.dab(Bm, Bm);
    fprintf(stderr,
            "(g) mirror n=(%.4f %.4f %.4f) own-gather n=(%.4f %.4f %.4f)\n",
            nM[0],
            nM[1],
            nM[2],
            g.exec->lastPlaneNormal[0],
            g.exec->lastPlaneNormal[1],
            g.exec->lastPlaneNormal[2]);
    test_assert(angleDeg(nM, g.exec->lastPlaneNormal) > 1.0f);
    g.end();
  }

  // (h) empty gather: a normal radius that reaches no vertex → degenerate
  // normal → the dab is skipped and nothing moves.
  {
    Fixture f;
    f.grid(65, 2.0f, [](float, float) { return 0.0f; });
    f.brush(SculptBrushes::CLAY, 0.3f, 1.0f, 0.4f, 1.0f);
    f.begin(false);
    f.policy(PlaneNormalMode::Area, PlaneCenterMode::Cursor, false, false, 0.001f, 1.0f);
    // Between grid verts (spacing 1/32).
    f.dab(float3(1.0f / 64.0f, 1.0f / 64.0f, 0), float3(0, 0, 1));
    test_assert(!f.exec->lastPlaneResolved);
    test_assert(f.movedCount() == 0);
    f.end();
  }

  // (i) stabiliser.
  {
    // 0/0: exact centre, normal within float normalisation noise.
    PlaneStabilizer st;
    const float3 n0 = float3(0.3f, -0.2f, 0.9f).normalized();
    const float3 c0(0.25f, 0.5f, -0.75f);
    float3 on, oc;
    st.apply(0, 0, n0, c0, on, oc);
    test_assert(oc[0] == c0[0] && oc[1] == c0[1] && oc[2] == c0[2]);
    test_assert(near3(on, n0, 1e-6f));
    const float3 n1 = float3(-0.4f, 0.1f, 0.8f).normalized();
    const float3 c1(0.1f, 0.2f, 0.3f);
    st.apply(0, 0, n1, c1, on, oc);
    test_assert(oc[0] == c1[0] && oc[1] == c1[1] && oc[2] == c1[2]);
    test_assert(near3(on, n1, 1e-6f));

    // Weights 0.5/0.5, cursor alternating between A and B on a sphere: the
    // stabilised normal blends the two raw area normals and stays unit length.
    const float3 A(0, 0, 1);
    const float3 B(std::sin(30 * kDeg), 0, std::cos(30 * kDeg));
    Fixture raw;
    raw.sphere();
    raw.brush(SculptBrushes::FILL, 0.3f, 0.0f, 0.0f, 1.0f);
    raw.begin(false);
    raw.policy(PlaneNormalMode::Area, PlaneCenterMode::Area);
    raw.dab(A, A);
    const float3 nA = raw.exec->lastPlaneNormal;
    raw.dab(B, B);
    const float3 nB = raw.exec->lastPlaneNormal;
    raw.end();

    Fixture f;
    f.sphere();
    f.brush(SculptBrushes::FILL, 0.3f, 0.0f, 0.0f, 1.0f);
    f.begin(false);
    f.policy(
        PlaneNormalMode::Area, PlaneCenterMode::Area, false, false, 1, 1, 0.5f, 0.5f);
    float3 last;
    for (int i = 0; i < 8; i++) {
      const float3 p = (i & 1) ? B : A;
      f.dab(p, p);
      last = f.exec->lastPlaneNormal;
      test_assert(std::fabs(last.length() - 1.0f) < 1e-5f);
    }
    fprintf(stderr,
            "(i) nA·last=%.4f nB·last=%.4f (nA·nB=%.4f)\n",
            nA.dot(last),
            nB.dot(last),
            nA.dot(nB));
    // The last dab was at B (i=7): the stabilised normal must lie strictly
    // between the two raw normals.
    test_assert(angleDeg(last, nB) > 1.0f);
    test_assert(angleDeg(last, nA) > 1.0f);
    test_assert(angleDeg(last, nA) + angleDeg(last, nB) < angleDeg(nA, nB) + 0.5f);
    f.end();
  }

  // (j) programs restore the cursor frame between sub-commands, on both the
  // raw (execProgram) and the resolved (applyResolvedProgram) path.
  for (int resolved = 0; resolved < 2; resolved++) {
    Fixture f;
    f.grid(129, 2.0f, [](float, float) { return 0.0f; }, /*leafLimit=*/16);
    const float3 cursor(1.0f, 0, 0);
    f.brush(SculptBrushes::CLAY, 0.5f, 1.0f, 0.4f, 1.0f);
    f.begin(false);
    f.policy(PlaneNormalMode::Area, PlaneCenterMode::Area);
    BrushProgram prog;
    prog.addCommand(int(SculptBrushes::CLAY));
    prog.addCommand(int(SculptBrushes::CLAY));
    if (resolved) {
      auto r = f.exec->applyResolvedProgram(&prog, cursor, float3(0, 0, 1));
      test_assert(r.error == sculptcore::props::PropError::ERROR_NONE);
    } else {
      f.exec->applyDab(&prog, cursor, float3(0, 0, 1), 0.5f, nullptr, 0);
    }
    fprintf(stderr,
            "(j) resolved=%d second-stage cursor=(%.4f %.4f %.4f) ctx=(%.4f %.4f %.4f)\n",
            resolved,
            f.exec->lastPlaneCursor[0],
            f.exec->lastPlaneCursor[1],
            f.exec->lastPlaneCursor[2],
            f.exec->ctx.surfacePos[0],
            f.exec->ctx.surfacePos[1],
            f.exec->ctx.surfacePos[2]);
    // The second CLAY resolved from the original cursor, not from the first
    // stage's area centre...
    test_assert(f.exec->lastPlaneResolved);
    test_assert(near3(f.exec->lastPlaneCursor, cursor, 0.0f));
    // ...and the ctx frame is back to the host's after the program.
    test_assert(near3(f.exec->ctx.surfacePos, cursor, 0.0f));
    test_assert(near3(f.exec->ctx.surfaceNo, float3(0, 0, 1), 0.0f));
    f.end();

    // [CLAY, SMOOTH]: the plane stage does not leak into the smooth stage.
    Fixture g;
    g.grid(65, 2.0f, [](float, float) { return 0.0f; });
    g.brush(SculptBrushes::CLAY, 0.3f, 1.0f, 0.4f, 1.0f);
    g.begin(false);
    g.policy(PlaneNormalMode::Area, PlaneCenterMode::Area);
    BrushProgram prog2;
    prog2.addCommand(int(SculptBrushes::CLAY));
    prog2.addCommand(int(SculptBrushes::BSMOOTH));
    if (resolved) {
      auto r = g.exec->applyResolvedProgram(&prog2, float3(0, 0, 0), float3(0, 0, 1));
      test_assert(r.error == sculptcore::props::PropError::ERROR_NONE);
    } else {
      g.exec->applyDab(&prog2, float3(0, 0, 0), float3(0, 0, 1), 0.3f, nullptr, 0);
    }
    test_assert(!g.exec->lastPlaneResolved); // the smooth stage ran last, untouched
    test_assert(near3(g.exec->ctx.surfacePos, float3(0, 0, 0), 0.0f));
    g.end();
  }

  gridCases();

  return test_end();
}
