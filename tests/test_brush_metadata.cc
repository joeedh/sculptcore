// The runtime half of the brush metadata surface (grid-domain-attributes P0a).
//
// test_sbrush_attr_writes.cc pins what the *compiler* infers; this pins what a
// host actually reads back, which is the thing a capability rule is keyed on:
//
//  1. Every attr entry has materialize == true (it only means "ensure the layer
//     exists"), and kernelWrites carries the real per-kernel answer.
//  2. brushAttrManifestFor writes into caller-owned storage, so two interleaved
//     queries do not clobber each other — BrushMetadata's bound methods park the
//     result on the query object because the binding runtime cannot marshal a
//     Vector, and engine code must not inherit that.
//  3. buildBrushDef reports a manifest for an extra (out-of-repo) kernel. It
//     passed brushOrNull = nullptr, and the extras branch needs a live Brush, so
//     every extra kernel reported unhandled — i.e. an empty manifest for exactly
//     the kernels the capability rule exists to serve. Only checkable in a build
//     configured with SCULPTCORE_EXTRA_KERNEL_DIRS (e.g. the addon's brushes/);
//     vacuous otherwise.
#include "test_util.h"

#include "brush/brush_executor.h"
#include "brush/brushes/all.h"
#include "brush/brushes/types.h"

#include <cstdio>

test_init;

using namespace sculptcore::brush;

struct GoldenRow {
  SculptBrushes tool;
  const char *toolName;
  const char *attr;
  bool kernelWrites;
};

// §4.1 of claudeMemory/plans/grid-domain-attributes.md, as the runtime sees it.
static const GoldenRow kGolden[] = {
    {SculptBrushes::BSMOOTH, "BSMOOTH", "vclass", false},
    {SculptBrushes::FEATURE_ALIGN, "FEATURE_ALIGN", "field", false},
    {SculptBrushes::FEATURE_ALIGN, "FEATURE_ALIGN", "vclass", false},
    {SculptBrushes::ENHANCE, "ENHANCE", "edisp", false},
    {SculptBrushes::COLOR, "COLOR", "color", true},
    {SculptBrushes::COLORSMOOTH, "COLORSMOOTH", "color", true},
    {SculptBrushes::LAYERDRAW, "LAYERDRAW", "slayer", true},
    {SculptBrushes::POLYGROUP, "POLYGROUP", "group", true},
};

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  {
    litestl::util::Vector<BrushAttrManifestEntry> manifest;
    for (const auto &row : kGolden) {
      test_assert(brushAttrManifestFor(row.tool, manifest));
      const BrushAttrManifestEntry *e = findBrushAttrEntry(manifest, row.attr);
      if (!e) {
        std::printf("%s: no manifest entry for attr '%s' (%d entries)\n",
                    row.toolName, row.attr, int(manifest.size()));
      }
      test_assert(e != nullptr);
      if (!e) {
        continue;
      }
      test_assert(e->materialize);
      if (e->kernelWrites != row.kernelWrites) {
        std::printf("%s.%s: kernelWrites %d, expected %d\n",
                    row.toolName, row.attr, int(e->kernelWrites),
                    int(row.kernelWrites));
      }
      test_assert(e->kernelWrites == row.kernelWrites);
    }
  }

  // Two live manifests at once: the read-only vclass entry must not be
  // overwritten by a later query for a painting kernel.
  {
    litestl::util::Vector<BrushAttrManifestEntry> a, b;
    test_assert(brushAttrManifestFor(SculptBrushes::BSMOOTH, a));
    test_assert(brushAttrManifestFor(SculptBrushes::COLOR, b));
    const BrushAttrManifestEntry *vclass = findBrushAttrEntry(a, "vclass");
    const BrushAttrManifestEntry *color = findBrushAttrEntry(b, "color");
    test_assert(vclass && !vclass->kernelWrites);
    test_assert(color && color->kernelWrites);
  }

  // The two derived flags that read the bit.
  {
    BrushDefFlags bsmooth = brushDefFlagsFor(SculptBrushes::BSMOOTH);
    test_assert(bsmooth.readsVclass);
    test_assert(!bsmooth.writesColor);
    BrushDefFlags color = brushDefFlagsFor(SculptBrushes::COLOR);
    test_assert(color.writesColor);
    BrushDefFlags polygroup = brushDefFlagsFor(SculptBrushes::POLYGROUP);
    test_assert(polygroup.faceMode);
    test_assert(!brushDefFlagsFor(SculptBrushes::DRAW).faceMode);
  }

  // A kernel with no attrs still reports a def; an id past the extras range
  // reports unhandled.
  {
    litestl::util::Vector<BrushAttrManifestEntry> manifest;
    test_assert(brushAttrManifestFor(SculptBrushes::DRAW, manifest));
    test_assert(manifest.size() == 0);
    test_assert(!brushAttrManifestFor(
        static_cast<SculptBrushes>(SculptBrushesBuiltinCount + extraBrushCount + 4),
        manifest));
  }

  // Gate 3. Vacuous without configured extra kernel dirs.
  if (extraBrushCount == 0) {
    std::printf("no extra kernels configured — extras manifest gate is vacuous "
                "(configure with -DSCULPTCORE_EXTRA_KERNEL_DIRS=<dir>)\n");
  }
  for (int i = 0; i < extraBrushCount; i++) {
    SculptBrushes id = static_cast<SculptBrushes>(SculptBrushesBuiltinCount + i);
    CommandExecutor::brush_command def;
    bool handled = buildBrushDef(id, def);
    if (!handled) {
      std::printf("extra kernel %d reported unhandled — buildBrushDef is not "
                  "passing a Brush\n",
                  i);
    }
    test_assert(handled);
    // Every extra kernel reaching the named-float store declares at least one
    // uniform; an empty manifest here means the def was never filled.
    test_assert(def.uniforms.size() > 0);
  }

  return test_end();
}
