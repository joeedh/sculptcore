// The generated built-in brush dispatch (grid-domain-attributes P0b).
//
// `createBuiltinBrush` (brushes/generated/builtin_brushes.gen.h) replaces the
// hand-written 23-case switch that used to live in CommandExecutor. The roster
// below is that switch, transcribed once, and it stays here permanently: it is
// the independent statement of "which kernel, and against which neighbor
// source" that the generator's output is graded against. Regenerating from a
// broken @tool annotation, or renumbering tools.txt, lands as a mismatch here
// rather than as a brush that silently runs the wrong kernel.
//
// Identity is compared through std::function::target_type() — RTTI is on, so
// that is the exact closure type, which distinguishes createSmoothBrush's
// CsrNbr instantiation from its LiveDiskNbr one. Comparing the manifest alone
// would not: the two differ only in the callable.
#include "test_util.h"

#include "brush/accum_mode.h"
#include "brush/brush_executor.h"
#include "brush/brushes/all.h"
#include "brush/brushes/types.h"
#include "brush/neighbor_source.h"

#include <cstdio>
#include <cstring>
#include <typeinfo>

test_init;

using namespace sculptcore::brush;

using Exec = CommandExecutor;
using Def = BrushCommandDef<CommandCtx<Exec>>;

// The reference roster: the pre-P0b hand-written switch, verbatim in intent.
// Returns false for an id no built-in claims (which the generated dispatcher
// must agree about — that is the extras fallthrough).
template <class AccMode> static bool referenceBrush(SculptBrushes tool, bool csrNeighbors, Def &def)
{
  using namespace sculptcore::brush::command;

  switch (tool) {
  case SculptBrushes::DRAW:
    createDrawBrush<Exec, AccMode>(def);
    return true;
  case SculptBrushes::INFLATE:
    createInflateBrush<Exec, AccMode>(def);
    return true;
  case SculptBrushes::CLAY:
  case SculptBrushes::SCRAPE:
  case SculptBrushes::FILL:
    createPlaneBrush<Exec, AccMode>(def);
    return true;
  case SculptBrushes::WINGSCRAPE:
    createWingscrapeBrush<Exec, AccMode>(def);
    return true;
  case SculptBrushes::PINCH:
    createPinchBrush<Exec, AccMode>(def);
    return true;
  case SculptBrushes::SHARP:
    createSharpBrush<Exec, AccMode>(def);
    return true;
  case SculptBrushes::MASK:
    createMaskBrush<Exec, AccMode>(def);
    return true;
  case SculptBrushes::SMOOTH:
    if (csrNeighbors) {
      createSmoothBrush<Exec, CsrNbr, AccMode>(def);
    } else {
      createSmoothBrush<Exec, LiveDiskNbr, AccMode>(def);
    }
    return true;
  case SculptBrushes::KELVINLET:
    createKelvinletBrush<Exec, AccMode>(def);
    return true;
  case SculptBrushes::GRAB:
    createGrabBrush<Exec, AccMode>(def);
    return true;
  case SculptBrushes::SNAKEHOOK:
    createSnakehookBrush<Exec, AccMode>(def);
    return true;
  case SculptBrushes::POSE:
    createPoseBrush<Exec, AccMode>(def);
    return true;
  case SculptBrushes::TEXDRAW:
    createTexdrawBrush<Exec, AccMode>(def);
    return true;
  case SculptBrushes::COLOR:
    createColorBrush<Exec, AccMode>(def);
    return true;
  case SculptBrushes::POLYGROUP:
    createPolygroupBrush<Exec, AccMode>(def);
    return true;
  case SculptBrushes::BSMOOTH:
    if (csrNeighbors) {
      createBsmoothBrush<Exec, CsrNbr, AccMode>(def);
    } else {
      createBsmoothBrush<Exec, LiveDiskNbr, AccMode>(def);
    }
    return true;
  case SculptBrushes::COLORSMOOTH:
    if (csrNeighbors) {
      createColorsmoothBrush<Exec, CsrNbr, AccMode>(def);
    } else {
      createColorsmoothBrush<Exec, LiveDiskNbr, AccMode>(def);
    }
    return true;
  case SculptBrushes::FEATURE_ALIGN:
    // Always live-disk: the cross-field pre-pass walks the vertex disk, so
    // topology is thawed regardless of the stroke's neighbor mode. This is what
    // the kernel's @fulltopo annotation has to reproduce.
    createFeaturealignBrush<Exec, LiveDiskNbr, AccMode>(def);
    return true;
  case SculptBrushes::LAYERDRAW:
    createLayerdrawBrush<Exec, AccMode>(def);
    return true;
  case SculptBrushes::ENHANCE:
    // @fulltopo but not a for_neighbor kernel — it applies the host pre-pass's
    // cached .brush.enhance.disp, so it takes no neighbor-source template.
    createEnhanceBrush<Exec, AccMode>(def);
    return true;
  case SculptBrushes::TEXGRAD:
    createTexgradBrush<Exec, AccMode>(def);
    return true;
  default:
    return false;
  }
}

