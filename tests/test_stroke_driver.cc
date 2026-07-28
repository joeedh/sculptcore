#include "test_util.h"

#include "brush/stroke_driver.h"
#include "litestl/math/vector.h"
#include "mesh/mesh.h"
#include "spatial/spatial.h"
#include "spatial/spatial_base.h"

#include <cmath>
#include <cstdio>

test_init;

// Local assert that flips retval (the shared test_assert macro has a known
// retval=0-on-failure bug — see tests/test_meshlog_topo.cc:14-21).
#define TASSERT(expr)                                                                    \
  do {                                                                                   \
    if (!(expr)) {                                                                       \
      retval = 1;                                                                        \
      fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                  \
      fflush(stderr);                                                                    \
    }                                                                                    \
  } while (0)

using namespace sculptcore;
using namespace sculptcore::brush;
using litestl::math::float3;

namespace {

constexpr double GL_W = 800.0;
constexpr double GL_H = 600.0;

/**
 * Identity render matrix + a camera at -Z, so a pixel's view ray is
 * `normalize(unproject(px) - camPos)` and the z=0 plane sits ~10 units ahead.
 */
void setupView(BrushStrokeDriver &d, bool hasObjectMatrix = false)
{
  for (int row = 0; row < 4; row++) {
    for (int mat = 0; mat < 2; mat++) {
      d.setViewRow(mat,
                   row,
                   row == 0 ? 1.0f : 0.0f,
                   row == 1 ? 1.0f : 0.0f,
                   row == 2 ? 1.0f : 0.0f,
                   row == 3 ? 1.0f : 0.0f);
    }
  }
  d.setViewParams(0.0f,
                  0.0f,
                  -10.0f,
                  float(GL_W),
                  float(GL_H),
                  float(GL_W),
                  float(GL_H),
                  0.1f,
                  hasObjectMatrix);
}

void pushAt(BrushStrokeDriver &d, double x, double y, double radius, double spacing)
{
  d.push(float(x),
         float(y),
         1.0f,
         0.0f,
         0.0f,
         0.0f,
         false,
         false,
         float(radius),
         1.0f,
         float(spacing));
}

double screenDist(const DabSample &a, const DabSample &b)
{
  const double dx = double(a.screenP[0]) - double(b.screenP[0]);
  const double dy = double(a.screenP[1]) - double(b.screenP[1]);
  return std::sqrt(dx * dx + dy * dy);
}

/** NxN grid of quads on z=0 spanning [-size, size] in XY. */
void buildGrid(mesh::Mesh &m, int N, float size)
{
  litestl::util::Vector<int> v;
  v.resize((N + 1) * (N + 1));
  auto vat = [&](int i, int j) -> int & { return v[j * (N + 1) + i]; };

  for (int j = 0; j <= N; j++) {
    for (int i = 0; i <= N; i++) {
      const float x = (float(i) / float(N) * 2.0f - 1.0f) * size;
      const float y = (float(j) / float(N) * 2.0f - 1.0f) * size;
      vat(i, j) = m.make_vertex(float3(x, y, 0.0f));
    }
  }
  for (int j = 0; j < N; j++) {
    for (int i = 0; i < N; i++) {
      const int v0 = vat(i, j), v1 = vat(i + 1, j);
      const int v2 = vat(i + 1, j + 1), v3 = vat(i, j + 1);

      const int ring[4][2] = {{v0, v1}, {v1, v2}, {v2, v3}, {v3, v0}};
      for (auto &e : ring) {
        if (m.find_edge(e[0], e[1]) == ELEM_NONE) {
          m.make_edge(e[0], e[1]);
        }
      }

      int verts[4] = {v0, v1, v2, v3};
      m.make_face(std::span<int>(verts, 4));
    }
  }
}

} // namespace

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  // PATH + SCREEN: evenly spaced dabs along a straight drag, spacing held
  // across every segment boundary (i.e. walkCarry carries correctly), plus the
  // first dab being the raw, non-interpolated one.
  {
    BrushStrokeDriver d;
    setupView(d);

    const double radius = 20.0, spacing = 0.5;
    const double step = 20.0; // spacing * 2 * radius

    for (int i = 0; i < 5; i++) {
      pushAt(d, 100.0 + 100.0 * i, 300.0, radius, spacing);
    }
    d.end();

    const int n = d.poll();
    TASSERT(d.finished());
    // 400 px of path at 20 px/dab, plus the raw dab at the start.
    TASSERT(n == 21);

    if (n == 21) {
      const DabSample *first = d.sampleAt(0);
      TASSERT(!first->isInterp);
      TASSERT(!first->hasCurve);
      TASSERT(std::fabs(double(first->screenP[0]) - 100.0) < 1e-3);

      double maxErr = 0.0;
      for (int i = 1; i < n; i++) {
        const DabSample *a = d.sampleAt(i - 1);
        const DabSample *b = d.sampleAt(i);
        TASSERT(b->isInterp);
        TASSERT(b->hasCurve);
        maxErr = std::max(maxErr, std::fabs(screenDist(*a, *b) - step));
      }
      TASSERT(maxErr < 0.05);

      // The walk runs the full path: last dab lands on the final control point.
      TASSERT(std::fabs(double(d.sampleAt(n - 1)->screenP[0]) - 500.0) < 0.05);
      // strokeS accumulates one `spacing` per interpolated dab.
      TASSERT(std::fabs(double(d.sampleAt(n - 1)->strokeS) - spacing * 20.0) < 1e-3);
    }
  }

  // Batched polls must produce the same cadence as one big poll — the carry is
  // driver state, not per-poll state.
  {
    BrushStrokeDriver d;
    setupView(d);

    litestl::util::Vector<double> xs;
    for (int i = 0; i < 5; i++) {
      pushAt(d, 100.0 + 100.0 * i, 300.0, 20.0, 0.5);
      const int n = d.poll();
      for (int k = 0; k < n; k++) {
        xs.append(double(d.sampleAt(k)->screenP[0]));
      }
    }
    d.end();
    const int n = d.poll();
    for (int k = 0; k < n; k++) {
      xs.append(double(d.sampleAt(k)->screenP[0]));
    }

    TASSERT(xs.size() == 21);
    double maxErr = 0.0;
    for (int i = 1; i < int(xs.size()); i++) {
      maxErr = std::max(maxErr, std::fabs(xs[i] - xs[i - 1] - 20.0));
    }
    TASSERT(maxErr < 0.05);
  }

  // DRAG_DOT: one raw dab per input, cursor tracked as the preview point.
  {
    BrushStrokeDriver d;
    d.strokeMethod = StrokeMethod::DragDot;
    setupView(d);

    pushAt(d, 100.0, 300.0, 20.0, 0.5);
    pushAt(d, 140.0, 300.0, 20.0, 0.5);
    pushAt(d, 180.0, 300.0, 20.0, 0.5);
    d.end();

    const int n = d.poll();
    TASSERT(n == 3);
    if (n == 3) {
      for (int i = 0; i < 3; i++) {
        TASSERT(!d.sampleAt(i)->isInterp);
        TASSERT(!d.sampleAt(i)->hasCurve);
      }
      TASSERT(std::fabs(double(d.sampleAt(2)->screenP[0]) - 180.0) < 1e-3);
    }
    TASSERT(d.hasPreviewScreen());
    TASSERT(std::fabs(double(d.previewScreenX()) - 180.0) < 1e-3);
  }

  // ANCHORED can't start over empty space, and WORLD mode has no plane to
  // project onto before the first hit — both discard every input.
  {
    BrushStrokeDriver d;
    d.strokeMethod = StrokeMethod::Anchored;
    setupView(d);

    pushAt(d, 400.0, 300.0, 20.0, 0.5);
    pushAt(d, 500.0, 300.0, 20.0, 0.5);
    d.end();

    TASSERT(d.poll() == 0);
    TASSERT(!d.hasAnchorScreen());
  }
  {
    BrushStrokeDriver d;
    d.spaceMode = StrokeSpaceMode::World;
    setupView(d);

    for (int i = 0; i < 4; i++) {
      pushAt(d, 300.0 + 40.0 * i, 300.0, 20.0, 0.5);
    }
    d.end();

    TASSERT(d.poll() == 0);
  }

  // With real geometry under the cursor: ANCHORED pins the dab origin at the
  // first hit and drives radius from the drag distance.
  {
    mesh::Mesh m;
    buildGrid(m, 8, 2.0f);
    spatial::SpatialTree tree(&m);
    tree.buildAll();
    for (auto *node : tree.leaves()) {
      tree.ensure_node_tris(node);
    }

    BrushStrokeDriver d(&tree);
    d.strokeMethod = StrokeMethod::Anchored;
    setupView(d);

    pushAt(d, 410.0, 290.0, 20.0, 0.5);
    pushAt(d, 510.0, 290.0, 20.0, 0.5);
    d.end();

    const int n = d.poll();
    TASSERT(n == 2);
    TASSERT(d.hasAnchorScreen());
    TASSERT(std::fabs(double(d.anchorScreenX()) - 410.0) < 1e-3);

    if (n == 2) {
      const DabSample *a = d.sampleAt(0);
      const DabSample *b = d.sampleAt(1);
      TASSERT(!a->isInterp && !b->isInterp);
      // Both dabs stay pinned to the anchor's screen + world position.
      TASSERT(std::fabs(double(b->screenP[0]) - 410.0) < 1e-3);
      for (int k = 0; k < 3; k++) {
        TASSERT(std::fabs(double(a->p[k]) - double(b->p[k])) < 1e-5);
      }
      // The grid lies on z=0, so the hit does too.
      TASSERT(std::fabs(double(a->p[2])) < 1e-4);
      // Drag of 100 px with a screen-space radius => radius is that drag.
      TASSERT(std::fabs(double(a->radius) - 20.0) < 1e-3);
      TASSERT(std::fabs(double(b->radius) - 100.0) < 1e-3);
      // anchorVec points along +X, the direction of the drag.
      TASSERT(b->anchorVec[0] > 0.1f);
      TASSERT(std::fabs(b->anchorVec[1]) < 1e-3f);
    }
  }

  // WORLD mode over real geometry: spacing is measured in world units.
  {
    mesh::Mesh m;
    buildGrid(m, 8, 2.0f);
    spatial::SpatialTree tree(&m);
    tree.buildAll();
    for (auto *node : tree.leaves()) {
      tree.ensure_node_tris(node);
    }

    BrushStrokeDriver d(&tree);
    d.spaceMode = StrokeSpaceMode::World;
    d.radiusIsWorld = true;
    setupView(d);

    const double radius = 0.1, spacing = 0.5;
    const double step = spacing * 2.0 * radius;

    for (int i = 0; i < 5; i++) {
      pushAt(d, 300.0 + 40.0 * i, 290.0, radius, spacing);
    }
    d.end();

    const int n = d.poll();
    TASSERT(n > 4);

    double maxErr = 0.0;
    for (int i = 2; i < n; i++) {
      const DabSample *a = d.sampleAt(i - 1);
      const DabSample *b = d.sampleAt(i);
      const double dx = double(b->p[0]) - double(a->p[0]);
      const double dy = double(b->p[1]) - double(a->p[1]);
      const double dz = double(b->p[2]) - double(a->p[2]);
      maxErr = std::max(maxErr, std::fabs(std::sqrt(dx * dx + dy * dy + dz * dz) - step));
    }
    TASSERT(maxErr < step * 0.02);
  }

  return test_end();
}
