// Golden {name -> id} table for the built-in SculptBrushes enum.
//
// The ids are not derivable from anything (not kernel file order, not the
// alphabet) and they are persisted: a saved brush asset, a host bridge and the
// extras registry all key off the integer. Codegen (grid-domain-attributes P0b)
// emits the Binder item list and the id-keyed dispatchers from a table, so this
// pins the pairing itself — a reorder that keeps the enum compiling still fails
// here.
#include "test_util.h"
#include <cstdio>
#include <cstring>

#include "binding/binding.h"
#include "brush/brushes/types.h"
#include "brush/props.h"

test_init;

struct ToolRow {
  const char *name;
  int id;
};

// Order is the id order. Append only; never renumber.
static const ToolRow kTools[] = {
    {"DRAW", 0},         {"INFLATE", 1},   {"CLAY", 2},        {"PINCH", 3},
    {"SHARP", 4},        {"MASK", 5},      {"SMOOTH", 6},      {"KELVINLET", 7},
    {"POSE", 8},         {"TEXDRAW", 9},   {"SCRAPE", 10},     {"FILL", 11},
    {"WINGSCRAPE", 12},  {"COLOR", 13},    {"POLYGROUP", 14},  {"BSMOOTH", 15},
    {"GRAB", 16},        {"SNAKEHOOK", 17}, {"COLORSMOOTH", 18}, {"FEATURE_ALIGN", 19},
    {"LAYERDRAW", 20},   {"ENHANCE", 21},  {"TEXGRAD", 22},
};

static void runTests()
{
  using namespace sculptcore::brush;
  using namespace litestl::binding;

  {
    static_assert(validate_prop("strength"));
    static_assert(!validate_prop("sdfsf"));

    BrushProps bp;
    float f = bp.lookup_prop<"strength", float>();
    (void)f;
  }

  constexpr int count = int(sizeof(kTools) / sizeof(kTools[0]));
  test_assert(count == SculptBrushesBuiltinCount);

  // The reflected enum is what hosts and the extras registry resolve names
  // through, so the golden is asserted against the binding, not the C++ enum.
  const types::Enum *e;
  {
    litestl::alloc::PermanentGuard guard;
    e = Binder<SculptBrushes>::bind();
  }
  test_assert(e != nullptr);
  if (!e) {
    return;
  }

  // The built-ins occupy the first `count` items. A build configured with extra
  // kernel dirs appends one item per extra kernel after them — their ids follow
  // the built-in count, which is exactly what this pairing has to keep true — so
  // only the no-extras build can pin the total (test_brush_metadata.cc grades the
  // extras themselves, and it takes the whole brush lib to count them here).
  if (int(e->items.size()) < count) {
    std::printf("SculptBrushes has %d bound items, fewer than the %d built-ins\n",
                int(e->items.size()), count);
  }
  test_assert(int(e->items.size()) >= count);
#ifndef SCULPTCORE_EXTRA_BRUSHES
  if (int(e->items.size()) != count) {
    std::printf("SculptBrushes has %d bound items, golden has %d\n",
                int(e->items.size()), count);
  }
  test_assert(int(e->items.size()) == count);
#endif

  for (int i = 0; i < count && i < int(e->items.size()); i++) {
    const auto &item = e->items[i];
    if (std::strcmp(item.name.c_str(), kTools[i].name) != 0 || item.value != kTools[i].id) {
      std::printf("item %d: bound {%s, %d}, golden {%s, %d}\n",
                  i, item.name.c_str(), item.value, kTools[i].name, kTools[i].id);
    }
    test_assert(std::strcmp(item.name.c_str(), kTools[i].name) == 0);
    test_assert(item.value == kTools[i].id);
  }
}

int main()
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  runTests();
  return test_end();
}
