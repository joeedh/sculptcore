// SPDX-FileCopyrightText: 2026 Blender Authors
//
// SPDX-License-Identifier: GPL-2.0-or-later

// The `brushNeedsLiveLinks` truth table (grid-domain-attributes P0c).
//
// The predicate decides whether a stroke may run topology-frozen, i.e. whether
// the mesh's disk/radial link pages are dropped for the stroke's duration. A
// false positive costs a thaw; a false negative is a kernel walking freed pages
// — a heap-layout-dependent use-after-free that no other test here covers.
//
// So the answer is pinned as a literal per-tool table rather than as a
// re-expression of the predicate's formula: the table is written down once, from
// the behaviour of the hand-written disjunction the predicate started as, and
// stays as the independent statement every later version is graded against.
// It must keep passing when the predicate is rederived from kernel metadata,
// which is the whole point of writing it before that change.
#include "test_util.h"

#include "brush/brush_executor.h"
#include "brush/brushes/all.h"
#include "brush/brushes/types.h"

#include <cstdio>
#include <cstring>

test_init;

using namespace sculptcore::brush;

/** When a tool needs live topology links. */
enum class Links {
  Never,      // topology can be frozen safely for the whole stroke
  NonCsrOnly, // has a for_neighbor loop: needs the links unless neighbors come from CSR
  Always,     // walks live links every dab regardless of the neighbor source
};

struct Row {
  SculptBrushes tool;
  const char *name;
  Links links;
};

// All 23 built-in ids, in id order. Rationale for the non-Never rows:
//   SMOOTH/BSMOOTH/COLORSMOOTH  for_neighbor loops; the CSR instantiation reads
//                               the ring1 cache instead of the live disk.
//   POLYGROUP                   face-stage kernel — dispatched per face, so it
//                               walks the face loop to reach its verts.
//   FEATURE_ALIGN               cross-field pre-pass walks the vertex rings
//                               every dab (and it is @fulltopo, so its
//                               for_neighbor loop is always the live-disk one).
//   ENHANCE                     difference-of-smooths pre-pass, same reason.
static const Row kTable[] = {
    {SculptBrushes::DRAW, "DRAW", Links::Never},
    {SculptBrushes::INFLATE, "INFLATE", Links::Never},
    {SculptBrushes::CLAY, "CLAY", Links::Never},
    {SculptBrushes::PINCH, "PINCH", Links::Never},
    {SculptBrushes::SHARP, "SHARP", Links::Never},
    {SculptBrushes::MASK, "MASK", Links::Never},
    {SculptBrushes::SMOOTH, "SMOOTH", Links::NonCsrOnly},
    {SculptBrushes::KELVINLET, "KELVINLET", Links::Never},
    {SculptBrushes::POSE, "POSE", Links::Never},
    {SculptBrushes::TEXDRAW, "TEXDRAW", Links::Never},
    {SculptBrushes::SCRAPE, "SCRAPE", Links::Never},
    {SculptBrushes::FILL, "FILL", Links::Never},
    {SculptBrushes::WINGSCRAPE, "WINGSCRAPE", Links::Never},
    {SculptBrushes::COLOR, "COLOR", Links::Never},
    {SculptBrushes::POLYGROUP, "POLYGROUP", Links::Always},
    {SculptBrushes::BSMOOTH, "BSMOOTH", Links::NonCsrOnly},
    {SculptBrushes::GRAB, "GRAB", Links::Never},
    {SculptBrushes::SNAKEHOOK, "SNAKEHOOK", Links::Never},
    {SculptBrushes::COLORSMOOTH, "COLORSMOOTH", Links::NonCsrOnly},
    {SculptBrushes::FEATURE_ALIGN, "FEATURE_ALIGN", Links::Always},
    {SculptBrushes::LAYERDRAW, "LAYERDRAW", Links::Never},
    {SculptBrushes::ENHANCE, "ENHANCE", Links::Always},
    {SculptBrushes::TEXGRAD, "TEXGRAD", Links::Never},
};

static bool expected(Links links, bool csr)
{
  switch (links) {
  case Links::Never:
    return false;
  case Links::NonCsrOnly:
    return !csr;
  case Links::Always:
    return true;
  }
  return false;
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);

  // The table covers the whole enum: a brush appended to tools.txt without a row
  // here would otherwise be silently ungraded.
  test_assert(int(sizeof(kTable) / sizeof(kTable[0])) == SculptBrushesBuiltinCount);

  // brushNeedsLiveLinks reads only `neighborMode`, so a tree-less, brush-less
  // executor answers for it.
  CommandExecutor ex(nullptr, nullptr);

  for (const Row &row : kTable) {
    for (int mode = 0; mode < 2; mode++) {
      const bool csr = mode == int(CommandExecutor::NeighborMode::Csr);
      ex.setNeighborMode(mode);
      const bool got = ex.brushNeedsLiveLinks(row.tool);
      const bool want = expected(row.links, csr);
      if (got != want) {
        std::printf("%s (id %d), neighborMode=%s: needsLiveLinks=%d, table says %d\n",
                    row.name,
                    int(row.tool),
                    csr ? "Csr" : "LiveDisk",
                    int(got),
                    int(want));
      }
      test_assert(got == want);
    }
  }

  // Every id in the table is the id the enum reflects under that name — the
  // table is keyed by name in the source and by id in the predicate, and a
  // renumbered tools.txt would otherwise shift the rows silently.
  for (int id = 0; id < SculptBrushesBuiltinCount; id++) {
    test_assert(int(kTable[id].tool) == id);
    test_assert(std::strcmp(kTable[id].name, kBuiltinBrushNames[id]) == 0);
  }

  return test_end();
}