/** The stored callable as an address, when it is a plain function pointer (which
 * every codegen'd stage is). Null for an empty std::function or a stateful
 * callable — target_type() carries the identity in that case. */
template <class Sig> static const void *stageAddr(const std::function<Sig> &f)
{
  if (!f) {
    return nullptr;
  }
  auto *p = f.template target<Sig *>();
  return p ? reinterpret_cast<const void *>(*p) : nullptr;
}

template <class Sig>
static bool sameStage(const char *what, int id, bool csr, const char *mode,
                      const std::function<Sig> &want, const std::function<Sig> &got)
{
  if (bool(want) != bool(got)) {
    std::printf("id %d (%s) csr=%d %s: %s presence mismatch (want %d, got %d)\n", id,
                kBuiltinBrushNames[id], int(csr), mode, what, int(bool(want)), int(bool(got)));
    return false;
  }
  if (!want) {
    return true;
  }
  if (want.target_type() != got.target_type()) {
    std::printf("id %d (%s) csr=%d %s: %s type mismatch: want %s, got %s\n", id,
                kBuiltinBrushNames[id], int(csr), mode, what, want.target_type().name(),
                got.target_type().name());
    return false;
  }
  // The type alone does not separate two instantiations of the same kernel: both
  // store the same `void(*)(Ctx&)`, so smooth<CsrNbr> and smooth<LiveDiskNbr> are
  // indistinguishable by target_type(). The address is what pins the kernel.
  const void *wa = stageAddr(want), *ga = stageAddr(got);
  if (wa != ga) {
    std::printf("id %d (%s) csr=%d %s: %s target mismatch (%p vs %p)\n", id,
                kBuiltinBrushNames[id], int(csr), mode, what, wa, ga);
    return false;
  }
  return true;
}

template <class AccMode> static void compareRoster(const char *mode)
{
  for (int id = 0; id < SculptBrushesBuiltinCount; id++) {
    for (int csr = 0; csr < 2; csr++) {
      Def want, got;
      const bool wantHandled = referenceBrush<AccMode>(SculptBrushes(id), csr != 0, want);
      const bool gotHandled =
          command::createBuiltinBrush<Exec, CsrNbr, LiveDiskNbr, AccMode>(id, csr != 0, got);

      test_assert(wantHandled);
      test_assert(gotHandled == wantHandled);
      if (!wantHandled || !gotHandled) {
        continue;
      }

      test_assert(sameStage("exec", id, csr != 0, mode, want.exec, got.exec));
      test_assert(sameStage("execPre", id, csr != 0, mode, want.execPre, got.execPre));
      test_assert(sameStage("execPost", id, csr != 0, mode, want.execPost, got.execPost));
      test_assert(sameStage("execHost", id, csr != 0, mode, want.execHost, got.execHost));

      // The rest of the def is codegen'd alongside the callable, so a wrong
      // kernel would usually change these too — cheap corroboration that the
      // whole manifest, not just the entry point, came from the same kernel.
      test_assert(want.flags == got.flags);
      test_assert(want.accumulable == got.accumulable);
      test_assert(want.needsCoPrev == got.needsCoPrev);
      test_assert(want.faceMode == got.faceMode);
      test_assert(want.uniforms.size() == got.uniforms.size());
      test_assert(want.attrs.size() == got.attrs.size());
    }
  }
}

