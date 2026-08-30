#include "test_util.h"

#include "brush/brush.h"
#include "brush/brush_executor.h"
#include "brush/texture_eval.h"
#include "brush/texture_jit.h"
#include "brush/texture_program.h"
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
using litestl::util::Vector;

/** Brush::texture_program integration (texture-scripts T3.4): the bind /
 * param-edit surface on Brush, and the stroke gate — a DRAW stroke with the
 * JIT'd Rings program must match a TEXDRAW stroke (whose kernel inlines the
 * clang-precompiled Rings) at radius 2, where draw's `* radius * 0.5` factor
 * is 1.0 and the two differ only by multiply order and tcc-vs-clang codegen. */

// Verbatim kernels/rings.stex — the unit texdraw.sbrush imports.
static const char *kRingsSrc = R"(
texture Rings {
  float eval(float3 p, float3 n) {
    float d = length(p);
    float rings = 0.5 + 0.5 * sin(d * 40.0);
    float bands = fract(d * 6.0);
    float steps = floor(bands * 4.0) * 0.25;
    float tilt = 0.5 + 0.5 * cos(dot(n, p) * 8.0);
    return rings * steps * tilt;
  }
}
)";

static const char *kScaledSrc = R"(
texture Scaled {
  param float scale = 2.0 @range(0.5, 8.0);
  param ramp shape;

  float eval(float3 p, float3 n) {
    float3 q = mapPoint(p);
    return shape.sample(fract(length(q) * scale));
  }
}
)";

namespace {

/** NxN grid of quads on z=0 spanning [-size, size] in XY (the
 * test_stroke_driver.cc grid). */
void buildGrid(mesh::Mesh &m, int N, float size)
{
  Vector<int> v;
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

/** One 5-dab +X drag across the grid (the meshStroke shape from
 * test_grid_stroke.cc, minus multires). */
void stroke(mesh::Mesh &m, Brush &b, SculptBrushes tool)
{
  spatial::SpatialTree tree(&m);
  tree.buildAll();
  for (auto *node : tree.leaves()) {
    tree.ensure_node_tris(node);
  }

  CommandExecutor ex(&tree, &b);
  ex.setStrokeGen(1);
  ex.beginStep(false);
  const float3 no(0.0f, 0.0f, 1.0f);
  for (int i = 0; i < 5; i++) {
    const float3 origin(-0.6f + 0.3f * float(i), 0.05f, 0.0f);
    Vector<spatial::SpatialNode *> nodes;
    tree.filterNodes(origin, b.radius, nodes);
    ex.execBrush(&m, tool, &nodes, origin, no);
    tree.updateQueries();
    tree.updateNormals();
  }
  ex.endStep();
}

void testBindSurface()
{
  Brush b;
  TASSERT(b.texture_program == nullptr);

  // Failed compile: unbound, error captured.
  TASSERT(!b.setTextureScriptSource("texture Broken { float eval(", "broken.stex"));
  TASSERT(b.texture_program == nullptr);
  TASSERT(b.texture_script_error.size() > 0);

  // Successful bind clears the error and seeds the live slab from defaults.
  TASSERT(b.setTextureScriptSource(kScaledSrc, "scaled.stex"));
  TASSERT(b.texture_program != nullptr);
  TASSERT(b.texture_script_error.size() == 0);
  TASSERT(b.textureParamCount() == 2);
  TASSERT((int)b.texture_params.size() == b.texture_program->paramSlabSize);
  TASSERT(b.texture_params[0] == 2.0f);
  TASSERT(b.textureParamIndex("scale") == 0);
  TASSERT(b.textureParamIndex("shape") == 1);
  TASSERT(b.textureParamIndex("nope") == -1);

  TextureProgramParam *p0 = b.queriedTextureParamEntry(0);
  TASSERT(p0 && !p0->isRamp && p0->hasRange);
  TASSERT(b.queriedTextureParamEntry(2) == nullptr);
  TASSERT(b.queriedTextureParamEntry(-1) == nullptr);

  // Scalar setter clamps to @range; refuses the ramp param.
  TASSERT(b.setTextureParamAt(0, 100.0f));
  TASSERT(b.texture_params[0] == 8.0f);
  TASSERT(b.setTextureParamAt(0, 0.75f));
  TASSERT(b.texture_params[0] == 0.75f);
  TASSERT(!b.setTextureParamAt(1, 1.0f));

  // Ramp setter wants exactly kTexRampSize samples; refuses the scalar.
  Vector<float> lut;
  lut.resize(kTexRampSize);
  for (int i = 0; i < kTexRampSize; i++) {
    lut[i] = float(i) / float(kTexRampSize - 1);
  }
  TASSERT(b.setTextureRampAt(1, lut));
  TASSERT(b.texture_params[1 + kTexRampSize / 2] > 0.4f);
  TASSERT(!b.setTextureRampAt(0, lut));
  Vector<float> shortLut;
  shortLut.resize(8);
  TASSERT(!b.setTextureRampAt(1, shortLut));

  b.clearTextureScript();
  TASSERT(b.texture_program == nullptr);
  TASSERT(b.texture_params.size() == 0);

  // Rebinding replaces a live program.
  TASSERT(b.setTextureScriptSource(kScaledSrc, "scaled.stex"));
  TASSERT(b.setTextureScriptSource(kRingsSrc, "rings.stex"));
  TASSERT(b.textureParamCount() == 0);
}

void testStrokeAB()
{
  mesh::Mesh mA, mB;
  buildGrid(mA, 24, 2.0f);
  buildGrid(mB, 24, 2.0f);

  Brush bA, bB;
  for (Brush *b : {&bA, &bB}) {
    b->radius = 2.0f;
    b->strength = 0.35f;
    b->writeProps();
  }
  TASSERT(bB.setTextureScriptSource(kRingsSrc, "rings.stex"));

  stroke(mA, bA, SculptBrushes::TEXDRAW);
  stroke(mB, bB, SculptBrushes::DRAW);

  TASSERT(mA.v.count == mB.v.count);
  double maxDisp = 0.0, maxErr = 0.0;
  for (int v = 0; v < mA.v.count; v++) {
    maxDisp = std::max(maxDisp, std::fabs(double(mA.v.co[v][2])));
    for (int k = 0; k < 3; k++) {
      maxErr = std::max(maxErr, std::fabs(double(mA.v.co[v][k]) - double(mB.v.co[v][k])));
    }
  }
  // The texture must actually have displaced the plane...
  TASSERT(maxDisp > 0.05);
  // ...and the JIT'd program must track the precompiled kernel to fp noise.
  if (maxErr > 1e-5) {
    fprintf(stderr, "texdraw-vs-program maxErr = %g\n", maxErr);
  }
  TASSERT(maxErr <= 1e-5);
}

} // namespace

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  TASSERT(sculptcore::brush::textureScriptCpuAvailable());
  testBindSurface();
  testStrokeAB();
  return test_end();
}
