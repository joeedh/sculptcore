// The generated GPU appended-uniform marshal (grids-native SW1).
//
// packBrushUniforms used to hand-pack each kernel's appended DSL uniforms in a
// per-tool switch, and twice a declared uniform simply never got a case: the
// color kernel's `mixMode` (every GPU color stroke blended as MIX) and
// bsmooth's `projection` (the GPU kernel read the old union slot's 1.0
// default). Both packs are now emitted by sbrushc from the kernel's own
// uniform declarations, at the offsets its WGSL BrushUniforms block implies —
// this test grades that emission byte-for-byte against an independent
// transcription of the layout, including the two once-missing fields and the
// @range clamps that replaced applyGpuHostClamps.
#include "test_util.h"

#include "brush/brush.h"
#include "brush/brushes/all.h"
#include "brush/compute_layout.h"
#include "brush/gpu_marshal.h"

#include <cstdio>
#include <cstring>

test_init;

using namespace sculptcore::brush;
using litestl::math::float4;

static float readF(const ComputeBrushUniforms &bu, int off)
{
  float v;
  std::memcpy(&v, reinterpret_cast<const unsigned char *>(&bu) + off, 4);
  return v;
}

static int32_t readI(const ComputeBrushUniforms &bu, int off)
{
  int32_t v;
  std::memcpy(&v, reinterpret_cast<const unsigned char *>(&bu) + off, 4);
  return v;
}

static void pack(Brush &brush, SculptBrushes tool, ComputeBrushUniforms &bu)
{
  packBrushUniforms(brush, tool, false, bu);
}

static void runTests()
{
  ComputeBrushUniforms bu;

  /* bsmooth `projection` @72 — the field the hand switch never packed. */
  {
    Brush brush;
    brush.projection = 0.7f;
    pack(brush, SculptBrushes::BSMOOTH, bu);
    test_assert(readF(bu, 72) == 0.7f);
  }

  /* color `brushColor` vec4 @80 (16-aligned past the scalar slots) and
   * `mixMode` i32 @96 — the SW0 regression, regraded through the generator. */
  {
    Brush brush;
    brush.brushColor = float4(0.125f, 0.25f, 0.5f, 1.0f);
    brush.mixMode = 3;
    pack(brush, SculptBrushes::COLOR, bu);
    test_assert(readF(bu, 80) == 0.125f);
    test_assert(readF(bu, 84) == 0.25f);
    test_assert(readF(bu, 88) == 0.5f);
    test_assert(readF(bu, 92) == 1.0f);
    test_assert(readI(bu, 96) == 3);
    /* The scalar slots are pad in color's WGSL view — stay zero. */
    test_assert(readF(bu, 72) == 0.0f);
  }

  /* kelvinlet mu/nu @72/76 with the @range clamps that replaced
   * applyGpuHostClamps, plus the @unbounded prelude slot @44. */
  {
    Brush brush;
    brush.mu = 0.0f;
    brush.nu = 0.9f;
    brush.unboundedExtent = 6.5f;
    pack(brush, SculptBrushes::KELVINLET, bu);
    test_assert(readF(bu, 72) == 1e-6f);
    test_assert(readF(bu, 76) == 0.499f);
    test_assert(readF(bu, 44) == 6.5f);
    /* The clamp is marshal-local now: the members stay what the host set. */
    test_assert(brush.mu == 0.0f && brush.nu == 0.9f);
  }

  /* pinch/sharp share `pinch` @72. */
  {
    Brush brush;
    brush.pinch = 0.35f;
    pack(brush, SculptBrushes::PINCH, bu);
    test_assert(readF(bu, 72) == 0.35f);
    pack(brush, SculptBrushes::SHARP, bu);
    test_assert(readF(bu, 72) == 0.35f);
  }

  /* Clay family: planeoff @72, planeSide @76, same for all three tools. */
  {
    Brush brush;
    brush.planeoff = -0.2f;
    brush.planeSide = -1.0f;
    for (SculptBrushes tool :
         {SculptBrushes::CLAY, SculptBrushes::SCRAPE, SculptBrushes::FILL}) {
      pack(brush, tool, bu);
      test_assert(readF(bu, 72) == -0.2f);
      test_assert(readF(bu, 76) == -1.0f);
    }
  }

  /* polygroup `activeGroup` i32 @72 — no more bit-reinterpret through `mu`. */
  {
    Brush brush;
    brush.activeGroup = 42;
    pack(brush, SculptBrushes::POLYGROUP, bu);
    test_assert(readI(bu, 72) == 42);
  }

  /* A kernel that appends nothing leaves the whole region zeroed. */
  {
    Brush brush;
    brush.pinch = 0.9f; // set but undeclared by draw — must not leak
    pack(brush, SculptBrushes::DRAW, bu);
    for (int off = 72; off < 112; off += 4) {
      test_assert(readI(bu, off) == 0);
    }
  }

  /* The dispatch is total over built-in ids and null past them. */
  {
    for (int id = 0; id < builtinBrushCount; id++) {
      test_assert(command::builtinBrushGpuPack(id) != nullptr);
    }
    test_assert(command::builtinBrushGpuPack(builtinBrushCount) == nullptr);
    test_assert(command::builtinBrushGpuPack(-1) == nullptr);
  }
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  runTests();
  return test_end();
}