static void runTests()
{
  test_assert(builtinBrushCount == SculptBrushesBuiltinCount);

  compareRoster<AccumLive>("AccumLive");
  compareRoster<AccumOrig>("AccumOrig");

  // Proof the comparison above has teeth along the neighbor-source axis: if it
  // could not tell CsrNbr from LiveDiskNbr, every csr=0/csr=1 pair would compare
  // equal and half the roster check would be vacuous.
  {
    Def csr, live;
    command::createBuiltinBrush<Exec, CsrNbr, LiveDiskNbr, AccumLive>(
        int(SculptBrushes::SMOOTH), true, csr);
    command::createBuiltinBrush<Exec, CsrNbr, LiveDiskNbr, AccumLive>(
        int(SculptBrushes::SMOOTH), false, live);
    test_assert(stageAddr(csr.exec) != nullptr);
    test_assert(stageAddr(csr.exec) != stageAddr(live.exec));
  }

  // The two reflected topology facts. `needsCoPrev` is the codegen's own
  // for_neighbor answer, so it pins builtinBrushUsesForNeighbor against the
  // emitted kernel rather than against a second reading of the .sbrush.
  for (int id = 0; id < SculptBrushesBuiltinCount; id++) {
    Def def;
    const bool handled =
        command::createBuiltinBrush<Exec, CsrNbr, LiveDiskNbr, AccumLive>(id, true, def);
    test_assert(handled);
    if (builtinBrushUsesForNeighbor(id) != def.needsCoPrev) {
      std::printf("id %d (%s): usesForNeighbor=%d but needsCoPrev=%d\n", id,
                  kBuiltinBrushNames[id], int(builtinBrushUsesForNeighbor(id)),
                  int(def.needsCoPrev));
    }
    test_assert(builtinBrushUsesForNeighbor(id) == def.needsCoPrev);
  }

  // @fulltopo is a host-pre-pass property, so it has no codegen'd counterpart to
  // cross-check — it is asserted as a golden. A kernel gaining or losing the
  // annotation must be a deliberate edit here, because getting it wrong on a
  // dyntopo stroke is a use-after-free, not a wrong pixel.
  for (int id = 0; id < SculptBrushesBuiltinCount; id++) {
    const bool want = id == int(SculptBrushes::FEATURE_ALIGN) ||
                      id == int(SculptBrushes::ENHANCE);
    if (builtinBrushFullTopo(id) != want) {
      std::printf("id %d (%s): fullTopo=%d, golden %d\n", id, kBuiltinBrushNames[id],
                  int(builtinBrushFullTopo(id)), int(want));
    }
    test_assert(builtinBrushFullTopo(id) == want);
  }

  // faceMode does have a codegen'd counterpart (the per-kernel def), so cross-check
  // the two rather than only pinning a golden: brushNeedsLiveLinks reads the cheap
  // id-keyed form per dab, and it must agree with the def the dispatch builds.
  for (int id = 0; id < SculptBrushesBuiltinCount; id++) {
    Def def;
    const bool handled =
        command::createBuiltinBrush<Exec, CsrNbr, LiveDiskNbr, AccumLive>(id, true, def);
    test_assert(handled);
    if (builtinBrushFaceMode(id) != def.faceMode) {
      std::printf("id %d (%s): faceMode=%d but def.faceMode=%d\n", id, kBuiltinBrushNames[id],
                  int(builtinBrushFaceMode(id)), int(def.faceMode));
    }
    test_assert(builtinBrushFaceMode(id) == def.faceMode);
    test_assert(builtinBrushFaceMode(id) == (id == int(SculptBrushes::POLYGROUP)));
  }

  // The kernel a tool dispatches to, as a name — the readable half of the same
  // pairing, and what a mis-annotated @tool shows up as first.
  test_assert(std::strcmp(kBuiltinBrushKernels[int(SculptBrushes::CLAY)], "plane") == 0);
  test_assert(std::strcmp(kBuiltinBrushKernels[int(SculptBrushes::SCRAPE)], "plane") == 0);
  test_assert(std::strcmp(kBuiltinBrushKernels[int(SculptBrushes::FILL)], "plane") == 0);
  test_assert(std::strcmp(kBuiltinBrushKernels[int(SculptBrushes::FEATURE_ALIGN)],
                          "featurealign") == 0);
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  runTests();
  return test_end();
}
